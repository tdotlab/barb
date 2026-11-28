//! barb for Windows: the command line, the console, and the loop that joins the agent to the system.
//!
//! The same program as posix/, on Win32. The loop sleeps in WaitForMultipleObjects on the
//! console, the operations' pipes and processes, and an event the Ctrl-C handler sets, with a
//! timeout only when a timer or a time limit is due: an agent waiting an hour for a timer or a
//! background task uses no CPU in the meantime.
//!
//! The console is put in raw mode for an interactive session: key records arrive one at a time
//! and go to the screen (barb-ui), which keeps the prompt, what is going on and a footer at the
//! bottom and draws them with VT sequences. Text goes to the console as UTF-16 (WriteConsoleW),
//! so it shows right whatever the console's code page; to a pipe or a file it goes as the UTF-8
//! the agent made. Standard input that is not ours to edit (a pipe, a file, a console in line
//! mode) is read by a thread and handed to the loop.

mod ops;
mod sys;

use crate::common::{self, Ev, Parsed, DEFAULT_MODEL, NOT_ALLOWED, VERSION};
use barb_core::agent::{Action, Agent, Answer, Config as AgentConfig, Style};
use barb_ui::edit::Key;
use barb_ui::screen::{Config as ScreenConfig, Mode, Screen};
use ops::Ops;
use std::collections::VecDeque;
use std::ffi::{c_void, OsString};
use std::io::Read;
use std::ptr::{null, null_mut};
use std::sync::atomic::{AtomicBool, AtomicIsize, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use sys::*;

// ---- Ctrl-C, Ctrl-Break and closing the window ----

static WAKE: AtomicIsize = AtomicIsize::new(0); // set by the handler
static CLEANED: AtomicIsize = AtomicIsize::new(0); // set when barb has tidied up, for the close handler
static INTERRUPTS: AtomicU32 = AtomicU32::new(0);
static TERMINATE: AtomicBool = AtomicBool::new(false);
/// The consoles' modes before barb changed them, for a panic to put back.
static MODES: Mutex<Vec<(HANDLE, u32)>> = Mutex::new(Vec::new());

/// Runs on a thread of Windows' own.
unsafe extern "system" fn on_ctrl(kind: u32) -> BOOL {
    if kind == CTRL_C_EVENT || kind == CTRL_BREAK_EVENT {
        INTERRUPTS.fetch_add(1, Ordering::SeqCst);
        // SAFETY: the event lives as long as the process.
        unsafe { SetEvent(WAKE.load(Ordering::SeqCst)) };
        return 1;
    }
    // The window is closing, or the user is logging off: stop, the way SIGTERM stops barb
    // elsewhere. Windows ends the process when this returns, so wait (briefly) for the main
    // thread to kill the commands and remove the temporary files first.
    TERMINATE.store(true, Ordering::SeqCst);
    // SAFETY: as above.
    unsafe {
        SetEvent(WAKE.load(Ordering::SeqCst));
        WaitForSingleObject(CLEANED.load(Ordering::SeqCst), 4000);
    }
    1
}

fn restore_modes() {
    if let Ok(m) = MODES.lock() {
        for &(h, mode) in m.iter() {
            // SAFETY: a standard handle, which stays open.
            unsafe { SetConsoleMode(h, mode) };
        }
    }
}

// ---- output ----

/// To a console as UTF-16, in pieces that keep surrogate pairs whole; anywhere else as it is.
fn write_handle(h: HANDLE, con: bool, b: &[u8]) {
    if b.is_empty() || h == 0 || h == INVALID_HANDLE_VALUE {
        return;
    }
    if !con {
        let mut p = b;
        while !p.is_empty() {
            let chunk = p.len().min(1 << 20) as u32;
            let mut w = 0u32;
            // SAFETY: p has at least `chunk` bytes.
            if unsafe { WriteFile(h, p.as_ptr(), chunk, &mut w, null_mut()) } == 0 || w == 0 {
                return;
            }
            p = &p[w as usize..];
        }
        return;
    }
    let w = utf16(b);
    let mut at = 0;
    while at < w.len() {
        let mut chunk = (w.len() - at).min(8192);
        if at + chunk < w.len() && (0xD800..=0xDBFF).contains(&w[at + chunk - 1]) {
            chunk -= 1;
        }
        let mut wrote = 0u32;
        // SAFETY: w[at..at + chunk] is in bounds.
        if unsafe { WriteConsoleW(h, w[at..].as_ptr(), chunk as u32, &mut wrote, null_mut()) } == 0 || wrote == 0 {
            break;
        }
        at += wrote as usize;
    }
}

fn std_handle(which: u32) -> (HANDLE, bool, u32) {
    // SAFETY: no pointers.
    let h = unsafe { GetStdHandle(which) };
    let mut mode = 0u32;
    // SAFETY: mode is a valid out-pointer; a handle that is not a console just fails.
    let con = h != 0 && h != INVALID_HANDLE_VALUE && unsafe { GetConsoleMode(h, &mut mode) } != 0;
    (h, con, mode)
}

fn console_width(h: HANDLE, con: bool) -> usize {
    let mut info = ScreenBufferInfo::default();
    // SAFETY: info is a valid out-pointer.
    if con && unsafe { GetConsoleScreenBufferInfo(h, &mut info) } != 0 && info.size_x > 0 {
        info.size_x as usize
    } else {
        80
    }
}

fn is_text_key(r: &InputRecord) -> bool {
    r.event_type == KEY_EVENT && r.key_down != 0 && r.ch >= 32
}

/// Is `ch` the LF of a CR LF, which is one Enter, not two? `last` is the character before it,
/// and is brought up to date. (A pseudo console turns that LF into a second CR itself, so only a
/// console that passes LF through, as conhost's own input can, reaches this.)
fn lf_after_cr(last: &mut u32, ch: u32) -> bool {
    if ch == 10 && *last == 13 {
        *last = 0;
        return true;
    }
    *last = ch;
    false
}

// ---- standard input that is not ours to edit ----

/// What the reader thread has read so far, and whether it has reached the end.
type Pending = Arc<Mutex<(Vec<u8>, bool)>>;

fn reader(h: HANDLE, con: bool, ready: HANDLE, pending: Pending) {
    let add = |b: &[u8], eof: bool| {
        if let Ok(mut p) = pending.lock() {
            p.0.extend_from_slice(b);
            p.1 |= eof;
        }
        // SAFETY: the event lives as long as the process.
        unsafe { SetEvent(ready) };
    };
    if con {
        // a console in line mode: lines in UTF-16, Ctrl-Z at the start of one ends input
        let mut w = [0u16; 4096];
        loop {
            let mut got = 0u32;
            // SAFETY: w has the room given.
            if unsafe { ReadConsoleW(h, w.as_mut_ptr(), w.len() as u32, &mut got, null_mut()) } == 0 || got == 0 || w[0] == 26 {
                break;
            }
            add(&utf8(&w[..got as usize]), false);
        }
    } else {
        let mut b = vec![0u8; 65536];
        loop {
            let mut got = 0u32;
            // SAFETY: b has the room given.
            if unsafe { ReadFile(h, b.as_mut_ptr(), b.len() as u32, &mut got, null_mut()) } == 0 || got == 0 {
                break;
            }
            add(&b[..got as usize], false);
        }
    }
    add(&[], true);
}

/// The task from standard input (-p -): from a console, until Ctrl-Z.
fn read_all_stdin(h: HANDLE, con: bool) -> Vec<u8> {
    let mut b = Vec::new();
    if con {
        let mut w = [0u16; 4096];
        loop {
            let mut got = 0u32;
            // SAFETY: w has the room given.
            if unsafe { ReadConsoleW(h, w.as_mut_ptr(), w.len() as u32, &mut got, null_mut()) } == 0 || got == 0 {
                break;
            }
            let got = got as usize;
            let end = w[..got].iter().position(|&c| c == 26).unwrap_or(got);
            b.extend(utf8(&w[..end]));
            if end < got {
                break;
            }
        }
    } else {
        let _ = std::io::stdin().lock().read_to_end(&mut b);
    }
    b
}

// ---- the program ----

struct App {
    agent: Agent,
    scr: Screen,
    ops: Ops,
    q: VecDeque<Ev>,
    done: bool,
    exit_code: i32,
    awaiting: i64, // the question the user is being asked, if any
    interactive: bool, // the console is ours: raw keys, the line editor
    oneshot: bool, // -p
    stdin_open: bool,
    h_in: HANDLE,
    h_out: HANDLE,
    h_err: HANDLE,
    con_out: bool,
    con_err: bool,
    wake: HANDLE,
    // the keyboard
    paste: Vec<u8>, // a pasted block of lines, so far
    batch: Vec<InputRecord>, // the console records being handled
    batch_i: usize,
    high_surrogate: u16,
    last_ch: u32,
    // line mode
    in_ready: HANDLE, // set by the reader thread; 0 if there is none
    pending: Pending,
    line: Vec<u8>,
    cleaned_up: bool,
}

const LINE_MAX: usize = 65536;

impl App {
    /// What the screen has to write, written: when the loop is about to wait, and before anything
    /// else writes. A frame of the bottom block is many small pieces, and a console shows each
    /// write as it comes.
    fn flush(&mut self) {
        for (fd, b) in self.scr.take_out() {
            if fd == 1 {
                write_handle(self.h_out, self.con_out, &b);
            } else {
                write_handle(self.h_err, self.con_err, &b);
            }
        }
    }

    fn push(&mut self, ev: Ev) {
        self.q.push_back(ev);
    }

    fn draw(&mut self) {
        let prompt_ok = self.interactive && self.awaiting == 0 && self.stdin_open && !self.done;
        let status_ok = self.awaiting == 0 && !self.done;
        self.scr.draw(now(), self.agent.busy(), prompt_ok, status_ok);
    }

    // ---- the keyboard ----

    fn answer(&mut self, ans: Answer) {
        let id = self.awaiting;
        self.awaiting = 0;
        self.scr.answer(ans);
        self.push(Ev::Answer { id, answer: ans });
    }

    // A paste arrives as keystrokes with an Enter after every line. Lines that are followed at
    // once by more text belong to one message, so they are gathered here and delivered together,
    // with the newlines the user pasted, when a line ends with nothing more waiting behind it.

    /// Is there typed text already waiting behind the key being handled? The rest of the batch
    /// being read, then the console's own queue. Part-way through a paste the next chunk may
    /// still be on its way, so it is given a moment.
    fn more_text_waiting(&self) -> bool {
        if self.batch.iter().skip(self.batch_i + 1).any(is_text_key) {
            return true;
        }
        for tries in 0..2 {
            let mut pk = [InputRecord::default(); 64];
            let mut got = 0u32;
            // SAFETY: pk has room for 64 records.
            if unsafe { PeekConsoleInputW(self.h_in, pk.as_mut_ptr(), 64, &mut got) } != 0
                && pk[..got as usize].iter().any(is_text_key)
            {
                return true;
            }
            if self.paste.is_empty() || tries > 0 {
                break;
            }
            // SAFETY: the console input handle stays open.
            unsafe { WaitForSingleObject(self.h_in, 40) };
        }
        false
    }

    fn enter(&mut self) {
        let line = self.scr.take();
        if self.more_text_waiting() {
            self.paste.extend_from_slice(&line);
            self.paste.push(b'\n');
            return;
        }
        if self.paste.is_empty() {
            self.push(Ev::Input(line));
            return;
        }
        let mut p = std::mem::take(&mut self.paste);
        p.extend_from_slice(&line);
        self.push(Ev::Input(p));
    }

    /// One key: `ch` is the character it typed (0 for none), `vk` the key, `ctrl` whether Ctrl
    /// was down.
    fn key(&mut self, ch: u32, vk: u16, ctrl: bool) {
        if self.awaiting != 0 {
            // one key answers; the line being typed waits
            match ch {
                0x79 | 0x59 => self.answer(Answer::Yes),           // y
                0x61 | 0x41 => self.answer(Answer::Always),        // a
                0x6E | 0x4E | 13 | 10 | 27 | 4 => self.answer(Answer::No), // n, Enter, Escape, Ctrl-D
                3 => self.push(Ev::Interrupt),
                _ => {}
            }
            return;
        }
        if ch == 0 {
            let k = match vk {
                VK_LEFT if ctrl => Key::WordLeft,
                VK_LEFT => Key::Left,
                VK_RIGHT if ctrl => Key::WordRight,
                VK_RIGHT => Key::Right,
                VK_HOME => Key::Home,
                VK_END => Key::End,
                VK_DELETE => Key::Del,
                VK_UP => Key::Up,
                VK_DOWN => Key::Down,
                _ => return,
            };
            self.scr.key(k);
            return;
        }
        let k = match ch {
            3 => {
                // Ctrl-C: stop the work; at an idle prompt, throw away what was typed first
                if !self.agent.busy() && !self.scr.ed.line.is_empty() {
                    self.scr.key(Key::Clear);
                } else {
                    self.push(Ev::Interrupt);
                }
                return;
            }
            4 => {
                // Ctrl-D: on an empty line, the end; otherwise delete forward, as in a shell
                if !self.scr.ed.line.is_empty() {
                    Key::Del
                } else {
                    self.scr.hide();
                    self.stdin_open = false;
                    self.push(Ev::Eof);
                    return;
                }
            }
            13 | 10 => {
                self.enter();
                return;
            }
            8 => Key::Back,
            0x7F => Key::KillWord, // Ctrl-Backspace
            1 => Key::Home,        // Ctrl-A
            5 => Key::End,         // Ctrl-E
            2 => Key::Left,        // Ctrl-B
            6 => Key::Right,       // Ctrl-F
            11 => Key::KillEnd,    // Ctrl-K
            21 => Key::KillStart,  // Ctrl-U
            23 => Key::KillWord,   // Ctrl-W
            16 => Key::Up,         // Ctrl-P
            14 => Key::Down,       // Ctrl-N
            27 => Key::Clear,      // Escape
            c => Key::Char(c),
        };
        self.scr.key(k);
    }

    fn console_record(&mut self, r: &InputRecord) {
        if r.event_type == WINDOW_BUFFER_SIZE_EVENT {
            let w = console_width(self.h_err, self.con_err);
            if w != self.scr.width {
                self.scr.resize(w);
            }
            return;
        }
        if r.event_type != KEY_EVENT {
            return;
        }
        let wc = r.ch;
        // Alt+numpad and some input methods deliver their character on the key's release.
        if r.key_down == 0 && !(r.vk == VK_MENU && wc != 0) {
            return;
        }
        let ctrl = r.ctrl_state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED) != 0;
        let mut ch = wc as u32;
        if (0xD800..=0xDBFF).contains(&wc) {
            self.high_surrogate = wc;
            return;
        }
        if (0xDC00..=0xDFFF).contains(&wc) {
            if self.high_surrogate == 0 {
                return;
            }
            ch = 0x10000 + ((self.high_surrogate as u32 - 0xD800) << 10) + (wc as u32 - 0xDC00);
        }
        self.high_surrogate = 0;
        let repeat = r.repeat.max(1);
        for _ in 0..repeat {
            if self.done || !self.stdin_open {
                break;
            }
            if lf_after_cr(&mut self.last_ch, ch) {
                continue;
            }
            self.key(ch, r.vk, ctrl);
        }
    }

    /// Whatever the console has for us, without waiting.
    fn read_console(&mut self) {
        let mut recs = vec![InputRecord::default(); 512];
        while self.stdin_open && !self.done {
            let (mut avail, mut got) = (0u32, 0u32);
            // SAFETY: out-pointers to locals; recs has room for 512 records.
            if unsafe { GetNumberOfConsoleInputEvents(self.h_in, &mut avail) } == 0
                || avail == 0
                || unsafe { ReadConsoleInputW(self.h_in, recs.as_mut_ptr(), 512, &mut got) } == 0
                || got == 0
            {
                break;
            }
            self.batch = recs[..got as usize].to_vec();
            let mut i = 0;
            while i < self.batch.len() && self.stdin_open && !self.done {
                self.batch_i = i;
                let r = self.batch[i];
                self.console_record(&r);
                i += 1;
            }
            self.batch.clear();
        }
    }

    // ---- lines ----

    fn deliver_line(&mut self) {
        let mut l = std::mem::take(&mut self.line);
        while l.last() == Some(&b'\r') {
            l.pop();
        }
        self.push(Ev::Input(l));
    }

    fn read_lines(&mut self) {
        let (data, eof) = {
            let Ok(mut p) = self.pending.lock() else { return };
            // SAFETY: the event lives as long as the process. Reset under the lock: whatever
            // comes after this sets it again.
            unsafe { ResetEvent(self.in_ready) };
            (std::mem::take(&mut p.0), p.1)
        };
        for &c in &data {
            if c == b'\n' {
                self.deliver_line();
                continue;
            }
            self.line.push(c);
            if self.line.len() == LINE_MAX - 1 {
                self.deliver_line(); // one enormous line
            }
        }
        if eof && self.stdin_open {
            self.stdin_open = false;
            if !self.line.is_empty() {
                self.deliver_line();
            }
            self.push(Ev::Eof);
        }
    }

    // ---- carrying out actions ----

    fn ask(&mut self, id: i64, tool: &[u8], what: &[u8]) {
        if self.oneshot || !self.interactive {
            self.scr.say(Style::Error, NOT_ALLOWED);
            self.push(Ev::Answer { id, answer: Answer::No });
            return;
        }
        self.read_console(); // keys typed before the question go to the line, so they cannot answer it
        self.scr.question(tool, what);
        self.awaiting = id;
    }

    /// Every action is shown to the screen first (Say is only that), then carried out.
    fn exec(&mut self, a: &Action) {
        self.scr.action(a, now());
        match a {
            Action::Say { .. } => {}
            Action::Http { id, path, body } => self.ops.http(&mut self.q, *id, path.as_bytes(), body),
            Action::Read { id, path } => self.ops.read(&mut self.q, *id, path),
            Action::Write { id, path, data } => self.ops.write(&mut self.q, *id, path, data),
            Action::Spawn { id, cmd, timeout_ms, background, log } => {
                self.ops.spawn(&mut self.q, *id, cmd, *timeout_ms, *background, log)
            }
            Action::Timer { id, ms } => self.ops.timer(*id, *ms),
            Action::Cancel { id } => {
                if *id == self.awaiting {
                    self.awaiting = 0;
                    self.scr.hide();
                } else {
                    self.ops.cancel(*id);
                }
            }
            Action::Ask { id, tool, what } => self.ask(*id, tool, what),
            Action::Done { status } => {
                self.done = true;
                self.exit_code = *status;
            }
        }
    }

    fn step(&mut self, ev: &Ev) {
        let e = ev.event();
        self.scr.event(&e);
        for a in self.agent.step(&e) {
            self.exec(&a);
        }
    }

    fn drain_queue(&mut self) {
        while !self.done {
            let Some(ev) = self.q.pop_front() else { break };
            self.step(&ev);
        }
    }

    fn take_signals(&mut self) {
        for _ in 0..INTERRUPTS.swap(0, Ordering::SeqCst) {
            self.push(Ev::Interrupt);
        }
        if TERMINATE.load(Ordering::SeqCst) {
            self.done = true;
            self.exit_code = 143;
        }
    }

    // ---- the loop ----

    fn run(&mut self) {
        let mut h: Vec<HANDLE> = Vec::with_capacity(MAXIMUM_WAIT_OBJECTS);
        loop {
            self.take_signals();
            self.drain_queue();
            if self.done {
                return;
            }
            self.draw();
            self.flush();
            h.clear();
            h.push(self.wake);
            let (mut console, mut lines) = (false, false);
            if self.stdin_open && !self.oneshot {
                if self.interactive {
                    console = true;
                    h.push(self.h_in);
                } else if self.in_ready != 0 {
                    lines = true;
                    h.push(self.in_ready);
                }
            }
            self.ops.handles(&mut h, MAXIMUM_WAIT_OBJECTS);
            let mut timeout = self.ops.timeout_ms();
            let anim = self.scr.next_ms(now()); // the spinner's next frame
            if anim >= 0 && (timeout < 0 || timeout > anim) {
                timeout = anim;
            }
            if !self.q.is_empty() {
                timeout = 0;
            }
            let ms = if timeout < 0 { INFINITE } else { timeout.min(INFINITE as i64 - 1) as u32 };
            // SAFETY: every handle listed is open: the operations' stay open while they are listed.
            unsafe { WaitForMultipleObjects(h.len() as u32, h.as_ptr(), 0, ms) };
            // Whatever woke the loop, look at everything that might be ready; each check is cheap.
            if console {
                self.read_console();
            }
            // SAFETY: the event lives as long as the process.
            if lines && unsafe { WaitForSingleObject(self.in_ready, 0) } == WAIT_OBJECT_0 {
                self.read_lines();
            }
            self.ops.service(&mut self.q);
        }
    }

    fn cleanup(&mut self) {
        if self.cleaned_up {
            return;
        }
        self.cleaned_up = true;
        self.scr.hide();
        self.flush();
        self.ops.shutdown();
        restore_modes();
        // SAFETY: the event lives as long as the process.
        unsafe { SetEvent(CLEANED.load(Ordering::SeqCst)) };
    }
}

