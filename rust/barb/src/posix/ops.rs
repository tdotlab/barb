//! What the agent's actions turn into on Linux and macOS: processes, HTTP, files, timers.
//!
//! Every operation ends in exactly one event pushed on the queue (or none, if it was cancelled).
//! The loop sleeps in [`sys::wait`] on the descriptors [`Ops::fill`] names, for at most
//! [`Ops::timeout_ms`], then calls [`Ops::service`], which reads output, reaps children, fires
//! timers and enforces time limits.

use super::sys::{self, PollFd, POLLIN, SIGKILL, SIGTERM};
use crate::common::{self, strerror, Ev, Output, HTTP_MAX};
use barb_core::agent::{Exit, Fs};
use std::collections::VecDeque;
use std::ffi::OsStr;
use std::fs::{self, File, OpenOptions};
use std::io::{ErrorKind, PipeReader, Read, Write};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::os::unix::io::{AsRawFd, IntoRawFd};
use std::os::unix::process::{CommandExt, ExitStatusExt};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::OnceLock;
use std::time::Instant;

const MAX_OPS: usize = 64;
const READ_LIMIT: u64 = 8 << 20;
const KILL_GRACE_MS: i64 = 2000;

/// Monotonic milliseconds.
pub fn now() -> i64 {
    static T0: OnceLock<Instant> = OnceLock::new();
    T0.get_or_init(Instant::now).elapsed().as_millis() as i64 + 1
}

#[derive(PartialEq, Clone, Copy)]
enum Kind {
    Proc,
    Http,
    Timer,
}

struct Op {
    kind: Kind,
    id: i64,
    child: Option<Child>,
    pid: i32,
    rd: Option<PipeReader>, // the child's output, until its end
    bg: bool,
    status: Option<ExitStatus>,
    cancelled: bool,
    timed_out: bool,
    started: i64,
    deadline: i64, // monotonic ms; 0 = none
    kill_at: i64,
    out: Output,
    body: Vec<u8>,
    tmpfile: Option<PathBuf>,
}

impl Op {
    fn new(kind: Kind, id: i64) -> Op {
        Op {
            kind,
            id,
            child: None,
            pid: 0,
            rd: None,
            bg: false,
            status: None,
            cancelled: false,
            timed_out: false,
            started: now(),
            deadline: 0,
            kill_at: 0,
            out: Output::default(),
            body: Vec::new(),
            tmpfile: None,
        }
    }

    fn kill(&self, sig: i32) {
        sys::kill_group(self.pid, sig);
    }

    // Read what is waiting; at the end of the output, stop watching it.
    fn drain(&mut self) {
        let mut buf = vec![0u8; 65536];
        while let Some(r) = self.rd.as_mut() {
            match r.read(&mut buf) {
                Ok(0) => self.rd = None,
                Ok(n) => {
                    if self.kind == Kind::Proc {
                        self.out.add(&buf[..n]);
                    } else {
                        let room = HTTP_MAX.saturating_sub(self.body.len()).min(n);
                        self.body.extend_from_slice(&buf[..room]);
                    }
                }
                Err(e) if e.kind() == ErrorKind::Interrupted => {}
                Err(e) if e.kind() == ErrorKind::WouldBlock => return,
                Err(_) => self.rd = None,
            }
        }
    }
}

impl Drop for Op {
    fn drop(&mut self) {
        if let Some(t) = self.tmpfile.take() {
            let _ = fs::remove_file(t);
        }
    }
}

pub struct Ops {
    ops: Vec<Op>,
    base: Vec<u8>,
    key: Vec<u8>,
}

fn path(p: &[u8]) -> &Path {
    Path::new(OsStr::from_bytes(p))
}

// All the directories above `p`, as far as they can be made.
fn mkdir_p(p: &Path) {
    if let Some(d) = p.parent() {
        if !d.as_os_str().is_empty() {
            let _ = fs::create_dir_all(d);
        }
    }
}

