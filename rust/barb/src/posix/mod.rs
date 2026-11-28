//! barb for Linux and macOS: the command line, and the loop that joins the agent to the system.
//!
//! This is the only place that touches the terminal. The agent (barb-core) decides what to do;
//! ops.rs does it; this loop waits for the next thing to happen (the user typing, a process or
//! reply arriving, a timer) and hands it to the agent. When nothing is happening it sleeps in
//! select() (Apple's poll() cannot watch a terminal): waiting costs nothing.
//!
//! An interactive session on a terminal puts it in raw mode: bytes arrive as they are typed, the
//! key decoder (barb-ui's keys) turns them into keys, and the screen keeps the prompt, what is
//! going on and a footer at the bottom. The terminal is asked to bracket pastes, so a pasted block
//! of lines is one message. Anything else (a pipe, a file, a terminal with no use for escapes) is
//! read as lines.

mod ops;
mod sys;

use crate::common::{self, Ev, Parsed, DEFAULT_MODEL, NOT_ALLOWED, VERSION};
use barb_core::agent::{Action, Agent, Answer, Config as AgentConfig, Style};
use barb_ui::edit::Key;
use barb_ui::keys::{Decoder, Input};
use barb_ui::screen::{Config as ScreenConfig, Mode, Screen};
use ops::Ops;
use std::collections::VecDeque;
use std::ffi::OsString;
use std::io::{ErrorKind, IsTerminal, Read, Write};
use std::mem::ManuallyDrop;
use std::os::raw::c_int;
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::io::{FromRawFd, IntoRawFd};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::OnceLock;
use sys::{PollFd, Termios, POLLERR, POLLHUP, POLLIN};

// ---- signals and the terminal, as the handlers and a panic see them ----

static SIG_W: AtomicI32 = AtomicI32::new(-1);
static COOKED: OnceLock<Termios> = OnceLock::new();
static RAW_ON: AtomicBool = AtomicBool::new(false);

extern "C" fn on_signal(sig: c_int) {
    let c = match sig {
        sys::SIGINT => b'i',
        sys::SIGWINCH => b'w',
        sys::SIGTSTP => b'z',
        sys::SIGCONT => b'c',
        _ => b't',
    };
    let saved = sys::errno();
    sys::write_raw(SIG_W.load(Ordering::Relaxed), &[c]);
    sys::set_errno(saved);
}

fn catch(sig: c_int) {
    sys::on_signal(sig, on_signal as extern "C" fn(c_int) as usize);
}

/// Wall-clock milliseconds, which the screen shows as times of day.
fn now_ms() -> i64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0)
}

fn fd_file(fd: c_int) -> ManuallyDrop<std::fs::File> {
    // SAFETY: fd is one of the standard descriptors, which stays open; ManuallyDrop never closes it.
    ManuallyDrop::new(unsafe { std::fs::File::from_raw_fd(fd) })
}

fn write_all(fd: c_int, b: &[u8]) {
    let _ = fd_file(fd).write_all(b);
}

fn read_fd(fd: c_int, buf: &mut [u8]) -> std::io::Result<usize> {
    fd_file(fd).read(buf)
}

fn term_width() -> usize {
    sys::columns(2).unwrap_or(80)
}

/// The terminal in raw mode, from its cooked settings (see [`App::term_raw`]).
fn set_raw(cooked: &Termios) -> bool {
    let mut t = *cooked;
    t.lflag &= !(sys::ICANON | sys::ECHO | sys::IEXTEN);
    t.iflag &= !(sys::ICRNL | sys::INLCR | sys::IGNCR | sys::IXON);
    t.cc[sys::VMIN] = 1;
    t.cc[sys::VTIME] = 0;
    t.cc[sys::VQUIT] = sys::VDISABLE;
    sys::set_term(0, &t)
}

fn restore_after_panic() {
    if RAW_ON.load(Ordering::Relaxed) {
        write_all(2, b"\x1b[?2004l");
        if let Some(t) = COOKED.get() {
            sys::set_term(0, t);
        }
    }
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
    tty_in: bool,
    oneshot: bool, // -p: no prompts, no reading the terminal
    need_prompt: bool,
    sig_r: c_int,
    cooked: Option<Termios>,
    keys_mode: bool, // one-key mode, for a question in a cooked terminal
    interactive: bool,
    raw_on: bool,
    kd: Decoder,
    kd_deadline: i64, // when a key left part-way is given up (a lone ESC: the Escape key)
    stdin_open: bool,
    inbuf: Vec<u8>,
    inlen: usize,
    paste: Vec<u8>, // a pasted block of lines, so far
    rbuf: [u8; 4096],
    rbuf_n: usize,
    rbuf_i: usize,
}