// ---- starting up ----

fn usage(h: HANDLE, con: bool) {
    let text = format!(
        "barb {}: a coding agent for the terminal\n\n\
         usage: barb [options]\n       barb -p TASK [options]\n\n\
         \x20 -p, --prompt TASK   do one task and exit (TASK of - reads it from standard input)\n\
         \x20 -m, --model NAME    the model (default {}, or %BARB_MODEL%)\n\
         \x20 -y, --yes           do not ask before writing files or running commands\n\
         \x20 -C, --cwd DIR       work in DIR\n\
         \x20     --max-turns N   stop after N model calls without you typing (default 60)\n\
         \x20     --no-instructions  do not read AGENTS.md or CLAUDE.md from the working directory\n\
         \x20 -V, --version\n\
         \x20 -h, --help\n\n\
         environment: ANTHROPIC_API_KEY (required), ANTHROPIC_BASE_URL, BARB_MODEL,\n\
         \x20            BARB_SHELL (the bash.exe to run commands in; Git for Windows' is found by itself)\n",
        VERSION, DEFAULT_MODEL
    );
    write_handle(h, con, text.as_bytes());
}

fn bytes(s: &std::ffi::OsStr) -> Vec<u8> {
    s.to_string_lossy().into_owned().into_bytes()
}