fn fs_status(e: &std::io::Error) -> Fs {
    match e.raw_os_error() {
        Some(2) | Some(20) => Fs::NotFound, // ENOENT, ENOTDIR
        Some(13) | Some(1) => Fs::Denied,   // EACCES, EPERM
        Some(21) => Fs::IsDir,              // EISDIR
        _ => Fs::Error,
    }
}

fn file_err(id: i64, e: &std::io::Error) -> Ev {
    Ev::file(id, fs_status(e), &strerror(e))
}

// A pipe for a child's output: (the end to read, unblocked; the end the child writes).
fn out_pipe() -> std::io::Result<(PipeReader, std::io::PipeWriter)> {
    let (r, w) = std::io::pipe()?;
    sys::set_nonblock(r.as_raw_fd());
    Ok((r, w))
}

impl Ops {
    pub fn new(base: Vec<u8>, key: Vec<u8>) -> Ops {
        Ops { ops: Vec::new(), base, key }
    }

    fn full(&self) -> bool {
        self.ops.len() >= MAX_OPS
    }

    // ---- processes ----

    /// `cmd` through sh, in a process group of its own, with no standard input. A foreground
    /// command's output comes back in its event; a background task's goes to `log`.
    pub fn spawn(&mut self, q: &mut VecDeque<Ev>, id: i64, cmd: &[u8], timeout_ms: i64, bg: bool, log: &[u8]) {
        if self.full() {
            q.push_back(Ev::failed_proc(id, "too many operations running at once"));
            return;
        }
        let mut o = Op::new(Kind::Proc, id);
        o.bg = bg;
        let mut c = Command::new("sh");
        c.arg("-c").arg(OsStr::from_bytes(cmd)).stdin(Stdio::null()).process_group(0);
        if bg {
            let lp = path(log);
            mkdir_p(lp);
            let f = match OpenOptions::new().write(true).create(true).truncate(true).mode(0o644).open(lp) {
                Ok(f) => f,
                Err(e) => {
                    let why = format!("cannot open the log file {}: {}", lp.display(), strerror(&e));
                    q.push_back(Ev::failed_proc(id, &why));
                    return;
                }
            };
            match f.try_clone() {
                Ok(f2) => c.stdout(f2).stderr(f),
                Err(e) => return q.push_back(Ev::failed_proc(id, &strerror(&e))),
            };
        } else {
            let (r, w1, w2) = match out_pipe().and_then(|(r, w)| Ok((r, w.try_clone()?, w))) {
                Ok(p) => p,
                Err(e) => return q.push_back(Ev::failed_proc(id, &strerror(&e))),
            };
            c.stdout(w1).stderr(w2);
            o.rd = Some(r);
            if timeout_ms > 0 {
                o.deadline = o.started + timeout_ms;
            }
        }
        let started = c.spawn();
        drop(c); // the parent's copies of the child's ends: only the child holds them now
        match started {
            Ok(ch) => {
                o.pid = ch.id() as i32;
                o.child = Some(ch);
                self.ops.push(o);
            }
            Err(e) => q.push_back(Ev::failed_proc(id, &format!("cannot start sh: {}", strerror(&e)))),
        }
    }

    // ---- HTTP, through curl ----
    //
    // The key and the headers go to curl on its standard input as a config file, never on the
    // command line (where any user could read them from the process list). The body goes in a
    // private temporary file. curl prints the body and then the status on its own last line.

