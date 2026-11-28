//! What the agent's actions turn into on Windows: processes, HTTP, files, timers.
//!
//! Processes. Each command is `bash -c COMMAND` in Git Bash, started suspended, put in a job object
//! of its own (which kills everything in it when its handle closes), and then let go. That is
//! Windows' process group: stopping a command stops whatever it started too. Commands get a
//! console of their own with no window (CREATE_NO_WINDOW), so programs they run open no window
//! and the user's Ctrl-C reaches barb, not them. Only the handles meant for the child are
//! inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST). There is no SIGTERM: a command that is stopped
//! or runs out of time is killed at once.
//!
//! Output comes through a named pipe read with overlapped I/O, so the main loop can sleep in
//! WaitForMultipleObjects on every pipe and process at once and needs no polling at all: a
//! background task that runs for an hour costs nothing until it exits.
//!
//! Every operation ends in exactly one event pushed on the queue (or none, if it was cancelled).

use super::sys::*;
use crate::common::{self, Ev, Output, HTTP_MAX};
use barb_core::agent::{Exit, Fs};
use std::collections::VecDeque;
use std::ffi::{c_void, OsString};
use std::fs::{self, OpenOptions};
use std::io::{Read, Write};
use std::os::windows::ffi::OsStringExt;
use std::os::windows::fs::OpenOptionsExt;
use std::os::windows::io::AsRawHandle;
use std::path::{Path, PathBuf};
use std::ptr::{null, null_mut};
use std::sync::atomic::{AtomicU32, Ordering};

const MAX_OPS: usize = 64;
const MAX_PARKED: usize = 64;
const READ_LIMIT: u64 = 8 << 20;
const RBUF: usize = 65536;
const DRAIN_MS: i64 = 5000; // the longest finish() waits for the last of the output
const MAX_PATH: usize = 260;

static PIPE_SERIAL: AtomicU32 = AtomicU32::new(0);

#[derive(PartialEq, Clone, Copy)]
enum Kind {
    Proc,
    Http,
    Timer,
}

struct Op {
    kind: Kind,
    id: i64,
    process: HANDLE,
    job: HANDLE, // 0 if Windows refused one
    pipe: HANDLE, // the read end of the output, or 0
    ov: Box<Overlapped>, // where it is, not just what, matters while a read is in flight
    rbuf: Vec<u8>,
    reading: bool,
    bg: bool,
    exited: bool,
    killed: bool,
    code: u32,
    cancelled: bool,
    timed_out: bool,
    started: i64,
    deadline: i64, // ms; 0 = none
    out: Output,
    body: Vec<u8>,
    tmpfile: Option<Vec<u16>>, // NUL-terminated
}

fn inheritable() -> SecurityAttributes {
    SecurityAttributes { length: std::mem::size_of::<SecurityAttributes>() as u32, descriptor: null_mut(), inherit: 1 }
}

impl Op {
    fn new(kind: Kind, id: i64) -> Op {
        Op {
            kind,
            id,
            process: 0,
            job: 0,
            pipe: 0,
            ov: Box::new(Overlapped { internal: 0, internal_high: 0, offset: 0, offset_high: 0, event: 0 }),
            rbuf: Vec::new(),
            reading: false,
            bg: false,
            exited: false,
            killed: false,
            code: 0,
            cancelled: false,
            timed_out: false,
            started: now(),
            deadline: 0,
            out: Output::default(),
            body: Vec::new(),
            tmpfile: None,
        }
    }

    fn close_pipe(&mut self) {
        if self.pipe == 0 {
            return;
        }
        if self.reading {
            // the buffer must outlive the read
            let mut got = 0;
            // SAFETY: the pipe and the OVERLAPPED of the read in flight are ours and alive.
            unsafe {
                CancelIoEx(self.pipe, &mut *self.ov);
                GetOverlappedResult(self.pipe, &mut *self.ov, &mut got, 1);
            }
            self.reading = false;
        }
        close(self.pipe);
        self.pipe = 0;
    }

    fn kill(&mut self) {
        if self.exited {
            return;
        }
        self.killed = true;
        // SAFETY: the handles are ours and open.
        unsafe {
            if self.job != 0 {
                TerminateJobObject(self.job, 1);
            } else if self.process != 0 {
                TerminateProcess(self.process, 1);
            }
        }
    }

    /// Collect what has arrived and keep a read in flight, until the pipe is empty or closed.
    fn pump(&mut self) {
        for _ in 0..64 {
            if self.pipe == 0 {
                return;
            }
            if self.reading {
                let mut got = 0u32;
                // SAFETY: the read was started on this pipe with this OVERLAPPED; no waiting.
                if unsafe { GetOverlappedResult(self.pipe, &mut *self.ov, &mut got, 0) } == 0 {
                    if last_error() == ERROR_IO_INCOMPLETE {
                        return;
                    }
                    self.reading = false;
                    self.close_pipe(); // the end of the output (every writer has closed it)
                    return;
                }
                self.reading = false;
                let data = &self.rbuf[..got as usize];
                if self.kind == Kind::Proc {
                    self.out.add(data);
                } else {
                    let room = HTTP_MAX.saturating_sub(self.body.len()).min(data.len());
                    self.body.extend_from_slice(&data[..room]);
                }
            }
            let ev = self.ov.event;
            *self.ov = Overlapped { internal: 0, internal_high: 0, offset: 0, offset_high: 0, event: ev };
            // SAFETY: rbuf and the boxed OVERLAPPED stay where they are until the read is over
            // (close_pipe waits for it).
            let ok = unsafe { ReadFile(self.pipe, self.rbuf.as_mut_ptr(), RBUF as u32, null_mut(), &mut *self.ov) };
            if ok != 0 || last_error() == ERROR_IO_PENDING {
                self.reading = true; // done at once or later: GetOverlappedResult tells
                continue;
            }
            self.close_pipe();
        }
    }