fn platform(shell: Option<&str>) -> String {
    type Rtl = unsafe extern "system" fn(*mut OsVersionInfoW) -> i32;
    let mut v = OsVersionInfoW { size: std::mem::size_of::<OsVersionInfoW>() as u32, major: 0, minor: 0, build: 0, platform: 0, csd: [0; 128] };
    let nt = wide("ntdll.dll");
    // SAFETY: the names are NUL-terminated; RtlGetVersion has this signature.
    unsafe {
        let m = GetModuleHandleW(nt.as_ptr());
        let f = if m != 0 { GetProcAddress(m, b"RtlGetVersion\0".as_ptr()) } else { null() };
        if f.is_null() || std::mem::transmute::<*const c_void, Rtl>(f)(&mut v) != 0 {
            v.major = 0;
        }
    }
    // SAFETY: an all-zero SYSTEM_INFO is valid; the call fills it in.
    let mut si: SystemInfo = unsafe { std::mem::zeroed() };
    // SAFETY: si is a valid out-pointer.
    unsafe { GetNativeSystemInfo(&mut si) };
    let arch = match si.arch {
        PROCESSOR_ARCHITECTURE_AMD64 => "x64",
        PROCESSOR_ARCHITECTURE_ARM64 => "arm64",
        PROCESSOR_ARCHITECTURE_INTEL => "x86",
        _ => "unknown",
    };
    let name = if v.major == 10 && v.build >= 22000 {
        format!("Windows 11 (build {})", v.build)
    } else if v.major != 0 {
        format!("Windows {}.{} (build {})", v.major, v.minor, v.build)
    } else {
        "Windows".to_string()
    };
    if shell.is_some() {
        format!(
            "{} {}. Commands run in Git Bash (bash -c, not sh), where paths are written /c/Users/...; \
             the file tools take C:/Users/... or /c/Users/.... Windows programs (cmd /c, powershell -Command, \
             py, where.exe) can be run from bash too",
            name, arch
        )
    } else {
        format!("{} {}. Commands cannot run: no Git Bash was found", name, arch)
    }
}