    pub fn http(&mut self, q: &mut VecDeque<Ev>, id: i64, api_path: &[u8], body: &[u8]) {
        if self.full() {
            q.push_back(Ev::failed_http(id, "too many operations running at once"));
            return;
        }
        let mut o = Op::new(Kind::Http, id);
        let dir = match std::env::var_os("TMPDIR") {
            Some(d) if !d.is_empty() => d.into_encoded_bytes(),
            _ => b"/tmp".to_vec(),
        };
        let mut tmpl = dir;
        tmpl.extend_from_slice(b"/barb-body-XXXXXX");
        let Some(mut f) = sys::make_temp(&mut tmpl) else {
            q.push_back(Ev::failed_http(id, "cannot make a temporary file for the request"));
            return;
        };
        o.tmpfile = Some(path(&tmpl).to_path_buf());
        let _ = f.set_permissions(fs::Permissions::from_mode(0o600));
        if f.write_all(body).is_err() {
            q.push_back(Ev::failed_http(id, "cannot write the request to a temporary file"));
            return;
        }
        drop(f);

        let mut url = self.base.clone();
        url.extend_from_slice(api_path);
        let mut cfg = common::curl_config(&url, &self.key, &tmpl);
        let (r, w1, w2) = match out_pipe().and_then(|(r, w)| Ok((r, w.try_clone()?, w))) {
            Ok(p) => p,
            Err(e) => {
                common::wipe(&mut cfg);
                return q.push_back(Ev::failed_http(id, &strerror(&e)));
            }
        };
        let mut c = Command::new("curl");
        c.args(common::curl_args("\n%{http_code}")).stdin(Stdio::piped()).stdout(w1).stderr(w2).process_group(0);
        let started = c.spawn();
        drop(c);
        match started {
            Ok(mut ch) => {
                // The config is small enough to sit in the pipe. A curl that died at once is an
                // error here, not a signal: std ignores SIGPIPE.
                if let Some(mut stdin) = ch.stdin.take() {
                    let _ = stdin.write_all(&cfg);
                }
                o.pid = ch.id() as i32;
                o.child = Some(ch);
                o.rd = Some(r);
                self.ops.push(o);
            }
            Err(_) => q.push_back(Ev::failed_http(id, "cannot start curl")),
        }
        common::wipe(&mut cfg); // the key was in there
    }

    // ---- files ----

    pub fn read(&mut self, q: &mut VecDeque<Ev>, id: i64, p: &[u8]) {
        q.push_back(read_file(id, path(p)));
    }

    /// Write to a temporary file beside the target and rename it into place, so a crash or a full
    /// disk never leaves half a file. A symbolic link is followed, so the link stays.
    pub fn write(&mut self, q: &mut VecDeque<Ev>, id: i64, p: &[u8], data: &[u8]) {
        q.push_back(write_file(id, path(p), data));
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

    /// A cancelled timer is gone; a command or a request is asked to stop, and killed if it has
    /// not after a grace period. Only a background task still reports.
    pub fn cancel(&mut self, id: i64) {
        let Some(i) = self.ops.iter().position(|o| o.id == id) else { return };
        if self.ops[i].kind == Kind::Timer {
            self.ops.remove(i);
            return;
        }
        let o = &mut self.ops[i];
        o.cancelled = true;
        o.kill(SIGTERM);
        o.kill_at = now() + KILL_GRACE_MS;
    }

    /// The descriptors to watch for input.
    pub fn fill(&self, fds: &mut Vec<PollFd>) {
        for o in &self.ops {
            if let Some(r) = &o.rd {
                fds.push(PollFd { fd: r.as_raw_fd(), events: POLLIN, revents: 0 });
            }
        }
    }

    /// How long the loop may sleep: -1 if nothing is waiting on the clock.
    pub fn timeout_ms(&self) -> i64 {
        let now = now();
        let (mut best, mut fg, mut bg) = (-1i64, false, false);
        for o in &self.ops {
            if o.kind == Kind::Proc && o.bg {
                bg = true;
            } else if o.kind != Kind::Timer {
                fg = true;
            }
            let mut d = 0;
            if o.kind == Kind::Timer || (o.kind == Kind::Proc && o.deadline != 0 && !o.timed_out) {
                d = o.deadline;
            }
            if o.kill_at != 0 && (d == 0 || o.kill_at < d) {
                d = o.kill_at;
            }
            if d != 0 && (best < 0 || d - now < best) {
                best = (d - now).max(0);
            }
        }
        // Children are noticed by polling (a SIGCHLD handler would be quicker, but this cannot
        // miss one). A command the user is waiting for is checked often; a background task, which
        // only has to wake the agent when it ends, rarely; with nothing running there is none.
        if fg && (best < 0 || best > 50) {
            best = 50;
        } else if bg && (best < 0 || best > 250) {
            best = 250;
        }
        best
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
            o.drain();
            if o.status.is_none() {
                if let Some(Ok(Some(st))) = o.child.as_mut().map(|c| c.try_wait()) {
                    o.status = Some(st);
                }
            }
            if o.kind == Kind::Proc && o.status.is_none() && o.deadline != 0 && !o.timed_out && now >= o.deadline {
                o.timed_out = true;
                o.kill(SIGTERM);
                o.kill_at = now + KILL_GRACE_MS;
            }
            if o.status.is_none() && o.kill_at != 0 && now >= o.kill_at {
                o.kill(SIGKILL);
                o.kill_at = 0;
            }
            if o.status.is_some() {
                // Anything the command left running (a server it started) is no longer wanted,
                // unless this is a background task, which owns its group until it ends.
                if !o.bg {
                    o.kill(SIGKILL);
                }
                let o = self.ops.remove(i);
                finish(o, q);
                continue;
            }
            i += 1;
        }
    }