const INBUF: usize = 65536;

impl App {
    /// What the screen has to write, written: when the loop is about to wait, and before anything
    /// else writes to the terminal. A frame of the bottom block is many small pieces, and a
    /// terminal may show each write as it comes.
    fn flush(&mut self) {
        for (fd, b) in self.scr.take_out() {
            write_all(fd as c_int, &b);
        }
    }

    fn push(&mut self, ev: Ev) {
        self.q.push_back(ev);
    }

    fn answer(&mut self, id: i64, answer: Answer) {
        self.push(Ev::Answer { id, answer });
    }

    // ---- the terminal's modes ----

    fn keys_on(&mut self) {
        if self.keys_mode || !self.tty_in {
            return;
        }
        let Some(mut t) = self.cooked else { return };
        t.lflag &= !(sys::ICANON | sys::ECHO);
        t.cc[sys::VMIN] = 1;
        t.cc[sys::VTIME] = 0;
        sys::flush_input(0); // a line typed ahead must not answer the question
        if sys::set_term(0, &t) {
            self.keys_mode = true;
        }
    }

    fn keys_off(&mut self) {
        if !self.keys_mode {
            return;
        }
        if let Some(t) = &self.cooked {
            sys::set_term(0, t);
        }
        self.keys_mode = false;
    }

    /// Raw mode: no line discipline, no echo; Ctrl-C and Ctrl-Z stay signals (commands run in
    /// process groups of their own, so the terminal sends those to barb alone). Output is still
    /// processed, so "\n" is a new line. Ctrl-\ is turned off: its default is to dump core with
    /// the terminal raw.
    fn term_raw(&mut self) {
        let Some(t) = self.cooked else { return };
        if !set_raw(&t) {
            return;
        }
        if !self.raw_on {
            self.flush();
            write_all(2, b"\x1b[?2004h"); // bracket pastes
        }
        self.raw_on = true;
        RAW_ON.store(true, Ordering::Relaxed);
    }

    fn term_restore(&mut self) {
        if !self.raw_on {
            return;
        }
        self.flush();
        write_all(2, b"\x1b[?2004l");
        if let Some(t) = &self.cooked {
            sys::set_term(0, t);
        }
        self.raw_on = false;
        RAW_ON.store(false, Ordering::Relaxed);
    }

    // ---- carrying out actions ----

    fn ask(&mut self, id: i64, tool: &[u8], what: &[u8]) {
        if self.oneshot || !self.tty_in {
            self.scr.say(Style::Error, NOT_ALLOWED);
            self.answer(id, Answer::No);
            return;
        }
        if self.interactive {
            self.read_waiting_keys(); // keys typed before the question go to the line, so they cannot answer it
            if !self.stdin_open {
                self.answer(id, Answer::No);
                return;
            }
        } else {
            self.keys_on(); // before the question shows, so the flush cannot eat a quick answer
        }
        self.scr.question(tool, what);
        self.awaiting = id;
    }

