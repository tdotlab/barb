//! What barb shows on a terminal: the record of the session (replies, tool lines, questions) and,
//! at the bottom, a block that stays there while the session runs: a line saying what is going on
//! (waiting for the model, running a command, how long it has taken), a rule, the prompt, and a
//! footer with the model, the directory and the context's size.
//!
//! ```text
//!     ● bash ls src
//!       └ exit 0, 3 lines
//!
//!     Three files: core, ui and posix.
//!
//!     ⠹ waiting for the model · 4s
//!     ────────────────────────────────────────────
//!     > _
//!       claude-sonnet · ~/src/barb · 12.4k tokens       Ctrl-C stops it
//! ```
//!
//! Like the editor and the markdown renderer it does no I/O: what to write collects in
//! [`Screen::out`], each piece tagged with the descriptor it goes to (1 or 2), and the platform
//! tells it what happens (the agent's events and actions, the keys, the time). Every platform uses
//! it, so they all look the same.
//!
//! Three modes. [`Mode::Region`] is an interactive session on a terminal: the block above.
//! [`Mode::Lite`] is a terminal that is not ours to edit (barb -p on a terminal, or a terminal in
//! line mode): the record is styled the same way and a one-line status ("⠹ waiting for the model ·
//! 4s") shows while the agent works. [`Mode::Pipe`] is everything else: plain text, as barb has
//! always written it, so scripts and logs see no escapes. Replies are drawn as markdown whenever
//! standard output is a terminal.

use alloc::boxed::Box;
use alloc::vec::Vec;
use barb_core::agent::{Action, Answer, Event, Style};

use crate::edit::{self, Editor, Key, DECOR};
use crate::md::{self, Md};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Mode {
    Pipe,
    Lite,
    Region,
}

/// "14:32" for a time in `now`'s units.
pub type Clock = Box<dyn Fn(i64) -> Vec<u8>>;

pub struct Config<'a> {
    pub mode: Mode,
    /// Standard output is a terminal that takes escape sequences.
    pub vt_out: bool,
    /// Standard error is.
    pub vt_err: bool,
    /// The font has braille, for the spinner.
    pub fancy: bool,
    /// No box-drawing characters or bullets (the Linux console).
    pub ascii: bool,
    /// The terminal's columns.
    pub width: usize,
    pub model: &'a [u8],
    pub cwd: &'a [u8],
    /// Shown as ~ at the start of cwd.
    pub home: Option<&'a [u8]>,
    /// None: times are not shown.
    pub clock: Option<Clock>,
}

/// Background tasks and timers it keeps track of.
pub const OPS: usize = 32;

// What was shown last, for the blank lines between things.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Gap {
    None,
    Banner,
    User,
    Assist,
    Tool,
    Note,
    Question,
}

pub struct Screen {
    pub mode: Mode,
    pub vt_out: bool,
    pub vt_err: bool,
    pub fancy: bool,
    pub ascii: bool,
    pub width: usize,
    clock: Option<Clock>,
    /// Cleaned up for showing.
    pub model: Vec<u8>,
    pub cwd: Vec<u8>,
    pub ed: Editor,
    pub md: Md,
    /// What to write, in order: (1 or 2, the bytes). The caller takes it.
    pub out: Vec<(u8, Vec<u8>)>,
    // the request out, and when it went
    http_id: i64,
    http_since: i64,
    // the command running in the foreground, its first line cleaned, and whether it had more
    fg_id: i64,
    fg_since: i64,
    fg_cmd: Vec<u8>,
    fg_more: bool,
    // the timer before a failed request is retried
    retry_id: i64,
    retry_due: i64,
    task: Vec<i64>,
    timer: Vec<(i64, i64)>, // the model's wakeups: id, when due
    /// The size of the last request, from a Usage line.
    pub tokens: i64,
    // the event being handled is a request that failed in a way that is retried
    http_failed: bool,
    last: Gap,
    // a question waits on the line for its answer
    pub midline: bool,
    /// Lite: the status line is on the screen.
    pub status_shown: bool,
    status: Vec<u8>,
    // the activity line has a spinner
    animating: bool,
}