pub fn main() -> i32 {
    let (h_in, con_in, mode_in) = std_handle(STD_INPUT_HANDLE);
    let (h_out, con_out, mode_out) = std_handle(STD_OUTPUT_HANDLE);
    let (h_err, con_err, mode_err) = std_handle(STD_ERROR_HANDLE);
    let err = |s: &[u8]| write_handle(h_err, con_err, s);

    let argv: Vec<OsString> = std::env::args_os().skip(1).collect();
    let args = match common::parse(&argv) {
        Parsed::Help => {
            usage(h_out, con_out);
            return 0;
        }
        Parsed::Version => {
            write_handle(h_out, con_out, format!("barb {}\n", VERSION).as_bytes());
            return 0;
        }
        Parsed::Bad(a) => {
            let mut m = b"barb: unknown or incomplete option ".to_vec();
            m.extend(bytes(&a));
            m.extend_from_slice(b"\n\n");
            err(&m);
            usage(h_err, con_err);
            return 2;
        }
        Parsed::Run(a) => a,
    };
    let mut model = args.model.as_deref().or(std::env::var_os("BARB_MODEL").as_deref()).map(bytes).unwrap_or_default();
    if model.is_empty() {
        model = DEFAULT_MODEL.as_bytes().to_vec();
    }

    // Take the key out of the environment: every command the model runs inherits it otherwise,
    // and `echo $ANTHROPIC_API_KEY` would show it. Only the platform layer holds it from here.
    let key = std::env::var_os("ANTHROPIC_API_KEY").map(|k| bytes(&k)).unwrap_or_default();
    std::env::remove_var("ANTHROPIC_API_KEY");
    if key.is_empty() {
        err(b"barb: ANTHROPIC_API_KEY is not set.\n");
        return 2;
    }
    let (base, warn) = common::base_url(std::env::var("ANTHROPIC_BASE_URL").ok());
    if warn {
        err(common::PLAIN_HTTP_WARNING.as_bytes());
    }

    if let Some(d) = &args.cwd {
        if std::env::set_current_dir(d).is_err() {
            let mut m = b"barb: cannot change to ".to_vec();
            m.extend(bytes(d));
            m.push(b'\n');
            err(&m);
            return 2;
        }
    }
    let cwd = std::env::current_dir().map(|p| bytes(p.as_os_str())).unwrap_or_else(|_| b".".to_vec());
    let date = local_time(0).map(|(y, m, d, _, _)| common::ymd(y, m, d)).unwrap_or_default();

    let prompt = match args.prompt {
        Some(p) if p == "-" => Some(read_all_stdin(h_in, con_in)),
        p => p.map(|p| bytes(&p)),
    };
    let oneshot = prompt.is_some();

    // VT sequences on the console, for the colours and the line editor. A console without them
    // (very old Windows) is treated like a pipe: plain text, no editor.
    let set = |h: HANDLE, mode: u32, new: u32| -> bool {
        // SAFETY: a standard handle.
        let ok = unsafe { SetConsoleMode(h, new) } != 0;
        if ok {
            MODES.lock().map(|mut m| m.push((h, mode))).ok();
        }
        ok
    };
    let vt = ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    let restore_out = con_out && set(h_out, mode_out, mode_out | vt);
    let restore_err = con_err && set(h_err, mode_err, mode_err | vt);
    let tty_out = con_err && restore_err;
    let raw = (mode_in & !(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)) | ENABLE_WINDOW_INPUT;
    let interactive = !oneshot && con_in && tty_out && set(h_in, mode_in, raw);
    if interactive {
        let default = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            restore_modes();
            default(info);
        }));
    }

    // SAFETY: plain values; unnamed events.
    let (wake, cleaned) = unsafe { (CreateEventW(null_mut(), 0, 0, null()), CreateEventW(null_mut(), 1, 0, null())) };
    if wake == 0 || cleaned == 0 {
        err(b"barb: cannot create an event\n");
        restore_modes();
        return 1;
    }
    WAKE.store(wake, Ordering::SeqCst);
    CLEANED.store(cleaned, Ordering::SeqCst);
    // SAFETY: on_ctrl has the signature a handler needs and lives as long as the process.
    unsafe {
        SetConsoleCtrlHandler(None, 0); // a parent may have told this process to ignore Ctrl-C
        SetConsoleCtrlHandler(Some(on_ctrl), 1);
    }

    let ops = Ops::new(base.into_bytes(), key);
    let platform = platform(ops.shell());

    // The screen: the whole bottom block for an interactive session, a status line on a console
    // that is not ours to edit, plain text for pipes and files. Windows Terminal's fonts have the
    // braille for the spinner; the old console's may not.
    let home = std::env::var_os("USERPROFILE").map(|h| bytes(&h));
    let set_var = |v: &str| std::env::var_os(v).is_some_and(|s| !s.is_empty());
    let mut app = App {
        agent: Agent::new(AgentConfig {
            model: model.clone(),
            max_tokens: 0,
            cwd: cwd.clone(),
            platform: platform.into_bytes(),
            date: date.into_bytes(),
            auto_approve: args.yes,
            max_turns: args.max_turns.max(0) as u32,
            instructions: !args.no_instructions,
        }),
        scr: Screen::new(ScreenConfig {
            mode: if interactive {
                Mode::Region
            } else if tty_out {
                Mode::Lite
            } else {
                Mode::Pipe
            },
            vt_out: con_out && restore_out,
            vt_err: tty_out,
            fancy: set_var("WT_SESSION") || set_var("TERM_PROGRAM"),
            ascii: false,
            width: console_width(h_err, con_err),
            model: &model,
            cwd: &cwd,
            home: home.as_deref(),
            clock: Some(Box::new(|at: i64| local_time(at - now()).map(|(_, _, _, h, m)| common::hhmm(h, m)).unwrap_or_default())),
        }),
        ops,
        q: VecDeque::new(),
        done: false,
        exit_code: 0,
        awaiting: 0,
        interactive,
        oneshot,
        stdin_open: true,
        h_in,
        h_out,
        h_err,
        con_out,
        con_err,
        wake,
        paste: Vec::new(),
        batch: Vec::new(),
        batch_i: 0,
        high_surrogate: 0,
        last_ch: 0,
        in_ready: 0,
        pending: Arc::new(Mutex::new((Vec::new(), false))),
        line: Vec::new(),
        cleaned_up: false,
    };

    match prompt {
        None => {
            if !interactive && h_in != 0 && h_in != INVALID_HANDLE_VALUE {
                // SAFETY: plain values; manual reset, so the wait does not use it up.
                let ready = unsafe { CreateEventW(null_mut(), 1, 0, null()) };
                let pending = app.pending.clone();
                let started = ready != 0
                    && std::thread::Builder::new().spawn(move || reader(h_in, con_in, ready, pending)).is_ok();
                if started {
                    app.in_ready = ready;
                } else {
                    app.stdin_open = false;
                }
            } else if !interactive {
                app.stdin_open = false;
            }
            app.scr.banner(VERSION.as_bytes());
            if app.ops.shell().is_none() {
                app.scr.say(
                    Style::Error,
                    b"barb: Git Bash was not found, so commands cannot run. Install Git for Windows, or set BARB_SHELL to a bash.exe.",
                );
            }
            if !app.stdin_open {
                app.push(Ev::Eof);
            }
        }
        Some(p) => {
            app.push(Ev::Input(p));
            app.push(Ev::Eof);
        }
    }
    app.run();
    app.cleanup();
    // The reader thread may still be blocked on standard input; ending the process ends it.
    app.exit_code
}

#[cfg(test)]
mod tests {
    use super::lf_after_cr;

    #[test]
    fn cr_lf_is_one_enter() {
        let mut last = 0;
        let typed: Vec<u32> = [b'a', 13, 10, b'b', 13, 13, 10, 10, 10]
            .iter()
            .map(|&c| c as u32)
            .filter(|&c| !lf_after_cr(&mut last, c))
            .collect();
        // a lone LF, and a LF after one that was dropped, are Enters of their own
        assert_eq!(typed, [b'a' as u32, 13, b'b' as u32, 13, 13, 10, 10]);
    }
}