    /// Every action is shown to the screen first (Say is only that), then carried out.
    fn exec(&mut self, a: &Action) {
        self.scr.action(a, now_ms());
        match a {
            Action::Say { .. } => self.need_prompt = true,
            Action::Http { id, path, body } => self.ops.http(&mut self.q, *id, path.as_bytes(), body),
            Action::Read { id, path } => self.ops.read(&mut self.q, *id, path),
            Action::Write { id, path, data } => {
                self.ops.write(&mut self.q, *id, path, data);
                common::wrote(path);
            }
            Action::Spawn { id, cmd, timeout_ms, background, log } => {
                self.ops.spawn(&mut self.q, *id, cmd, *timeout_ms, *background, log)
            }
            Action::Timer { id, ms } => self.ops.timer(*id, *ms),
            Action::Cancel { id } => {
                if *id == self.awaiting {
                    self.awaiting = 0;
                    self.keys_off();
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

    // ---- lines ----

    fn handle_line(&mut self, mut line: &[u8]) {
        while line.last() == Some(&b'\r') {
            line = &line[..line.len() - 1];
        }
        if self.awaiting != 0 {
            // answered on a line of its own: the terminal echoed it
            let id = self.awaiting;
            self.awaiting = 0;
            self.scr.midline = false;
            let is = |w: &str| line.eq_ignore_ascii_case(w.as_bytes());
            let ans = if is("y") || is("yes") {
                Answer::Yes
            } else if is("a") || is("always") {
                Answer::Always
            } else {
                Answer::No
            };
            self.answer(id, ans);
            return;
        }
        self.push(Ev::Input(line.to_vec()));
    }

    /// A key pressed while a question waits in one-key mode: y, n or a answers it (Enter, Escape
    /// and Ctrl-D say no); anything else is ignored.
    fn answer_key(&mut self, c: u8) -> bool {
        let ans = match c {
            b'y' | b'Y' => Answer::Yes,
            b'a' | b'A' => Answer::Always,
            b'n' | b'N' | b'\r' | b'\n' | 27 | 4 => Answer::No,
            _ => return false,
        };
        let id = self.awaiting;
        self.awaiting = 0;
        self.keys_off();
        self.scr.answer(ans);
        self.answer(id, ans);
        true
    }

    // A pasted block of lines is one message. The terminal brackets a paste, so each Enter inside
    // one adds its line here; a terminal that does not is caught the way Windows catches it, by
    // lines that are followed at once by more text. Delivered with the line that ends it, or, when
    // the paste itself ended with an Enter, when it ends.
    fn deliver_paste(&mut self) {
        let p = std::mem::take(&mut self.paste);
        self.push(Ev::Input(p));
    }

    /// Is typed text already waiting behind the Enter being handled? Part-way through a paste the
    /// next chunk may still be on its way, so it is given a moment.
    fn more_text_waiting(&self) -> bool {
        let mut probe = self.kd.clone();
        for &b in &self.rbuf[(self.rbuf_i + 1).min(self.rbuf_n)..self.rbuf_n] {
            if let Some(Input::Edit(Key::Char(_))) = probe.feed(b) {
                return true;
            }
        }
        let mut p = [PollFd { fd: 0, events: POLLIN, revents: 0 }];
        sys::wait(&mut p, if self.paste.is_empty() { 0 } else { 40 }) > 0 && p[0].revents & POLLIN != 0
    }

    fn enter(&mut self) {
        // a paste while a question waits goes to the line, which is not on the screen then
        let line = if self.awaiting != 0 { self.scr.take_unshown() } else { self.scr.take() };
        if self.kd.pasting() || self.more_text_waiting() {
            self.paste.extend_from_slice(&line);
            self.paste.push(b'\n');
            return;
        }
        if self.paste.is_empty() {
            self.push(Ev::Input(line));
            return;
        }
        self.paste.extend_from_slice(&line);
        self.deliver_paste();
    }

    /// Ctrl-D on an empty line, or the terminal gone: the end of input.
    fn end_input(&mut self) {
        self.scr.hide();
        self.stdin_open = false;
        if !self.paste.is_empty() {
            self.deliver_paste();
        }
        self.push(Ev::Eof);
    }

    /// Ctrl-C: stop the work; at an idle prompt, throw away what was typed (and pasted) first.
    fn interrupt(&mut self) {
        if self.interactive
            && self.awaiting == 0
            && !self.agent.busy()
            && (!self.scr.ed.line.is_empty() || !self.paste.is_empty() || self.kd.pasting())
        {
            self.scr.key(Key::Clear);
            self.paste.clear();
            self.kd = Decoder::new();
            return;
        }
        self.push(Ev::Interrupt);
    }

    fn on_key(&mut self, k: Input) {
        // One key answers a question. Pasted text does not: it goes to the line, which waits.
        if self.awaiting != 0 && !self.kd.pasting() && k != Input::PasteEnd {
            match k {
                Input::Enter => {
                    self.answer_key(b'\r');
                }
                Input::Eof => {
                    self.answer_key(4);
                }
                Input::Intr => self.interrupt(),
                Input::Edit(Key::Clear) => {
                    self.answer_key(27);
                }
                Input::Edit(Key::Char(c)) if c < 0x80 => {
                    self.answer_key(c as u8);
                }
                _ => {}
            }
            return;
        }
        match k {
            Input::Edit(key) => self.scr.key(key),
            Input::Enter => self.enter(),
            Input::Eof => {
                // on an empty line, the end; otherwise delete forward, as in a shell
                if !self.scr.ed.line.is_empty() {
                    self.scr.key(Key::Del);
                } else {
                    self.end_input();
                }
            }
            Input::Intr => self.interrupt(),
            Input::PasteEnd => {
                // a paste that ended with an Enter is sent; otherwise its last line waits
                if !self.paste.is_empty() && self.scr.ed.line.is_empty() {
                    self.deliver_paste();
                }
            }
            Input::PasteBegin => {}
        }
    }

    fn read_keys(&mut self) {
        let mut buf = [0u8; 4096];
        let r = read_fd(0, &mut buf);
        let n = match r {
            Err(e) if e.kind() == ErrorKind::Interrupted || e.kind() == ErrorKind::WouldBlock => return,
            Ok(0) | Err(_) => {
                // the terminal has gone
                if self.awaiting != 0 {
                    self.answer_key(4);
                }
                self.end_input();
                return;
            }
            Ok(n) => n,
        };
        self.rbuf = buf;
        self.rbuf_n = n;
        self.rbuf_i = 0;
        while self.rbuf_i < self.rbuf_n && self.stdin_open && !self.done {
            if let Some(k) = self.kd.feed(self.rbuf[self.rbuf_i]) {
                self.on_key(k);
            }
            self.rbuf_i += 1;
        }
        self.rbuf_n = 0;
        self.rbuf_i = 0;
        // A sequence cut off mid-paste is most likely the end marker split across reads: wait longer.
        if self.kd.pending() {
            self.kd_deadline = now_ms() + if self.kd.pasting() { 1000 } else { 30 };
        }
    }

    /// Whatever the terminal has for us, without waiting.
    fn read_waiting_keys(&mut self) {
        while self.stdin_open && !self.done {
            let mut p = [PollFd { fd: 0, events: POLLIN, revents: 0 }];
            if sys::wait(&mut p, 0) <= 0 || p[0].revents & (POLLIN | POLLHUP | POLLERR) == 0 {
                break;
            }
            self.read_keys();
        }
    }

    fn resize(&mut self) {
        self.scr.resize(term_width());
    }

    /// Ctrl-Z: give the terminal back as it was, stop, and take it again when continued.
    fn suspend(&mut self) {
        self.scr.hide();
        self.term_restore();
        sys::on_signal(sys::SIGTSTP, sys::SIG_DFL);
        sys::raise_signal(sys::SIGTSTP);
        catch(sys::SIGTSTP);
        self.term_raw();
        self.resize();
    }

    fn read_stdin(&mut self) {
        if self.interactive {
            self.read_keys();
            return;
        }
        if self.keys_mode && self.awaiting != 0 {
            let mut c = [0u8; 1];
            match read_fd(0, &mut c) {
                Ok(1) => {
                    self.answer_key(c[0]);
                }
                Err(e) if e.kind() == ErrorKind::Interrupted || e.kind() == ErrorKind::WouldBlock => {}
                _ => {
                    self.answer_key(b'n');
                }
            }
            return;
        }
        let at = self.inlen;
        let r = read_fd(0, &mut self.inbuf[at..INBUF - 1]);
        let n = match r {
            Err(e) if e.kind() == ErrorKind::Interrupted || e.kind() == ErrorKind::WouldBlock => return,
            Ok(0) | Err(_) => {
                self.stdin_open = false;
                if self.inlen > 0 {
                    let l = self.inbuf[..self.inlen].to_vec();
                    self.handle_line(&l);
                    self.inlen = 0;
                }
                if self.awaiting != 0 {
                    let id = self.awaiting;
                    self.awaiting = 0;
                    self.answer(id, Answer::No);
                }
                self.push(Ev::Eof);
                if self.tty_in {
                    self.scr.raw(2, b"\n"); // after the ^D the terminal echoed
                }
                return;
            }
            Ok(n) => n,
        };
        self.inlen += n;
        let mut start = 0;
        let mut i = 0;
        while i < self.inlen {
            if self.inbuf[i] == b'\n' {
                let l = self.inbuf[start..i].to_vec();
                self.handle_line(&l);
                start = i + 1;
            }
            i += 1;
        }
        if start > 0 {
            self.inbuf.copy_within(start..self.inlen, 0);
            self.inlen -= start;
        }
        if self.inlen >= INBUF - 1 {
            // one enormous line: hand over what there is
            let l = self.inbuf[..self.inlen].to_vec();
            self.handle_line(&l);
            self.inlen = 0;
        }
    }

    fn read_signals(&mut self) {
        let mut buf = [0u8; 32];
        let n = read_fd(self.sig_r, &mut buf).unwrap_or(0);
        for &c in &buf[..n] {
            match c {
                b'i' => self.interrupt(),
                b'w' => self.resize(),
                b'z' => self.suspend(),
                b'c' => self.term_raw(), // continued after a stop barb did not see coming
                _ => {
                    // SIGTERM
                    self.done = true;
                    self.exit_code = 143;
                }
            }
        }
    }

    /// The bottom of the screen: the block with the prompt whenever the user can type, otherwise
    /// (a question waits, input has ended, the terminal is not ours) at most a status line.
    fn draw(&mut self) {
        let prompt_ok = self.interactive && self.awaiting == 0 && self.stdin_open && !self.done;
        let status_ok = self.awaiting == 0 && !self.done;
        self.scr.draw(now_ms(), self.agent.busy(), prompt_ok, status_ok);
    }

    fn run(&mut self) {
        loop {
            if self.interactive && self.kd.pending() && now_ms() >= self.kd_deadline {
                if let Some(k) = self.kd.timeout() {
                    self.on_key(k);
                }
            }
            self.drain_queue();
            if self.done {
                return;
            }
            // A terminal in line mode gets a plain prompt when the agent is ready for the next line.
            if !self.interactive
                && self.need_prompt
                && self.tty_in
                && !self.oneshot
                && self.stdin_open
                && self.awaiting == 0
                && !self.agent.busy()
            {
                self.scr.raw(2, b"> ");
                self.need_prompt = false;
            }
            self.draw();
            self.flush();
            let mut fds = vec![PollFd { fd: self.sig_r, events: POLLIN, revents: 0 }];
            let stdin_slot = if self.stdin_open && !self.oneshot {
                fds.push(PollFd { fd: 0, events: POLLIN, revents: 0 });
                Some(1)
            } else {
                None
            };
            self.ops.fill(&mut fds);
            let mut timeout = self.ops.timeout_ms();
            let anim = self.scr.next_ms(now_ms()); // the spinner's next frame
            if anim >= 0 && (timeout < 0 || timeout > anim) {
                timeout = anim;
            }
            if self.interactive && self.kd.pending() {
                let left = (self.kd_deadline - now_ms()).max(0);
                if timeout < 0 || timeout > left {
                    timeout = left;
                }
            }
            let r = sys::wait(&mut fds, timeout.min(i32::MAX as i64) as i32);
            if r > 0 {
                if fds[0].revents != 0 {
                    self.read_signals();
                }
                if let Some(s) = stdin_slot {
                    if fds[s].revents != 0 {
                        self.read_stdin();
                    }
                }
            }
            self.ops.service(&mut self.q);
        }
    }
}

// ---- starting up ----

fn usage(to_err: bool) {
    let text = format!(
        "barb {}: a coding agent for the terminal\n\n\
         usage: barb [options]\n       barb -p TASK [options]\n\n\
         \x20 -p, --prompt TASK   do one task and exit (TASK of - reads it from standard input)\n\
         \x20 -m, --model NAME    the model (default {}, or $BARB_MODEL; /model changes it)\n\
         \x20     --effort LEVEL  low, medium, high, xhigh or max (or $BARB_EFFORT; /effort changes it)\n\
         \x20 -y, --yes           do not ask before writing files or running commands\n\
         \x20 -C, --cwd DIR       work in DIR\n\
         \x20     --max-turns N   stop after N model calls without you typing (default 60)\n\
         \x20     --no-instructions  do not read AGENTS.md or CLAUDE.md from the working directory\n\
         \x20 -c, --continue      take up the conversation saved last in this directory\n\
         \x20 -r, --resume NAME   take up a saved conversation (--sessions lists them)\n\
         \x20     --sessions      list the conversations saved in this directory\n\
         \x20     --no-save       do not save this conversation (they go in .barb/sessions)\n\
         \x20 -v, --verbose       show the model's thinking (/verbose switches it)\n\
         \x20 -V, --version\n\
         \x20 -h, --help\n\n\
         environment: ANTHROPIC_API_KEY (required), ANTHROPIC_BASE_URL, BARB_MODEL, BARB_EFFORT\n",
        VERSION, DEFAULT_MODEL
    );
    write_all(if to_err { 2 } else { 1 }, text.as_bytes());
}

fn bytes(s: OsString) -> Vec<u8> {
    s.into_vec()
}

pub fn main() -> i32 {
    let argv: Vec<OsString> = std::env::args_os().skip(1).collect();
    let args = match common::parse(&argv) {
        Parsed::Help => {
            usage(false);
            return 0;
        }
        Parsed::Version => {
            write_all(1, format!("barb {}\n", VERSION).as_bytes());
            return 0;
        }
        Parsed::Bad(a) => {
            let mut m = b"barb: unknown or incomplete option ".to_vec();
            m.extend_from_slice(a.as_bytes());
            m.extend_from_slice(b"\n\n");
            write_all(2, &m);
            usage(true);
            return 2;
        }
        Parsed::Run(a) => a,
    };
    let mut model = args.model.clone().or_else(|| std::env::var_os("BARB_MODEL")).map(bytes).unwrap_or_default();
    let chosen = !model.is_empty();
    if model.is_empty() {
        model = DEFAULT_MODEL.as_bytes().to_vec();
    }

    if let Some(d) = &args.cwd {
        if let Err(e) = std::env::set_current_dir(d) {
            let mut m = b"barb: cannot change to ".to_vec();
            m.extend_from_slice(d.as_bytes());
            m.extend_from_slice(format!(": {}\n", common::strerror(&e)).as_bytes());
            write_all(2, &m);
            return 2;
        }
    }
    if args.sessions {
        write_all(1, common::list_sessions().as_bytes());
        return 0;
    }
    let now_s = now_ms() / 1000;
    let plan = match common::plan(&args, sys::local(now_s).unwrap_or((1970, 1, 1, 0, 0)), now_s.rem_euclid(60) as u32) {
        Ok(p) => p,
        Err(e) => {
            write_all(2, format!("barb: {}\n", e).as_bytes());
            return 2;
        }
    };
    if !chosen && !plan.model.is_empty() {
        model = plan.model.clone(); // go on with the model it was saved with
    }
    let effort = match common::effort(&args, std::env::var_os("BARB_EFFORT")) {
        Ok(e) => e,
        Err(e) => {
            write_all(2, format!("barb: {}\n", e).as_bytes());
            return 2;
        }
    };

    // Take the key out of the environment: every command the model runs inherits it otherwise,
    // and `echo $ANTHROPIC_API_KEY` would show it. Only the platform layer holds it from here.
    let key = std::env::var_os("ANTHROPIC_API_KEY").map(bytes).unwrap_or_default();
    std::env::remove_var("ANTHROPIC_API_KEY");
    if key.is_empty() {
        write_all(2, b"barb: ANTHROPIC_API_KEY is not set.\n");
        return 2;
    }
    let (base, warn) = common::base_url(std::env::var("ANTHROPIC_BASE_URL").ok());
    if warn {
        write_all(2, common::PLAIN_HTTP_WARNING.as_bytes());
    }

    let cwd = std::env::current_dir().map(|p| bytes(p.into_os_string())).unwrap_or_else(|_| b".".to_vec());
    let date = sys::local(now_s).map(|(y, m, d, _, _)| common::ymd(y, m, d)).unwrap_or_default();
    let platform = sys::platform();

    let prompt = match args.prompt {
        Some(p) if p == "-" => {
            let mut b = Vec::new();
            let _ = fd_file(0).read_to_end(&mut b);
            Some(b)
        }
        p => p.map(bytes),
    };
    let oneshot = prompt.is_some();
    let cooked = if std::io::stdin().is_terminal() { sys::get_term(0) } else { None };
    let tty_in = cooked.is_some();
    if let Some(t) = cooked {
        let _ = COOKED.set(t);
    }
    let tty_err = std::io::stderr().is_terminal();
    let term = std::env::var_os("TERM").map(bytes);
    let dumb = term.as_deref() == Some(b"dumb");

    let (sig_r, sig_w) = match std::io::pipe() {
        Ok((r, w)) => (r.into_raw_fd(), w.into_raw_fd()),
        Err(e) => {
            write_all(2, format!("barb: pipe: {}\n", common::strerror(&e)).as_bytes());
            return 1;
        }
    };
    sys::set_nonblock(sig_r);
    sys::set_nonblock(sig_w);
    SIG_W.store(sig_w, Ordering::Relaxed);

    // The screen: the whole bottom block for an interactive session, a status line on a terminal
    // that is not ours to edit, plain text for pipes, files and dumb terminals. The Linux
    // console's font has no braille or bullets, and a terminal not set up for UTF-8 may show none
    // of it.
    let loc = ["LC_ALL", "LC_CTYPE", "LANG"]
        .iter()
        .filter_map(|v| std::env::var_os(v).map(bytes))
        .find(|v| !v.is_empty());
    let utf8 = match &loc {
        None => true,
        Some(l) => {
            let has = |w: &[u8]| l.windows(w.len()).any(|x| x == w);
            has(b"UTF-8") || has(b"utf-8") || has(b"UTF8") || has(b"utf8") || l == b"C" || l == b"POSIX"
        }
    };
    let ascii = !utf8 || term.as_deref().is_some_and(|t| t.starts_with(b"linux"));
    let vt_err = tty_err && !dumb;
    let home = std::env::var_os("HOME").map(bytes);
    let interactive = !oneshot && tty_err && !dumb && cooked.as_ref().is_some_and(set_raw);
    if interactive {
        write_all(2, b"\x1b[?2004h"); // bracket pastes
        RAW_ON.store(true, Ordering::Relaxed);
    }
    let mode = if interactive {
        Mode::Region
    } else if vt_err {
        Mode::Lite
    } else {
        Mode::Pipe
    };
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
            session: plan.path.clone(),
            verbose: args.verbose,
            effort,
        }),
        scr: Screen::new(ScreenConfig {
            mode,
            vt_out: std::io::stdout().is_terminal() && !dumb,
            vt_err,
            fancy: !ascii,
            ascii,
            width: if tty_err { term_width() } else { 80 },
            model: &model,
            cwd: &cwd,
            home: home.as_deref(),
            clock: Some(Box::new(|at: i64| {
                sys::local(at.div_euclid(1000)).map(|(_, _, _, h, m)| common::hhmm(h, m)).unwrap_or_default()
            })),
        }),
        ops: Ops::new(base.into_bytes(), key),
        q: VecDeque::new(),
        done: false,
        exit_code: 0,
        awaiting: 0,
        tty_in,
        oneshot,
        need_prompt: false,
        sig_r,
        cooked,
        keys_mode: false,
        interactive,
        raw_on: interactive,
        kd: Decoder::new(),
        kd_deadline: 0,
        stdin_open: true,
        inbuf: vec![0; INBUF],
        inlen: 0,
        paste: Vec::new(),
        rbuf: [0; 4096],
        rbuf_n: 0,
        rbuf_i: 0,
    };
    catch(sys::SIGINT);
    catch(sys::SIGTERM);
    if app.interactive {
        catch(sys::SIGWINCH);
        catch(sys::SIGTSTP);
        catch(sys::SIGCONT);
        let default = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            restore_after_panic();
            default(info);
        }));
    }

    match prompt {
        None => {
            app.scr.banner(VERSION.as_bytes());
            app.need_prompt = true;
        }
        Some(p) => {
            app.push(Ev::Input(p));
            app.push(Ev::Eof);
        }
    }
    if let Some(d) = &plan.data {
        // Queued events wait until the loop starts, so this comes first.
        for a in app.agent.resume(d, app.interactive).unwrap_or_default() {
            app.exec(&a);
        }
    }
    app.run();
    app.keys_off();
    app.scr.hide();
    app.flush();
    app.term_restore();
    app.ops.shutdown();
    if app.interactive {
        if let Some(l) = common::saved_line(&app.agent.session_path()) {
            write_all(2, l.as_bytes());
        }
    }
    app.exit_code
}