// ---- a small string builder, cut at its size (as the C kept it) ----

struct Buf {
    b: Vec<u8>,
    cap: usize,
}

impl Buf {
    fn new(cap: usize) -> Buf {
        Buf { b: Vec::new(), cap }
    }
    fn put(&mut self, p: &[u8]) {
        let room = self.cap - 1 - self.b.len();
        self.b.extend_from_slice(&p[..p.len().min(room)]);
    }
    fn int(&mut self, v: i64) {
        let mut t = [0u8; 24];
        let mut k = 0;
        let mut v = v.max(0);
        loop {
            t[k] = b'0' + (v % 10) as u8;
            k += 1;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        while k > 0 {
            k -= 1;
            self.put(&t[k..k + 1]);
        }
    }
    fn spaces(&mut self, n: isize) {
        for _ in 0..n.max(0) {
            self.put(b" ");
        }
    }
}

// ---- characters ----

// A code point and its length, strictly: overlong forms, surrogates and broken sequences are
// U+FFFD with length 1.
fn dec(p: &[u8]) -> (u32, usize) {
    let u0 = p[0];
    if u0 < 0x80 {
        return (u0 as u32, 1);
    }
    let (mut c, k) = if u0 & 0xE0 == 0xC0 {
        ((u0 & 0x1F) as u32, 2)
    } else if u0 & 0xF0 == 0xE0 {
        ((u0 & 0x0F) as u32, 3)
    } else if u0 & 0xF8 == 0xF0 {
        ((u0 & 0x07) as u32, 4)
    } else {
        return (0xFFFD, 1);
    };
    if k > p.len() {
        return (0xFFFD, 1);
    }
    for &b in &p[1..k] {
        if b & 0xC0 != 0x80 {
            return (0xFFFD, 1);
        }
        c = (c << 6) | (b & 0x3F) as u32;
    }
    if (k == 2 && c < 0x80)
        || (k == 3 && c < 0x800)
        || (k == 4 && !(0x10000..=0x10FFFF).contains(&c))
        || (0xD800..=0xDFFF).contains(&c)
    {
        return (0xFFFD, 1);
    }
    (c, k)
}

/// Copy text for a one-line place (the activity line, the footer) into at most `cap - 1` bytes:
/// control characters become spaces, C1 controls and broken UTF-8 '?'. At a newline it stops. The
/// bool: it did, or ran out of room (there is more).
fn clean(cap: usize, p: &[u8]) -> (Vec<u8>, bool) {
    let mut d = Vec::new();
    let mut more = false;
    let n = p.len();
    let mut i = 0;
    while i < n {
        let (c, len) = dec(&p[i..]);
        let src: &[u8] = if c == b'\n' as u32 || c == b'\r' as u32 {
            more = i + 1 < n;
            break;
        } else if c < 0x20 || c == 0x7F {
            b" "
        } else if (0x80..=0x9F).contains(&c) || (c == 0xFFFD && len == 1) {
            b"?"
        } else {
            &p[i..i + len]
        };
        if d.len() + src.len() > cap - 1 {
            more = true;
            break;
        }
        d.extend_from_slice(src);
        i += len;
    }
    (d, more)
}

fn cols_of(p: &[u8]) -> isize {
    edit::text_cols(p) as isize
}

/// Put p (clean) in at most `maxc` columns: cut at the end with an ellipsis if it does not fit, or
/// if `cut` says it was already cut. `head`: cut at the start instead, keeping the end.
fn fit(b: &mut Buf, p: &[u8], maxc: isize, cut: bool, head: bool, ell: &[u8]) {
    let total = cols_of(p);
    let ec = cols_of(ell);
    let n = p.len();
    if maxc <= 0 {
        return;
    }
    if total <= maxc && !cut {
        b.put(p);
        return;
    }
    if maxc <= ec {
        b.put(ell);
        return;
    }
    let room = maxc - ec;
    let mut i = 0;
    if head {
        // the last `room` columns
        let mut c = total;
        while i < n && c > room {
            let (ch, len) = dec(&p[i..]);
            c -= edit::cols(ch) as isize;
            i += len;
        }
        b.put(ell);
        b.put(&p[i..]);
        return;
    }
    let mut c = 0;
    while i < n {
        let (ch, len) = dec(&p[i..]);
        let w = edit::cols(ch) as isize;
        if c + w > room {
            break;
        }
        c += w;
        i += len;
    }
    b.put(&p[..i]);
    b.put(ell);
}

// "12s", "1m 05s", "1h 02m".
fn elapsed(b: &mut Buf, ms: i64) {
    let t = if ms < 0 { 0 } else { ms / 1000 };
    if t < 60 {
        b.int(t);
        b.put(b"s");
        return;
    }
    let (big, small) = if t < 3600 { (t / 60, t % 60) } else { (t / 3600, t / 60 % 60) };
    b.int(big);
    b.put(if t < 3600 { b"m " } else { b"h " });
    if small < 10 {
        b.put(b"0");
    }
    b.int(small);
    b.put(if t < 3600 { b"s" } else { b"m" });
}

fn tokens(b: &mut Buf, t: i64) {
    if t < 1000 {
        b.int(t);
        return;
    }
    let unit = if t < 1_000_000 { 1000 } else { 1_000_000 };
    let u: &[u8] = if unit == 1000 { b"k" } else { b"M" };
    if t / unit >= 100 {
        b.int(t / unit);
        b.put(u);
        return;
    }
    let tenths = t * 10 / unit;
    b.int(tenths / 10);
    b.put(b".");
    b.int(tenths % 10);
    b.put(u);
}

const BRAILLE: [&str; 10] = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];
const PLAIN: &[u8] = b"-\\|/";