    /// After the process has gone (and taken everything it started with it), the rest of its output.
    fn drain_all(&mut self) {
        let until = now() + DRAIN_MS;
        while self.pipe != 0 {
            self.pump();
            if self.pipe == 0 {
                return;
            }
            let left = until - now();
            // SAFETY: the event is ours.
            if left <= 0 || unsafe { WaitForSingleObject(self.ov.event, left as u32) } == WAIT_TIMEOUT {
                self.close_pipe();
                return;
            }
        }
    }

    /// A pipe for the child's output: an overlapped read end for barb, and the inheritable write
    /// end for the child, which is returned.
    fn make_pipe(&mut self) -> Result<HANDLE, u32> {
        // SAFETY: no arguments.
        let pid = unsafe { GetCurrentProcessId() };
        let name = wide(format!(r"\\.\pipe\barb-{}-{}", pid, PIPE_SERIAL.fetch_add(1, Ordering::Relaxed) + 1));
        // SAFETY: name is NUL-terminated; the rest are plain values.
        let p = unsafe {
            CreateNamedPipeW(
                name.as_ptr(),
                PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                0,
                RBUF as u32,
                0,
                null_mut(),
            )
        };
        if p == INVALID_HANDLE_VALUE {
            return Err(last_error());
        }
        self.pipe = p;
        // FILE_READ_ATTRIBUTES: Git Bash's runtime looks at what kind of handle it was given.
        let mut sa = inheritable();
        // SAFETY: name is NUL-terminated and sa lives through the call.
        let child =
            unsafe { CreateFileW(name.as_ptr(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, &mut sa, OPEN_EXISTING, 0, 0) };
        if child == INVALID_HANDLE_VALUE {
            let e = last_error();
            close(self.pipe);
            self.pipe = 0;
            return Err(e);
        }
        // SAFETY: plain values; an unnamed manual-reset event.
        self.ov.event = unsafe { CreateEventW(null_mut(), 1, 0, null()) };
        if self.ov.event == 0 {
            let e = last_error();
            close(child);
            return Err(e);
        }
        self.rbuf = vec![0; RBUF];
        Ok(child)
    }

    /// Start `cmdline` with `app` in a job of its own, its standard input from `inp` and its
    /// output to `out` (both inheritable).
    fn start(&mut self, app: &[u16], cmdline: &mut [u16], inp: HANDLE, out: HANDLE) -> Result<(), u32> {
        let mut size = 0usize;
        // SAFETY: the first call only asks for the size.
        unsafe { InitializeProcThreadAttributeList(null_mut(), 1, 0, &mut size) };
        let mut attrs = vec![0usize; size / std::mem::size_of::<usize>() + 1];
        let list_ptr = attrs.as_mut_ptr() as *mut c_void;
        // SAFETY: attrs has room for `size` bytes, aligned for pointers.
        if unsafe { InitializeProcThreadAttributeList(list_ptr, 1, 0, &mut size) } == 0 {
            return Err(ERROR_OUTOFMEMORY);
        }
        let mut list = [inp, out];
        let n = if inp == out { 1 } else { 2 };
        // SAFETY: list outlives CreateProcessW below, which is the only reader of the attribute.
        unsafe {
            UpdateProcThreadAttribute(
                list_ptr,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                list.as_mut_ptr() as *mut c_void,
                n * std::mem::size_of::<HANDLE>(),
                null_mut(),
                null_mut(),
            )
        };
        // SAFETY: an all-zero STARTUPINFOEXW is valid (null pointers, zero handles).
        let mut si: StartupInfoExW = unsafe { std::mem::zeroed() };
        si.si.cb = std::mem::size_of::<StartupInfoExW>() as u32;
        si.si.flags = STARTF_USESTDHANDLES;
        si.si.std_input = inp;
        si.si.std_output = out;
        si.si.std_error = out;
        si.attrs = list_ptr;
        let mut pi = ProcessInformation { process: 0, thread: 0, pid: 0, tid: 0 };
        // SAFETY: app and cmdline are NUL-terminated; cmdline is writable as CreateProcessW wants.
        let ok = unsafe {
            CreateProcessW(
                app.as_ptr(),
                cmdline.as_mut_ptr(),
                null_mut(),
                null_mut(),
                1,
                CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                null_mut(),
                null(),
                &mut si.si,
                &mut pi,
            )
        };
        let err = last_error();
        // SAFETY: the list was initialised above.
        unsafe { DeleteProcThreadAttributeList(list_ptr) };
        if ok == 0 {
            return Err(err);
        }
        // SAFETY: plain values and handles just made.
        unsafe {
            self.job = CreateJobObjectW(null_mut(), null());
            if self.job != 0 {
                let mut li = JobExtendedLimits::default();
                li.basic.flags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if SetInformationJobObject(
                    self.job,
                    JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                    &mut li as *mut _ as *mut c_void,
                    std::mem::size_of::<JobExtendedLimits>() as u32,
                ) == 0
                    || AssignProcessToJobObject(self.job, pi.process) == 0
                {
                    close(self.job); // run it anyway; stopping it then stops only the process itself
                    self.job = 0;
                }
            }
            ResumeThread(pi.thread);
        }
        close(pi.thread);
        self.process = pi.process;
        Ok(())
    }
}

impl Drop for Op {
    fn drop(&mut self) {
        self.close_pipe();
        close(self.ov.event);
        close(self.process);
        close(self.job); // kills anything still in it
        if let Some(t) = self.tmpfile.take() {
            // SAFETY: t is NUL-terminated.
            unsafe { DeleteFileW(t.as_ptr()) };
        }
    }
}

#[link(name = "kernel32")]
extern "system" {
    fn DeleteFileW(name: *const u16) -> BOOL;
}

// ---- command lines and paths ----

/// One argument of a command line, quoted the way CommandLineToArgvW and the C runtimes (Git
/// Bash's included) take it apart: backslashes are literal unless they come before a quote.
pub fn quote_arg(b: &mut Vec<u16>, a: &[u16]) {
    let bs_ = b'\\' as u16;
    let q = b'"' as u16;
    if !b.is_empty() {
        b.push(b' ' as u16);
    }
    b.push(q);
    let mut i = 0;
    loop {
        let mut bs = 0;
        while i < a.len() && a[i] == bs_ {
            bs += 1;
            i += 1;
        }
        if i == a.len() {
            b.extend(std::iter::repeat_n(bs_, bs * 2));
            break;
        }
        if a[i] == q {
            b.extend(std::iter::repeat_n(bs_, bs * 2 + 1));
        } else {
            b.extend(std::iter::repeat_n(bs_, bs));
        }
        b.push(a[i]);
        i += 1;
    }
    b.push(q);
}

fn temp_dir() -> Option<Vec<u16>> {
    let mut buf = [0u16; MAX_PATH + 1];
    // SAFETY: buf has the room given.
    let k = unsafe { GetTempPathW(buf.len() as u32, buf.as_mut_ptr()) } as usize;
    (k > 0 && k <= MAX_PATH).then(|| buf[..k].to_vec())
}

/// A path the model gave, as Windows wants it: absolute, with the Git Bash spellings
/// (/c/Users/..., /tmp/...) turned into Windows ones.
pub fn win_path(s: &[u8]) -> PathBuf {
    let n = s.len();
    let w: Vec<u16> = if n >= 2 && s[0] == b'/' && s[1].is_ascii_alphabetic() && (n == 2 || s[2] == b'/') {
        let mut t = vec![s[1], b':', b'/'];
        if n > 3 {
            t.extend_from_slice(&s[3..]);
        }
        utf16(&t)
    } else if n >= 4 && &s[..4] == b"/tmp" && (n == 4 || s[4] == b'/') {
        let rest = &s[(4 + (n > 4) as usize)..];
        match temp_dir() {
            Some(tmp) => {
                let mut w = tmp;
                if let Some(&c) = w.last() {
                    if c != b'\\' as u16 && c != b'/' as u16 {
                        w.push(b'\\' as u16);
                    }
                }
                w.extend(utf16(rest));
                w
            }
            None => utf16(s),
        }
    } else {
        utf16(s)
    };
    let p = PathBuf::from(OsString::from_wide(&w));
    std::path::absolute(&p).unwrap_or(p)
}

/// The path NUL-terminated for a Win32 call, with the \\?\ prefix when it is long (std adds it
/// itself; this is for the calls barb makes directly).
fn long(p: &Path) -> Vec<u16> {
    let mut w: Vec<u16> = unz(&wide(p)).to_vec();
    let verbatim: Vec<u16> = r"\\?\".encode_utf16().collect();
    if w.len() >= MAX_PATH - 12 && !w.starts_with(&verbatim) {
        if w.starts_with(&[b'\\' as u16, b'\\' as u16]) {
            let mut r: Vec<u16> = r"\\?\UNC".encode_utf16().collect();
            r.extend_from_slice(&w[1..]); // the second \ of \\server stays
            w = r;
        } else {
            let mut r = verbatim;
            r.extend_from_slice(&w);
            w = r;
        }
    }
    w.push(0);
    w
}

fn mkdir_parents(p: &Path) {
    if let Some(d) = p.parent() {
        if !d.as_os_str().is_empty() {
            let _ = fs::create_dir_all(d);
        }
    }
}

// ---- finding programs ----

/// A program in a directory on PATH. Only absolute directories count: Windows' own search would
/// try the working directory first, and a project that ships a curl.exe must not get the API key.
fn on_path(exe: &str) -> Option<Vec<u16>> {
    let path = env("PATH")?;
    for dir in path.split(|&c| c == b';' as u16) {
        let mut dir = dir;
        let q = b'"' as u16;
        if dir.len() >= 2 && dir[0] == q && dir[dir.len() - 1] == q {
            dir = &dir[1..dir.len() - 1];
        }
        let is = |i: usize, c: u8| dir.get(i) == Some(&(c as u16));
        let absolute = dir.len() >= 3 && ((is(1, b':') && (is(2, b'\\') || is(2, b'/'))) || (is(0, b'\\') && is(1, b'\\')));
        if !absolute {
            continue;
        }
        let p = join(dir, exe);
        if is_file(&p) {
            return Some(p);
        }
    }
    None
}

/// bin\bash.exe under a Git for Windows installation: the launcher that sets up PATH so that ls,
/// grep and the rest are found (usr\bin\bash.exe alone does not).
fn git_bash(root: &[u16]) -> Option<Vec<u16>> {
    let p = join(root, "bin\\bash.exe");
    is_file(&p).then_some(p)
}

fn find_shell() -> Option<Vec<u16>> {
    if let Some(p) = env("BARB_SHELL") {
        return is_file(&p).then_some(p); // set, and wrong: say so rather than use something else
    }
    let sub = wide("SOFTWARE\\GitForWindows");
    let val = wide("InstallPath");
    for root in [HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER] {
        let mut buf = [0u16; MAX_PATH * 2];
        let mut size = std::mem::size_of_val(&buf) as u32;
        // SAFETY: the names are NUL-terminated and buf has `size` bytes.
        let r = unsafe {
            RegGetValueW(root, sub.as_ptr(), val.as_ptr(), RRF_RT_REG_SZ, null_mut(), buf.as_mut_ptr() as *mut c_void, &mut size)
        };
        if r == 0 {
            if let Some(p) = git_bash(unz(&buf)) {
                return Some(p);
            }
        }
    }
    // git.exe on PATH is Git\cmd\git.exe or Git\mingw64\bin\git.exe.
    if let Some(mut git) = on_path("git.exe") {
        for _ in 0..3 {
            let Some(cut) = git.iter().rposition(|&c| c == b'\\' as u16) else { break };
            git.truncate(cut);
            if let Some(p) = git_bash(&git) {
                return Some(p);
            }
        }
    }
    for (var, sub) in [("ProgramW6432", "Git"), ("ProgramFiles", "Git"), ("LOCALAPPDATA", "Programs\\Git")] {
        if let Some(base) = env(var) {
            if let Some(p) = git_bash(&join(&base, sub)) {
                return Some(p);
            }
        }
    }
    None
}

fn find_curl() -> Option<Vec<u16>> {
    let mut sys = [0u16; MAX_PATH];
    // SAFETY: sys has the room given.
    let n = unsafe { GetSystemDirectoryW(sys.as_mut_ptr(), MAX_PATH as u32) } as usize;
    if n > 0 && n < MAX_PATH {
        let p = join(&sys[..n], "curl.exe");
        if is_file(&p) {
            return Some(p);
        }
    }
    on_path("curl.exe")
}

fn with_nul(w: &[u16]) -> Vec<u16> {
    let mut v = unz(w).to_vec();
    v.push(0);
    v
}

fn open_nul() -> HANDLE {
    let name = wide("NUL");
    let mut sa = inheritable();
    // SAFETY: name is NUL-terminated and sa lives through the call.
    unsafe { CreateFileW(name.as_ptr(), GENERIC_READ, FILE_SHARE_READ_WRITE, &mut sa, OPEN_EXISTING, 0, 0) }
}

fn write_handle(h: HANDLE, mut p: &[u8]) -> bool {
    while !p.is_empty() {
        let chunk = p.len().min(1 << 30) as u32;
        let mut w = 0u32;
        // SAFETY: p has at least `chunk` bytes.
        if unsafe { WriteFile(h, p.as_ptr(), chunk, &mut w, null_mut()) } == 0 || w == 0 {
            return false;
        }
        p = &p[w as usize..];
    }
    true
}

// ---- files ----

fn fs_status(err: u32) -> Fs {
    match err {
        2 | 3 | 123 | 161 | 15 | 53 | 67 | 267 => Fs::NotFound, // FILE/PATH_NOT_FOUND, INVALID_NAME, BAD_PATHNAME, INVALID_DRIVE, BAD_NETPATH, BAD_NET_NAME, DIRECTORY
        5 | 1314 | 19 => Fs::Denied,                           // ACCESS_DENIED, PRIVILEGE_NOT_HELD, WRITE_PROTECT
        _ => Fs::Error,
    }
}

fn file_error(id: i64, err: u32) -> Ev {
    Ev::file(id, fs_status(err), &why(err))
}

fn io_code(e: &std::io::Error) -> u32 {
    e.raw_os_error().map(|c| c as u32).unwrap_or(ERROR_WRITE_FAULT)
}

fn read_file(id: i64, p: &[u8]) -> Ev {
    let w = win_path(p);
    // SAFETY: the name is NUL-terminated.
    let attrs = unsafe { GetFileAttributesW(long(&w).as_ptr()) };
    if attrs == 0xFFFF_FFFF {
        return file_error(id, last_error());
    }
    if attrs & 0x10 != 0 {
        return Ev::File { id, status: Fs::IsDir, data: Vec::new(), note: Vec::new() };
    }
    let f = match OpenOptions::new().read(true).share_mode(FILE_SHARE_ALL).custom_flags(FILE_FLAG_SEQUENTIAL_SCAN).open(&w) {
        Ok(f) => f,
        Err(e) => return file_error(id, io_code(&e)),
    };
    // SAFETY: the handle belongs to f, which is open.
    if unsafe { GetFileType(f.as_raw_handle() as HANDLE) } != FILE_TYPE_DISK {
        return Ev::file(id, Fs::Error, "not a file on disk (a device such as CON or NUL)");
    }
    if let Ok(md) = f.metadata() {
        if md.len() > READ_LIMIT {
            return Ev::file(id, Fs::TooBig, &format!("{} bytes, the limit is {}", md.len(), READ_LIMIT));
        }
    }
    let mut b = Vec::new();
    let r = f.take(READ_LIMIT + 1).read_to_end(&mut b);
    if b.len() as u64 > READ_LIMIT {
        return Ev::file(id, Fs::TooBig, "more than the size limit");
    }
    if let Err(e) = r {
        return file_error(id, io_code(&e));
    }
    Ev::File { id, status: Fs::Ok, data: b, note: Vec::new() }
}

/// Write to a temporary file beside the target and move it into place, so a crash or a full disk
/// never leaves half a file. ReplaceFileW keeps the old file's attributes and permissions. A
/// symbolic link is followed, so the link stays.
fn write_file(id: i64, p: &[u8], data: &[u8]) -> Ev {
    let mut target = win_path(p);
    let mut exists = false;
    if let Ok(h) = OpenOptions::new().access_mode(0).share_mode(FILE_SHARE_ALL).custom_flags(FILE_FLAG_BACKUP_SEMANTICS).open(&target) {
        exists = true;
        if h.metadata().map(|m| m.is_dir()).unwrap_or(false) {
            return Ev::file(id, Fs::IsDir, "it is a directory");
        }
        drop(h);
        if let Ok(t) = fs::canonicalize(&target) {
            target = t;
        }
    }
    mkdir_parents(&target);
    let mut tmp = target.clone().into_os_string();
    // SAFETY: no arguments.
    tmp.push(format!(".barb-tmp-{}", unsafe { GetCurrentProcessId() }));
    let tmp = PathBuf::from(tmp);
    let mut err = 0u32;
    match OpenOptions::new().write(true).create(true).truncate(true).open(&tmp) {
        Err(e) => err = io_code(&e),
        Ok(mut f) => {
            if let Err(e) = f.write_all(data) {
                err = io_code(&e);
            }
            drop(f);
            if err == 0 {
                let (t, s) = (long(&target), long(&tmp));
                // SAFETY: both names are NUL-terminated.
                let moved = exists
                    && unsafe {
                        ReplaceFileW(
                            t.as_ptr(),
                            s.as_ptr(),
                            null(),
                            REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS,
                            null_mut(),
                            null_mut(),
                        )
                    } != 0;
                if !moved {
                    if let Err(e) = fs::rename(&tmp, &target) {
                        err = io_code(&e);
                    }
                }
            }
            if err != 0 {
                let _ = fs::remove_file(&tmp);
            }
        }
    }
    if err != 0 {
        return file_error(id, err);
    }
    Ev::File { id, status: Fs::Ok, data: Vec::new(), note: Vec::new() }
}

// ---- the operations ----

pub struct Ops {
    ops: Vec<Op>,
    parked: Vec<HANDLE>, // jobs of finished background tasks, with whatever they left running
    overflow: bool,      // more handles than the main loop could wait on
    base: Vec<u8>,
    key: Vec<u8>,
    shell: Option<Vec<u16>>, // bash.exe, NUL-terminated
    shell_u8: Option<String>,
    curl: Option<Vec<u16>>,
}

impl Ops {
    pub fn new(base: Vec<u8>, key: Vec<u8>) -> Ops {
        let shell = find_shell().map(|s| with_nul(&s));
        let shell_u8 = shell.as_ref().map(|s| String::from_utf16_lossy(unz(s)));
        Ops { ops: Vec::new(), parked: Vec::new(), overflow: false, base, key, shell, shell_u8, curl: find_curl().map(|c| with_nul(&c)) }
    }