    /// Kill everything still running and remove the temporary files.
    pub fn shutdown(&mut self) {
        for o in &mut self.ops {
            if o.kind != Kind::Timer {
                o.kill(SIGKILL);
                if let Some(c) = o.child.as_mut() {
                    let _ = c.wait();
                }
            }
        }
        self.ops.clear();
    }
}

fn finish(mut o: Op, q: &mut VecDeque<Ev>) {
    o.drain();
    let ms = now() - o.started;
    let st = o.status.expect("finished");
    let sig = st.signal().is_some();
    let code = st.code().unwrap_or(-1);
    match o.kind {
        Kind::Proc if !o.cancelled || o.bg => {
            // (An interrupted foreground command has no one listening.)
            let out = std::mem::take(&mut o.out).take(); // (none for a background task: it went to the log)
            let exit = if o.timed_out {
                Exit::TimedOut
            } else if sig || o.cancelled {
                Exit::Signaled
            } else {
                Exit::Code(code)
            };
            q.push_back(Ev::Proc { id: o.id, exit, out, ms, note: Vec::new() });
        }
        Kind::Http if !o.cancelled => {
            let (status, body, note) = common::curl_reply(code, &o.body);
            q.push_back(Ev::Http { id: o.id, status, body, note });
        }
        _ => {}
    }
}

fn read_file(id: i64, p: &Path) -> Ev {
    let f = match File::open(p) {
        Ok(f) => f,
        Err(e) => return file_err(id, &e),
    };
    let md = match f.metadata() {
        Ok(m) => m,
        Err(e) => return Ev::file(id, Fs::Error, &strerror(&e)),
    };
    if md.is_dir() {
        return Ev::File { id, status: Fs::IsDir, data: Vec::new(), note: Vec::new() };
    }
    if md.is_file() && md.len() > READ_LIMIT {
        return Ev::file(id, Fs::TooBig, &format!("{} bytes, the limit is {}", md.len(), READ_LIMIT));
    }
    let mut b = Vec::new();
    let r = f.take(READ_LIMIT + 1).read_to_end(&mut b);
    if b.len() as u64 > READ_LIMIT {
        return Ev::file(id, Fs::TooBig, "more than the size limit");
    }
    if let Err(e) = r {
        return file_err(id, &e);
    }
    Ev::File { id, status: Fs::Ok, data: b, note: Vec::new() }
}