impl Screen {
    pub fn new(cfg: Config) -> Screen {
        let width = if cfg.width < 10 { 80 } else { cfg.width };
        let model = clean(96, cfg.model).0;
        let cwd = cfg.cwd;
        let mut hl = cfg.home.map_or(0, |h| h.len());
        let home = cfg.home.unwrap_or(b"");
        while hl > 1 && (home[hl - 1] == b'/' || home[hl - 1] == b'\\') {
            hl -= 1;
        }
        let cwd = if hl > 0
            && cwd.len() >= hl
            && cwd[..hl] == home[..hl]
            && (cwd.len() == hl || cwd[hl] == b'/' || cwd[hl] == b'\\')
        {
            let mut v = alloc::vec![b'~'];
            v.extend_from_slice(&clean(255, &cwd[hl..]).0);
            v
        } else {
            clean(256, cwd).0
        };
        let mut ed = Editor::new(width);
        if cfg.mode == Mode::Region {
            ed.frame(2, true);
        }
        ed.out.clear();
        Screen {
            mode: cfg.mode,
            vt_out: cfg.vt_out,
            vt_err: cfg.vt_err,
            fancy: cfg.fancy,
            ascii: cfg.ascii,
            width,
            clock: cfg.clock,
            model,
            cwd,
            ed,
            md: Md::new(width, cfg.ascii),
            out: Vec::new(),
            http_id: 0,
            http_since: 0,
            fg_id: 0,
            fg_since: 0,
            fg_cmd: Vec::new(),
            fg_more: false,
            retry_id: 0,
            retry_due: 0,
            task: Vec::new(),
            timer: Vec::new(),
            tokens: 0,
            http_failed: false,
            last: Gap::None,
            midline: false,
            status_shown: false,
            status: Vec::new(),
            animating: false,
        }
    }

    /// What to write, in order, taken.
    pub fn take_out(&mut self) -> Vec<(u8, Vec<u8>)> {
        core::mem::take(&mut self.out)
    }

    // ---- writing ----

    fn wr(&mut self, fd: u8, p: &[u8]) {
        if p.is_empty() {
            return;
        }
        match self.out.last_mut() {
            Some((f, b)) if *f == fd => b.extend_from_slice(p),
            _ => self.out.push((fd, p.to_vec())),
        }
    }

    // Text from elsewhere (the model, a command, a file name), its control characters made visible.
    fn safe(&mut self, fd: u8, p: &[u8]) {
        let mut v = Vec::new();
        md::safe(p, &mut v);
        self.wr(fd, &v);
    }