    /// The bash commands run in, if there is one.
    pub fn shell(&self) -> Option<&str> {
        self.shell_u8.as_deref()
    }

    fn full(&self) -> bool {
        self.ops.len() >= MAX_OPS
    }

    // ---- processes ----

    pub fn spawn(&mut self, q: &mut VecDeque<Ev>, id: i64, cmd: &[u8], timeout_ms: i64, bg: bool, log: &[u8]) {
        let Some(shell) = self.shell.clone() else {
            q.push_back(Ev::failed_proc(
                id,
                "Git Bash was not found. barb runs commands in the bash that comes with Git for Windows: \
                 install Git for Windows, or set BARB_SHELL to the full path of a bash.exe",
            ));
            return;
        };
        if self.full() {
            q.push_back(Ev::failed_proc(id, "too many operations running at once"));
            return;
        }
        let mut o = Op::new(Kind::Proc, id);
        o.bg = bg;
        let mut line = Vec::new();
        quote_arg(&mut line, unz(&shell));
        quote_arg(&mut line, &utf16(b"-c"));
        quote_arg(&mut line, &utf16(cmd));
        line.push(0);

        let inp = open_nul();
        let in_err = last_error();
        let out = if bg {
            let lp = win_path(log);
            mkdir_parents(&lp);
            let name = long(&lp);
            let mut sa = inheritable();
            // SAFETY: name is NUL-terminated and sa lives through the call.
            let h = unsafe {
                CreateFileW(name.as_ptr(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_ALL, &mut sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0)
            };
            if h == INVALID_HANDLE_VALUE {
                let note = format!("cannot open the log file {}: {}", String::from_utf8_lossy(log), why(last_error()));
                close(inp);
                q.push_back(Ev::failed_proc(id, &note));
                return;
            }
            h
        } else {
            match o.make_pipe() {
                Ok(h) => h,
                Err(e) => {
                    close(inp);
                    q.push_back(Ev::failed_proc(id, &format!("cannot make a pipe for the output: {}", why(e))));
                    return;
                }
            }
        };
        let started = if inp == INVALID_HANDLE_VALUE { Err(in_err) } else { o.start(&shell, &mut line, inp, out) };
        close(inp);
        close(out); // the child has its own; this end must close for the pipe to end
        if let Err(e) = started {
            let note = format!("cannot start {}: {}", self.shell_u8.as_deref().unwrap_or("bash"), why(e));
            q.push_back(Ev::failed_proc(id, &note));
            return;
        }
        if !bg {
            if timeout_ms > 0 {
                o.deadline = o.started + timeout_ms;
            }
            o.pump();
        }
        self.ops.push(o);
    }

    // ---- HTTP, through curl ----
    //
    // As on Linux: the key and the headers go to curl on its standard input as a config file,
    // never on the command line; the body goes in a temporary file; curl prints the body and then
    // the status on its own last line.

    pub fn http(&mut self, q: &mut VecDeque<Ev>, id: i64, api_path: &[u8], body: &[u8]) {
        let Some(curl) = self.curl.clone() else {
            q.push_back(Ev::failed_http(id, "curl.exe was not found (Windows 10 and later have it in System32)"));
            return;
        };
        if self.full() {
            q.push_back(Ev::failed_http(id, "too many operations running at once"));
            return;
        }
        let mut o = Op::new(Kind::Http, id);
        let mut file = [0u16; MAX_PATH + 1];
        let made = temp_dir().is_some_and(|d| {
            let d = with_nul(&d);
            let prefix = wide("brb");
            // SAFETY: d and prefix are NUL-terminated; file has MAX_PATH + 1 places.
            unsafe { GetTempFileNameW(d.as_ptr(), prefix.as_ptr(), 0, file.as_mut_ptr()) != 0 }
        });
        if !made {
            q.push_back(Ev::failed_http(id, "cannot make a temporary file for the request"));
            return;
        }
        let file = with_nul(&file);
        o.tmpfile = Some(file.clone());
        let fpath = PathBuf::from(OsString::from_wide(unz(&file)));
        let wrote = OpenOptions::new()
            .write(true)
            .truncate(true)
            .attributes(0x100) // FILE_ATTRIBUTE_TEMPORARY
            .open(&fpath)
            .and_then(|mut f| f.write_all(body));
        if wrote.is_err() {
            q.push_back(Ev::failed_http(id, "cannot write the request to a temporary file"));
            return;
        }
        // curl may open files with the ANSI code page, so give it the short (8.3) name, which has
        // no characters a user name might bring into the temporary directory's path.
        let mut short = [0u16; MAX_PATH + 1];
        // SAFETY: file is NUL-terminated; short has the room given.
        let sn = unsafe { GetShortPathNameW(file.as_ptr(), short.as_mut_ptr(), short.len() as u32) } as usize;
        let data_path = utf8(if sn > 0 && sn <= MAX_PATH { &short[..sn] } else { unz(&file) });

        let mut url = self.base.clone();
        url.extend_from_slice(api_path);
        let mut cfg = common::curl_config(&url, &self.key, &data_path);
        let mut line = Vec::new();
        quote_arg(&mut line, unz(&curl));
        for a in common::curl_args("\\n%{http_code}") {
            quote_arg(&mut line, &utf16(a.as_bytes()));
        }
        line.push(0);

        let (mut in_r, mut in_w) = (0, 0);
        let mut out = INVALID_HANDLE_VALUE;
        // SAFETY: out-pointers to locals; the handles are ours.
        let mut started = if unsafe { CreatePipe(&mut in_r, &mut in_w, null_mut(), 1 << 16) } == 0
            || unsafe { SetHandleInformation(in_r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) } == 0
        {
            Err(last_error())
        } else {
            Ok(())
        };
        if started.is_ok() {
            started = o.make_pipe().map(|h| out = h);
        }
        if started.is_ok() {
            started = o.start(&curl, &mut line, in_r, out);
        }
        close(in_r);
        close(out);
        if started.is_ok() {
            // The config is far smaller than the pipe's buffer, so this does not block. (A curl
            // that died at once makes it fail, which is harmless: its exit says what happened.)
            write_handle(in_w, &cfg);
        }
        close(in_w);
        common::wipe(&mut cfg); // the key was in there
        if let Err(e) = started {
            q.push_back(Ev::failed_http(id, &format!("cannot start curl: {}", why(e))));
            return;
        }
        o.pump();
        self.ops.push(o);
    }

    // ---- files ----

    pub fn read(&mut self, q: &mut VecDeque<Ev>, id: i64, p: &[u8]) {
        q.push_back(read_file(id, p));
    }

    pub fn write(&mut self, q: &mut VecDeque<Ev>, id: i64, p: &[u8], data: &[u8]) {
        q.push_back(write_file(id, p, data));
    }

    // ---- timers and the rest ----

    pub fn timer(&mut self, id: i64, ms: i64) {
        if self.full() {
            return;
        }
        let mut o = Op::new(Kind::Timer, id);
        o.deadline = now() + ms.max(0);
        self.ops.push(o);
    }

    /// A cancelled timer is gone; a command or a request is killed. Only a background task still
    /// reports.
    pub fn cancel(&mut self, id: i64) {
        let Some(i) = self.ops.iter().position(|o| o.id == id) else { return };
        if self.ops[i].kind == Kind::Timer {
            self.ops.remove(i);
            return;
        }
        let o = &mut self.ops[i];
        o.cancelled = true;
        o.kill();
    }

    /// The handles to wait on: the pipes being read and the processes still running.
    pub fn handles(&mut self, h: &mut Vec<HANDLE>, max: usize) {
        self.overflow = false;
        for o in &self.ops {
            if o.kind == Kind::Timer {
                continue;
            }
            if o.pipe != 0 && o.reading {
                if h.len() < max {
                    h.push(o.ov.event);
                } else {
                    self.overflow = true;
                }
            }
            if o.process != 0 && !o.exited {
                if h.len() < max {
                    h.push(o.process);
                } else {
                    self.overflow = true;
                }
            }
        }
    }

    /// How long the loop may sleep: -1 if nothing is waiting on the clock.
    pub fn timeout_ms(&self) -> i64 {
        let now = now();
        let mut best = -1i64;
        for o in &self.ops {
            let d = match o.kind {
                Kind::Timer => o.deadline,
                Kind::Proc if o.deadline != 0 && !o.timed_out && !o.exited => o.deadline,
                _ => 0,
            };
            if d != 0 && (best < 0 || d - now < best) {
                best = (d - now).max(0);
            }
        }
        if self.overflow && (best < 0 || best > 50) {
            best = 50; // what could not be waited on is checked often
        }
        best
    }

    /// A background task's job outlives the task, with anything the task left running (a server
    /// it started with &), until barb exits.
    fn park(&mut self, o: &mut Op) {
        if o.job == 0 {
            return;
        }
        if self.parked.len() == MAX_PARKED {
            close(self.parked.remove(0));
        }
        self.parked.push(o.job);
        o.job = 0;
    }

    fn finish(&mut self, mut o: Op, q: &mut VecDeque<Ev>) {
        // What a foreground command left running is no longer wanted, and must go before its
        // output can end: it holds the pipe open.
        if !o.bg && o.job != 0 {
            // SAFETY: the job is ours.
            unsafe { TerminateJobObject(o.job, 1) };
        }
        o.drain_all();
        let ms = now() - o.started;
        let code = if o.killed { -1 } else { o.code as i32 };
        match o.kind {
            Kind::Proc => {
                if !o.cancelled || o.bg {
                    // (an interrupted foreground command has no listener)
                    let out = std::mem::take(&mut o.out).take(); // (none for a background task: it went to the log)
                    let exit = if o.timed_out {
                        Exit::TimedOut
                    } else if o.killed {
                        Exit::Signaled
                    } else {
                        Exit::Code(code)
                    };
                    q.push_back(Ev::Proc { id: o.id, exit, out, ms, note: Vec::new() });
                }
                if o.bg {
                    self.park(&mut o);
                }
            }
            Kind::Http if !o.cancelled => {
                let (status, body, note) = common::curl_reply(code, &o.body);
                q.push_back(Ev::Http { id: o.id, status, body, note });
            }
            _ => {}
        }
    }

    /// Whatever is due.
    pub fn service(&mut self, q: &mut VecDeque<Ev>) {
        let now = now();
        let mut i = 0;
        while i < self.ops.len() {
            let o = &mut self.ops[i];
            if o.kind == Kind::Timer {
                if now >= o.deadline {
                    let id = o.id;
                    self.ops.remove(i);
                    q.push_back(Ev::Timer { id });
                } else {
                    i += 1;
                }
                continue;
            }
            o.pump();
            // SAFETY: the process handle is ours; no waiting.
            if o.process != 0 && !o.exited && unsafe { WaitForSingleObject(o.process, 0) } == WAIT_OBJECT_0 {
                o.exited = true;
                // SAFETY: as above.
                if unsafe { GetExitCodeProcess(o.process, &mut o.code) } == 0 {
                    o.code = u32::MAX;
                }
            }
            if o.kind == Kind::Proc && !o.exited && o.deadline != 0 && !o.timed_out && now >= o.deadline {
                o.timed_out = true;
                o.kill();
                i += 1; // its exit is noticed the next time round
                continue;
            }
            if o.exited {
                let o = self.ops.remove(i);
                self.finish(o, q);
                continue;
            }
            i += 1;
        }
    }

    /// Kill everything still running and remove the temporary files.
    pub fn shutdown(&mut self) {
        for o in &mut self.ops {
            o.kill();
            if o.process != 0 {
                // SAFETY: the handle is ours.
                unsafe { WaitForSingleObject(o.process, 2000) };
            }
        }
        self.ops.clear();
        for j in self.parked.drain(..) {
            // SAFETY: the job is ours.
            unsafe { TerminateJobObject(j, 1) };
            close(j);
        }
    }
}

impl Drop for Ops {
    fn drop(&mut self) {
        common::wipe(&mut self.key);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn quoted(args: &[&str]) -> String {
        let mut b = Vec::new();
        for a in args {
            quote_arg(&mut b, &a.encode_utf16().collect::<Vec<_>>());
        }
        String::from_utf16(&b).unwrap()
    }

    #[test]
    fn arguments_are_quoted_for_the_c_runtime() {
        assert_eq!(quoted(&["a b", "-c"]), r#""a b" "-c""#);
        assert_eq!(quoted(&[r#"say "hi""#]), r#""say \"hi\"""#);
        assert_eq!(quoted(&[r"C:\dir\"]), r#""C:\dir\\""#, "a trailing backslash is doubled");
        assert_eq!(quoted(&[r#"a\"b"#]), r#""a\\\"b""#);
        assert_eq!(quoted(&[r"a\\b"]), r#""a\\b""#, "backslashes elsewhere stay as they are");
        assert_eq!(quoted(&[""]), r#""""#);
    }

    #[test]
    fn git_bash_paths_become_windows_ones() {
        assert_eq!(win_path(b"/c/Users/x"), PathBuf::from(r"c:\Users\x"));
        assert_eq!(win_path(b"/d"), PathBuf::from(r"d:\"));
        let tmp = PathBuf::from(OsString::from_wide(&temp_dir().unwrap()));
        assert_eq!(win_path(b"/tmp/a.txt"), tmp.join("a.txt"));
        assert!(win_path(b"rel.txt").is_absolute());
        assert_eq!(win_path(b"/cd/x"), std::path::absolute("/cd/x").unwrap(), "not a drive");
    }

    #[test]
    fn long_paths_get_the_prefix() {
        let p = PathBuf::from(format!(r"C:\{}", "a".repeat(300)));
        assert!(String::from_utf16_lossy(&long(&p)).starts_with(r"\\?\C:\"));
        let u = PathBuf::from(format!(r"\\server\share\{}", "a".repeat(300)));
        assert!(String::from_utf16_lossy(&long(&u)).starts_with(r"\\?\UNC\server\share"));
        assert_eq!(long(Path::new(r"C:\x")), wide(r"C:\x"));
    }

    fn run(ops: &mut Ops, q: &mut VecDeque<Ev>, until: impl Fn(&VecDeque<Ev>) -> bool) {
        let t = std::time::Instant::now();
        while !until(q) {
            assert!(t.elapsed().as_secs() < 30, "timed out");
            let mut h = Vec::new();
            ops.handles(&mut h, MAXIMUM_WAIT_OBJECTS);
            let w = ops.timeout_ms().clamp(0, 100) as u32;
            if h.is_empty() {
                std::thread::sleep(std::time::Duration::from_millis(w as u64));
            } else {
                // SAFETY: the handles are the operations' own, open while they are listed.
                unsafe { WaitForMultipleObjects(h.len() as u32, h.as_ptr(), 0, w) };
            }
            ops.service(q);
        }
    }

    #[test]
    fn a_command_its_output_and_its_time_limit() {
        let (mut ops, mut q) = (Ops::new(vec![], vec![]), VecDeque::new());
        if ops.shell().is_none() {
            eprintln!("no Git Bash here: skipped");
            return;
        }
        ops.spawn(&mut q, 1, b"printf 'out '; printf 'err' >&2; exit 3", 0, false, b"");
        run(&mut ops, &mut q, |q| !q.is_empty());
        match q.pop_front().unwrap() {
            Ev::Proc { id: 1, exit: Exit::Code(3), out, .. } => assert_eq!(out, b"out err"),
            e => panic!("{:?}", e),
        }
        ops.spawn(&mut q, 2, b"sleep 30", 300, false, b"");
        let t = std::time::Instant::now();
        run(&mut ops, &mut q, |q| !q.is_empty());
        assert!(t.elapsed().as_millis() < 5000);
        assert!(matches!(q.pop_front(), Some(Ev::Proc { id: 2, exit: Exit::TimedOut, .. })));
        assert_eq!(ops.timeout_ms(), -1, "nothing left to wait for");
    }

    #[test]
    fn files_are_read_and_replaced() {
        let d = std::env::temp_dir().join(format!("barb-winops-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        let (mut ops, mut q) = (Ops::new(vec![], vec![]), VecDeque::new());
        let f = d.join("sub").join("f.txt");
        let fb = f.to_string_lossy().into_owned().into_bytes();
        ops.write(&mut q, 1, &fb, b"one");
        assert!(matches!(q.pop_front(), Some(Ev::File { id: 1, status: Fs::Ok, .. })));
        ops.write(&mut q, 2, &fb, b"two");
        assert!(matches!(q.pop_front(), Some(Ev::File { id: 2, status: Fs::Ok, .. })));
        ops.read(&mut q, 3, &fb);
        match q.pop_front().unwrap() {
            Ev::File { id: 3, status: Fs::Ok, data, .. } => assert_eq!(data, b"two"),
            e => panic!("{:?}", e),
        }
        let names: Vec<_> = fs::read_dir(d.join("sub")).unwrap().map(|e| e.unwrap().file_name()).collect();
        assert_eq!(names.len(), 1, "no temporary file left: {:?}", names);
        ops.read(&mut q, 4, d.to_string_lossy().as_bytes());
        assert!(matches!(q.pop_front(), Some(Ev::File { id: 4, status: Fs::IsDir, .. })));
        ops.write(&mut q, 5, d.to_string_lossy().as_bytes(), b"x");
        assert!(matches!(q.pop_front(), Some(Ev::File { id: 5, status: Fs::IsDir, .. })));
        ops.read(&mut q, 6, d.join("none").to_string_lossy().as_bytes());
        assert!(matches!(q.pop_front(), Some(Ev::File { id: 6, status: Fs::NotFound, .. })));
        let _ = fs::remove_dir_all(&d);
    }
}