fn write_file(id: i64, p: &Path, data: &[u8]) -> Ev {
    let target = fs::canonicalize(p).unwrap_or_else(|_| p.to_path_buf());
    mkdir_p(&target);
    let mut tmp = target.clone().into_os_string();
    tmp.push(format!(".barb-tmp-{}", std::process::id()));
    let tmp = PathBuf::from(tmp);
    let mode = fs::metadata(&target).map(|m| m.permissions().mode() & 0o7777).unwrap_or(0o644);
    let mut f = match OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp) {
        Ok(f) => f,
        Err(e) => return file_err(id, &e),
    };
    let mut r = f.write_all(data);
    if r.is_ok() {
        r = f.set_permissions(fs::Permissions::from_mode(mode));
    }
    let closed = sys::close_fd(f.into_raw_fd());
    if r.is_ok() {
        r = closed;
    }
    if r.is_ok() {
        r = fs::rename(&tmp, &target);
    }
    match r {
        Ok(()) => Ev::File { id, status: Fs::Ok, data: Vec::new(), note: Vec::new() },
        Err(e) => {
            let _ = fs::remove_file(&tmp);
            file_err(id, &e)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(ops: &mut Ops, q: &mut VecDeque<Ev>, until: impl Fn(&VecDeque<Ev>) -> bool) {
        let t = Instant::now();
        while !until(q) {
            assert!(t.elapsed().as_secs() < 20, "timed out");
            let mut fds = Vec::new();
            ops.fill(&mut fds);
            let w = ops.timeout_ms();
            sys::wait(&mut fds, w.clamp(-1, 100) as i32);
            ops.service(q);
        }
    }

    fn dir(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("barb-ops-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn a_command_and_its_output() {
        let (mut ops, mut q) = (Ops::new(vec![], vec![]), VecDeque::new());
        ops.spawn(&mut q, 1, b"printf 'out '; printf 'err' >&2; exit 3", 0, false, b"");
        run(&mut ops, &mut q, |q| !q.is_empty());
        match q.pop_front().unwrap() {
            Ev::Proc { id: 1, exit: Exit::Code(3), out, .. } => assert_eq!(out, b"out err"),
            e => panic!("{:?}", e),
        }
        assert_eq!(ops.timeout_ms(), -1, "nothing left to wait for");
    }

    #[test]
    fn a_time_limit_kills_the_group() {
        let d = dir("limit");
        let (mut ops, mut q) = (Ops::new(vec![], vec![]), VecDeque::new());
        let cmd = format!("trap 'touch {}/term; exit 1' TERM; sleep 30 & wait", d.display());
        ops.spawn(&mut q, 2, cmd.as_bytes(), 300, false, b"");
        let t = Instant::now();
        run(&mut ops, &mut q, |q| !q.is_empty());
        assert!(t.elapsed().as_millis() < 3000);
        assert!(matches!(q.pop_front(), Some(Ev::Proc { id: 2, exit: Exit::TimedOut, .. })));
        assert!(d.join("term").exists(), "SIGTERM first");
    }

    #[test]
    fn a_cancelled_command_reports_only_in_the_background() {
        let d = dir("bg");
        let (mut ops, mut q) = (Ops::new(vec![], vec![]), VecDeque::new());
        ops.spawn(&mut q, 3, b"sleep 30", 0, false, b"");
        let log = d.join("tasks/x.log");
        ops.spawn(&mut q, 4, b"echo hi; sleep 30", 0, true, log.as_os_str().as_bytes());
        std::thread::sleep(std::time::Duration::from_millis(200));
        ops.cancel(3);
        ops.cancel(4);
        run(&mut ops, &mut q, |q| !q.is_empty());
        assert!(matches!(q.pop_front(), Some(Ev::Proc { id: 4, exit: Exit::Signaled, .. })));
        assert_eq!(fs::read(&log).unwrap(), b"hi\n");
        let t = Instant::now();
        while t.elapsed().as_millis() < 300 {
            ops.service(&mut q);
        }
        assert!(q.is_empty(), "no event for the foreground one");
    }

    #[test]
    fn unstartable_things_fail_at_once() {
        let (mut ops, mut q) = (Ops::new(vec![], vec![]), VecDeque::new());
        ops.spawn(&mut q, 5, b"true", 0, true, b"/nonexistent-dir/\0/x");
        assert!(matches!(q.pop_front(), Some(Ev::Proc { id: 5, exit: Exit::Failed, .. })));
        for i in 0..MAX_OPS as i64 {
            ops.timer(100 + i, 100000);
        }
        ops.spawn(&mut q, 6, b"true", 0, false, b"");
        match q.pop_front() {
            Some(Ev::Proc { id: 6, exit: Exit::Failed, note, .. }) => assert_eq!(note, b"too many operations running at once"),
            e => panic!("{:?}", e),
        }
        ops.http(&mut q, 7, b"/v1", b"{}");
        assert!(matches!(q.pop_front(), Some(Ev::Http { id: 7, status: 0, .. })));
        ops.cancel(100);
        assert_eq!(ops.ops.len(), MAX_OPS - 1, "a cancelled timer is gone");
    }

    #[test]
    fn timers_fire_in_order() {
        let (mut ops, mut q) = (Ops::new(vec![], vec![]), VecDeque::new());
        ops.timer(8, 120);
        ops.timer(9, 20);
        let w = ops.timeout_ms();
        assert!((0..=20).contains(&w), "{}", w);
        run(&mut ops, &mut q, |q| q.len() == 2);
        assert_eq!(q.pop_front(), Some(Ev::Timer { id: 9 }));
        assert_eq!(q.pop_front(), Some(Ev::Timer { id: 8 }));
    }

    #[test]
    fn files() {
        let d = dir("files");
        let p = d.join("sub/new.txt");
        let pb = p.as_os_str().as_bytes();
        let (mut ops, mut q) = (Ops::new(vec![], vec![]), VecDeque::new());
        ops.write(&mut q, 1, pb, b"hello");
        assert!(matches!(q.pop_front(), Some(Ev::File { id: 1, status: Fs::Ok, .. })));
        assert_eq!(fs::metadata(&p).unwrap().permissions().mode() & 0o777, 0o644, "a new file");
        fs::set_permissions(&p, fs::Permissions::from_mode(0o755)).unwrap();
        ops.write(&mut q, 2, pb, b"again");
        q.pop_front();
        assert_eq!(fs::metadata(&p).unwrap().permissions().mode() & 0o777, 0o755, "the mode is kept");
        std::os::unix::fs::symlink(&p, d.join("link")).unwrap();
        ops.write(&mut q, 3, d.join("link").as_os_str().as_bytes(), b"through");
        q.pop_front();
        assert!(fs::symlink_metadata(d.join("link")).unwrap().file_type().is_symlink(), "the link stays");
        assert_eq!(fs::read(&p).unwrap(), b"through");
        ops.read(&mut q, 4, pb);
        assert_eq!(q.pop_front(), Some(Ev::File { id: 4, status: Fs::Ok, data: b"through".to_vec(), note: vec![] }));
        ops.read(&mut q, 5, d.join("none").as_os_str().as_bytes());
        assert!(matches!(q.pop_front(), Some(Ev::File { id: 5, status: Fs::NotFound, .. })));
        ops.read(&mut q, 6, d.as_os_str().as_bytes());
        assert!(matches!(q.pop_front(), Some(Ev::File { id: 6, status: Fs::IsDir, .. })));
        ops.write(&mut q, 7, d.join("sub").as_os_str().as_bytes(), b"x");
        assert!(matches!(q.pop_front(), Some(Ev::File { id: 7, status: Fs::IsDir, .. }) | Some(Ev::File { id: 7, status: Fs::Error, .. })));
        let big = fs::File::create(d.join("big")).unwrap();
        big.set_len(READ_LIMIT + 1).unwrap();
        ops.read(&mut q, 8, d.join("big").as_os_str().as_bytes());
        match q.pop_front() {
            Some(Ev::File { id: 8, status: Fs::TooBig, note, .. }) => assert_eq!(note, b"8388609 bytes, the limit is 8388608"),
            e => panic!("{:?}", e),
        }
        // the temporary file sits beside its target, so a failed write over `sub` left it in `d`
        let left: Vec<_> = [d.clone(), d.join("sub")]
            .iter()
            .flat_map(|x| fs::read_dir(x).unwrap().map(|e| e.unwrap().file_name()))
            .filter(|n| n.to_string_lossy().contains(".barb-tmp"))
            .collect();
        assert!(left.is_empty(), "temporary files left: {:?}", left);
    }
}