    fn flush_ed(&mut self) {
        let o = core::mem::take(&mut self.ed.out);
        self.wr(2, &o);
    }

    fn vt(&self, fd: u8) -> bool {
        if fd == 1 { self.vt_out } else { self.vt_err }
    }

    fn styled(&self, fd: u8) -> bool {
        self.mode != Mode::Pipe && self.vt(fd)
    }

    fn ell(&self) -> &'static [u8] {
        if self.ascii { b"..." } else { "…".as_bytes() }
    }

    fn dot(&self) -> &'static [u8] {
        if self.ascii { b" - " } else { " · ".as_bytes() }
    }

    // ---- making room ----

    fn status_clear(&mut self) {
        if !self.status_shown {
            return;
        }
        self.wr(2, b"\r\x1b[K");
        self.status_shown = false;
        self.status.clear();
    }

    // Before anything is written: the block and the status line go, and a question's line ends.
    fn clear(&mut self) {
        if self.ed.shown {
            self.ed.hide();
            self.flush_ed();
        }
        self.status_clear();
        if self.midline {
            self.wr(2, b"\n");
            self.midline = false;
        }
    }

    /// Take the block and the status line off the screen (and end a question's line), leaving the
    /// cursor at the start of a free row: before exiting or stopping, and when a question goes.
    pub fn hide(&mut self) {
        self.clear();
    }

    // A blank line between things of different kinds, on a terminal: before every reply, tool and
    // question, before the user's line unless it follows another, and before a note that follows
    // one of those. Never in Pipe mode, so what scripts read is as it was.
    fn gap(&mut self, fd: u8, what: Gap) {
        let l = self.last;
        let want = match what {
            Gap::Note => l == Gap::Assist || l == Gap::Tool || l == Gap::Question,
            Gap::User => l != Gap::None && l != Gap::User,
            _ => l != Gap::None,
        };
        if want && self.styled(fd) {
            self.wr(fd, b"\n");
        }
        self.last = what;
    }

    // ---- the record ----

    /// Written as it is.
    pub fn raw(&mut self, fd: u8, text: &[u8]) {
        self.clear();
        self.wr(fd, text);
    }

    pub fn banner(&mut self, version: &[u8]) {
        self.clear();
        let model = core::mem::take(&mut self.model);
        let cwd = core::mem::take(&mut self.cwd);
        if !self.styled(2) {
            self.wr(2, b"barb ");
            self.wr(2, version);
            self.wr(2, b", model ");
            self.wr(2, &model);
            self.wr(2, b", in ");
            self.wr(2, &cwd);
            self.wr(2, b"\n/help for commands; Ctrl-D to exit.\n");
        } else {
            let dot = self.dot();
            self.wr(2, b"\x1b[1mbarb\x1b[0m\x1b[2m ");
            self.wr(2, version);
            self.wr(2, dot);
            self.wr(2, &model);
            self.wr(2, dot);
            self.wr(2, &cwd);
            self.wr(2, b"\x1b[0m\n\x1b[2m/help for commands");
            self.wr(2, dot);
            self.wr(2, b"Ctrl-D to exit\x1b[0m\n");
        }
        self.model = model;
        self.cwd = cwd;
        self.last = Gap::Banner;
    }

    // A reply: markdown on a terminal, as it came anywhere else.
    fn assistant(&mut self, p: &[u8]) {
        self.gap(1, Gap::Assist);
        if !self.vt_out {
            self.wr(1, p);
            self.wr(1, b"\n");
            return;
        }
        let mut n = p.len();
        while n > 0 && (p[n - 1] == b'\n' || p[n - 1] == b'\r') {
            n -= 1;
        }
        self.md.render(&p[..n]);
        let o = core::mem::take(&mut self.md.out);
        self.wr(1, &o);
    }

    // A tool's follow-up ("  exit 0, 3 lines", "  error: ..."): under the tool line, hung off a corner.
    fn follow_up(&mut self, style: Style, p: &[u8]) {
        let n = p.len();
        let err = style == Style::Error;
        let mut i = 0;
        loop {
            let mut e = i;
            while e < n && p[e] != b'\n' {
                e += 1;
            }
            let mut a = i;
            while a < e && p[a] == b' ' {
                a += 1;
            }
            if i == 0 {
                self.wr(2, if self.ascii { b"\x1b[2m  - " } else { "\x1b[2m  └ ".as_bytes() });
                self.wr(2, if err { b"\x1b[0m\x1b[31m" } else { b"" });
            } else {
                self.wr(2, b"    ");
                self.wr(2, if err { b"\x1b[31m" } else { b"\x1b[2m" });
            }
            self.safe(2, &p[a..e]);
            self.wr(2, b"\x1b[0m\n");
            if e >= n {
                break;
            }
            i = e + 1;
        }
    }

    // What the user said, shown again (a resumed conversation): as it looked when they typed it.
    fn user(&mut self, p: &[u8]) {
        self.gap(2, Gap::User);
        let st = self.styled(2);
        for (k, line) in p.split(|&c| c == b'\n').enumerate() {
            self.wr(2, match (k, st) {
                (0, true) => b"\x1b[1;35m> \x1b[0m",
                (0, false) => b"> ",
                _ => b"  ",
            });
            self.safe(2, line);
            self.wr(2, b"\n");
        }
    }

    // The model's thinking: dim and italic, every line, so it reads as an aside. It is spaced
    // like a reply.
    fn thinking(&mut self, p: &[u8]) {
        self.gap(2, Gap::Assist);
        let st = self.styled(2);
        for line in p.split(|&c| c == b'\n') {
            if st {
                self.wr(2, b"\x1b[2;3m");
            }
            self.safe(2, line);
            self.wr(2, if st { b"\x1b[0m\n" } else { b"\n" });
        }
    }

    pub fn say(&mut self, style: Style, p: &[u8]) {
        self.clear();
        match style {
            Style::Assistant => return self.assistant(p),
            Style::User => return self.user(p),
            Style::Thinking => return self.thinking(p),
            _ => {}
        }
        let follow = !p.is_empty() && p[0] == b' ';
        if !self.styled(2) {
            if !follow {
                self.last = if style == Style::Tool { Gap::Tool } else { Gap::Note };
            }
            if style == Style::Tool && !follow {
                self.wr(2, b"> ");
            }
            if self.vt(2) {
                self.safe(2, p);
            } else {
                self.wr(2, p);
            }
            self.wr(2, b"\n");
            return;
        }
        if follow {
            return self.follow_up(style, p);
        }
        if style == Style::Tool {
            // "bash ls src": a dot, the tool's name in bold, what it does
            self.gap(2, Gap::Tool);
            let mut w = 0;
            while w < p.len() && p[w] != b' ' && p[w] != b'\n' {
                w += 1;
            }
            self.wr(2, if self.ascii { b"\x1b[32m*\x1b[0m \x1b[1m" } else { "\x1b[32m●\x1b[0m \x1b[1m".as_bytes() });
            self.safe(2, &p[..w]);
            self.wr(2, b"\x1b[0m");
            self.safe(2, &p[w..]);
            self.wr(2, b"\n");
            return;
        }
        self.gap(2, Gap::Note);
        self.wr(2, if style == Style::Error { b"\x1b[31m" } else { b"\x1b[2m" });
        self.safe(2, p);
        self.wr(2, b"\x1b[0m\n");
    }

    pub fn question(&mut self, tool: &[u8], desc: &[u8]) {
        self.clear();
        if !self.styled(2) {
            self.wr(2, b"\n");
            self.wr(2, tool);
            self.wr(2, b" wants to:\n");
            if self.vt(2) {
                self.safe(2, desc);
            } else {
                self.wr(2, desc);
            }
            self.wr(2, b"\nAllow? [y]es / [n]o / [a]lways this session: ");
        } else {
            self.gap(2, Gap::Question);
            self.wr(2, if self.ascii { b"\x1b[33m*\x1b[0m \x1b[1m" } else { "\x1b[33m●\x1b[0m \x1b[1m".as_bytes() });
            self.safe(2, tool);
            self.wr(2, b" wants to:\x1b[0m\n");
            // each line indented; a diff's added and removed lines coloured
            let dn = desc.len();
            let mut i = 0;
            while i < dn {
                let mut e = i;
                while e < dn && desc[e] != b'\n' {
                    e += 1;
                }
                let on: &[u8] = if e - i >= 2 && desc[i] == b'+' && desc[i + 1] == b' ' {
                    b"\x1b[32m"
                } else if e - i >= 2 && desc[i] == b'-' && desc[i + 1] == b' ' {
                    b"\x1b[31m"
                } else {
                    b""
                };
                self.wr(2, b"  ");
                self.wr(2, on);
                self.safe(2, &desc[i..e]);
                self.wr(2, if on.is_empty() { b"\n" } else { b"\x1b[0m\n" });
                i = e + 1;
            }
            self.wr(2, b"\x1b[1mAllow?\x1b[0m [y]es / [n]o / [a]lways this session: ");
        }
        self.midline = true;
        self.last = Gap::Question;
    }

    /// Shown after the question.
    pub fn answer(&mut self, ans: Answer) {
        let word: &[u8] = match ans {
            Answer::Yes => b"yes",
            Answer::Always => b"always",
            Answer::No => b"no",
        };
        let st = self.styled(2);
        if st {
            self.wr(2, if ans == Answer::No { b"\x1b[31m" } else { b"\x1b[32m" });
        }
        self.wr(2, word);
        self.wr(2, if st { b"\x1b[0m\n" } else { b"\n" });
        self.midline = false;
    }

    // ---- what is going on ----

    fn forget(&mut self, id: i64) {
        if id == 0 {
            return;
        }
        if self.http_id == id {
            self.http_id = 0;
        }
        if self.fg_id == id {
            self.fg_id = 0;
        }
        if self.retry_id == id {
            self.retry_id = 0;
        }
        if let Some(i) = self.task.iter().position(|&t| t == id) {
            self.task.swap_remove(i);
        }
        if let Some(i) = self.timer.iter().position(|&(t, _)| t == id) {
            self.timer.swap_remove(i);
        }
    }

    /// Every event, just before the agent sees it.
    pub fn event(&mut self, ev: &Event) {
        self.http_failed = false;
        match *ev {
            Event::Http { id, status, .. } => {
                if id != 0 && id == self.http_id {
                    let st = status;
                    self.http_failed = st == 0 || st == 408 || st == 429 || (500..=599).contains(&st);
                }
                self.forget(id);
            }
            Event::Proc { id, .. } | Event::Timer { id } => self.forget(id),
            _ => {}
        }
    }

    /// Every action, just before it is carried out: Say is shown here; the rest only noted. `now`
    /// is in milliseconds.
    pub fn action(&mut self, a: &Action, now: i64) {
        match a {
            Action::Say { style, text, tokens } => {
                if *style == Style::Usage {
                    // Region shows it in the footer
                    self.tokens = *tokens;
                    if self.mode != Mode::Region {
                        self.say(Style::Info, text);
                    }
                    return;
                }
                if *style == Style::Model {
                    self.model = clean(96, text).0;
                    let mut t = b"Model: ".to_vec();
                    t.extend_from_slice(text);
                    t.extend_from_slice(b", from the next request.");
                    self.say(Style::Info, &t);
                    return;
                }
                self.say(*style, text);
            }
            Action::Http { id, .. } => {
                self.http_id = *id;
                self.http_since = now;
            }
            Action::Spawn { id, cmd, background, .. } => {
                if *background {
                    if self.task.len() < OPS {
                        self.task.push(*id);
                    }
                    return;
                }
                self.fg_id = *id;
                self.fg_since = now;
                let mut k = 0;
                while k < cmd.len() && (cmd[k] == b' ' || cmd[k] == b'\n') {
                    k += 1;
                }
                let (c, more) = clean(160, &cmd[k..]);
                self.fg_cmd = c;
                self.fg_more = more;
            }
            Action::Timer { id, ms } => {
                if self.http_failed {
                    self.retry_id = *id;
                    self.retry_due = now + ms;
                    self.http_failed = false;
                    return;
                }
                if self.timer.len() < OPS {
                    self.timer.push((*id, now + ms));
                }
            }
            Action::Cancel { id } => self.forget(*id),
            _ => {}
        }
    }

    fn spinner(&self, b: &mut Buf, now: i64, colour: &[u8]) {
        let t = now.max(0);
        b.put(colour);
        if self.fancy {
            b.put(BRAILLE[(t / 120 % 10) as usize].as_bytes());
        } else {
            let k = (t / 250 % 4) as usize;
            b.put(&PLAIN[k..k + 1]);
        }
        b.put(b"\x1b[0m ");
    }

    // The activity line. true if it has a spinner (it changes with time).
    fn activity(&self, b: &mut Buf, now: i64, busy: bool) -> bool {
        let width = self.ed.width as isize - 1;
        if self.retry_id != 0 {
            self.spinner(b, now, b"\x1b[33m");
            b.put(b"the request failed; retrying in ");
            let left = self.retry_due - now;
            elapsed(b, if left > 0 { left + 999 } else { 0 });
            return true;
        }
        if self.http_id != 0 || self.fg_id != 0 {
            let since = if self.http_id != 0 { self.http_since } else { self.fg_since };
            let mut tb = Buf::new(32);
            if now - since >= 1000 {
                tb.put(self.dot());
                elapsed(&mut tb, now - since);
            }
            self.spinner(b, now, b"\x1b[35m");
            if self.http_id != 0 {
                b.put(b"waiting for the model");
            } else {
                b.put(b"running ");
                fit(b, &self.fg_cmd, width - 10 - cols_of(&tb.b), self.fg_more, false, self.ell());
            }
            b.put(b"\x1b[2m");
            b.put(&tb.b);
            b.put(b"\x1b[0m");
            return true;
        }
        if busy {
            self.spinner(b, now, b"\x1b[35m");
            b.put(b"working");
            return true;
        }
        let (nt, nm) = (self.task.len(), self.timer.len());
        if nt == 0 && nm == 0 {
            return false;
        }
        b.put(if self.ascii { b"\x1b[2mo " } else { "\x1b[2m○ ".as_bytes() });
        if nt == 1 {
            b.put(b"task ");
            b.int(self.task[0]);
            b.put(b" running");
        } else if nt > 0 {
            b.int(nt as i64);
            b.put(b" tasks running");
        }
        if nm > 0 {
            let due = self.timer.iter().map(|&(_, d)| d).min().unwrap_or(0);
            let mut hm: Vec<u8> = match &self.clock {
                Some(f) => f(due),
                None => Vec::new(),
            };
            if let Some(z) = hm.iter().position(|&c| c == 0) {
                hm.truncate(z);
            }
            hm.truncate(15);
            if nt > 0 {
                b.put(b", ");
            }
            b.put(if nm == 1 { b"a wakeup" } else { b"wakeups" });
            if !hm.is_empty() {
                b.put(if nm == 1 { b" at " } else { b", the next at " });
                b.put(&hm);
            }
        }
        b.put(self.dot());
        b.put(if nt + nm == 1 {
            if nm > 0 { b"idle until then" } else { b"idle until it ends" }
        } else {
            b"idle until one of them"
        });
        b.put(b"\x1b[0m");
        false
    }

    fn footer(&self, b: &mut Buf, busy: bool) {
        let mut l = Buf::new(DECOR);
        l.put(b"  ");
        l.put(&self.model);
        if !self.cwd.is_empty() {
            l.put(self.dot());
            fit(&mut l, &self.cwd, 32, false, true, self.ell());
        }
        if self.tokens != 0 {
            l.put(self.dot());
            tokens(&mut l, self.tokens);
            l.put(b" tokens");
        }
        let hint: &[u8] = if busy {
            if !self.ed.line.is_empty() { b"Enter queues it" } else { b"Ctrl-C stops it" }
        } else {
            b"/help for commands"
        };
        let (lc, hc, width) = (cols_of(&l.b), cols_of(hint), self.ed.width as isize - 1);
        b.put(b"\x1b[2m");
        b.put(&l.b);
        if lc + 3 + hc <= width {
            b.spaces(width - lc - hc);
            b.put(hint);
        }
    }

    fn rule(&self, b: &mut Buf) {
        let n = (self.ed.width as isize - 1).min(160);
        b.put(b"\x1b[2m");
        for _ in 0..n.max(0) {
            b.put(if self.ascii { b"-" } else { "─".as_bytes() });
        }
    }

    /// Bring the bottom of the screen up to date. busy: the agent is working. prompt_ok: the user
    /// can type (Region: the block is shown). status_ok: a status line may be shown when the block
    /// is not. Cheap when nothing changed.
    pub fn draw(&mut self, now: i64, busy: bool, prompt_ok: bool, status_ok: bool) {
        let mut ab = Buf::new(DECOR);
        self.animating = self.activity(&mut ab, now, busy);
        let act = ab.b;
        if self.mode == Mode::Region && prompt_ok {
            self.status_clear();
            if self.midline {
                self.wr(2, b"\n");
                self.midline = false;
            }
            self.ed.set_prompt(if busy { b"\x1b[2m> \x1b[0m" } else { b"\x1b[1;35m> \x1b[0m" }, 2);
            self.ed.set_above(0, &act);
            let mut rb = Buf::new(DECOR);
            self.rule(&mut rb);
            self.ed.set_above(1, &rb.b);
            let mut fb = Buf::new(DECOR);
            self.footer(&mut fb, busy);
            self.ed.set_below(&fb.b);
            if self.ed.shown {
                self.ed.refresh();
            } else {
                self.ed.show();
            }
            self.flush_ed();
            return;
        }
        if self.ed.shown {
            self.ed.hide();
            self.flush_ed();
        }
        if !status_ok || self.mode == Mode::Pipe || !self.vt_err || !self.animating || self.midline {
            self.status_clear();
            self.animating = false; // nothing on the screen changes with time
            return;
        }
        // Lite: the activity alone, on the row the cursor is on, cut to fit.
        let mut lb = Buf::new(DECOR);
        let width = self.width as isize - 1;
        let mut c = 0;
        lb.put(b"\r");
        let n = act.len();
        let mut i = 0;
        while i < n {
            if act[i] == 0x1B {
                let mut e = i + 1;
                while e < n && !((0x40..=0x7E).contains(&act[e]) && e > i + 1) {
                    e += 1;
                }
                if e >= n {
                    break;
                }
                lb.put(&act[i..e + 1]);
                i = e + 1;
                continue;
            }
            let (ch, len) = dec(&act[i..]);
            let w = edit::cols(ch) as isize;
            if c + w > width {
                break;
            }
            lb.put(&act[i..i + len]);
            c += w;
            i += len;
        }
        lb.put(b"\x1b[0m\x1b[K");
        if self.status_shown && lb.b == self.status {
            return;
        }
        self.wr(2, &lb.b);
        self.status = lb.b;
        self.status_shown = true;
    }

    /// Milliseconds until [`Screen::draw`] has something new to show (the spinner), or -1.
    pub fn next_ms(&self, now: i64) -> i64 {
        if !self.animating {
            return -1;
        }
        let period = if self.fancy { 120 } else { 250 };
        period - now.max(0) % period
    }

    // ---- the line ----

    pub fn key(&mut self, k: Key) {
        self.ed.key(k);
        self.flush_ed();
    }

    /// Enter: the line. The prompt and the line stay as the record, with a blank line above them.
    pub fn take(&mut self) -> Vec<u8> {
        self.clear();
        self.gap(2, Gap::User);
        self.ed.show();
        self.flush_ed();
        let line = self.ed.take();
        self.flush_ed();
        line
    }

    /// Enter while a question waits, when the line is not on the screen: the line, taken.
    pub fn take_unshown(&mut self) -> Vec<u8> {
        let line = self.ed.take();
        self.flush_ed();
        line
    }

    /// A new width.
    pub fn resize(&mut self, width: usize) {
        self.status_clear();
        let width = width.max(10);
        self.ed.resize(width);
        self.flush_ed();
        self.width = width;
        self.md.width = width;
    }
}
