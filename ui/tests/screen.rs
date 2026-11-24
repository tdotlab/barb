// Tests for the terminal's screen (ui/src/screen.rs): the record, the block at the bottom with its
// activity line, rule, prompt and footer, the one-line status, and the plain output of Pipe mode.
//
// The screen writes bytes for a terminal, so the tests play them on the small terminal of
// common/mod.rs (with output processing, as a terminal in raw mode still has: "\n" starts a new
// row) and look at the rows. A random run at the end mixes everything and checks the screen after
// each step.
mod common;

use barb_core::agent::{Action, Answer, Event, Exit, Style};
use barb_ui::edit::{self, Key};
use barb_ui::screen::{Config, Mode, Screen};
use common::Term;

const TR: usize = 60;
const TC: usize = 100;

struct X {
    t: Term,
    sc: Screen,
    out: Vec<u8>, // everything written, in order
    fd1: Vec<u8>,
    fd2: Vec<u8>,
}

fn clock(at: i64) -> Vec<u8> {
    let m = at / 60000 % 1440;
    let d = |v: i64| b'0' + v as u8;
    vec![d(m / 600), d(m / 60 % 10), b':', d(m % 60 / 10), d(m % 10)]
}

#[allow(clippy::too_many_arguments)]
fn setup_full(mode: Mode, vt: bool, width: usize, deferred: bool, ascii: bool, model: &[u8], cwd: &[u8], home: Option<&[u8]>) -> X {
    let mut t = Term::new(TR, TC, width, deferred);
    t.scroll = true;
    let sc = Screen::new(Config {
        mode,
        vt_out: vt,
        vt_err: vt,
        fancy: !ascii,
        ascii,
        width,
        model,
        cwd,
        home,
        clock: Some(Box::new(clock)),
    });
    let mut x = X { t, sc, out: Vec::new(), fd1: Vec::new(), fd2: Vec::new() };
    x.pump();
    x
}

fn setup(mode: Mode, vt: bool, width: usize) -> X {
    setup_full(mode, vt, width, true, false, b"m1", b"/w", None)
}

fn http(id: i64) -> Action {
    Action::Http { id, path: "/v1/messages", body: Vec::new() }
}
fn spawn(id: i64, background: bool, cmd: &[u8]) -> Action {
    Action::Spawn { id, cmd: cmd.to_vec(), timeout_ms: 10000, background, log: Vec::new() }
}
fn timer(id: i64, ms: i64) -> Action {
    Action::Timer { id, ms }
}
fn cancel(id: i64) -> Action {
    Action::Cancel { id }
}
fn sayact(style: Style, tokens: i64, text: &[u8]) -> Action {
    Action::Say { style, text: text.to_vec(), tokens }
}
fn ev_http(id: i64, status: i32) -> Event<'static> {
    Event::Http { id, status, body: b"", note: b"" }
}
fn ev_proc(id: i64) -> Event<'static> {
    Event::Proc { id, exit: Exit::Code(0), out: b"", ms: 0, note: b"" }
}
fn ev_timer(id: i64) -> Event<'static> {
    Event::Timer { id }
}

fn contains(hay: &[u8], needle: &[u8]) -> bool {
    needle.is_empty() || hay.windows(needle.len()).any(|w| w == needle)
}

fn cols(s: &str) -> usize {
    edit::text_cols(s.as_bytes())
}

// Every escape is a CSI the terminal knows: colours, erasing, moving the cursor.
fn escapes_ok(s: &[u8]) -> bool {
    let n = s.len();
    let mut i = 0;
    while i < n {
        if s[i] == 0x1B {
            if i + 1 >= n || s[i + 1] != b'[' {
                return false;
            }
            let mut j = i + 2;
            while j < n && (s[j].is_ascii_digit() || s[j] == b';') {
                j += 1;
            }
            if j >= n || !b"mKJABC".contains(&s[j]) {
                return false;
            }
            i = j;
        }
        i += 1;
    }
    true
}

impl X {
    // Play what the screen wrote on the terminal, a "\n" as "\r\n".
    fn pump(&mut self) {
        for (fd, p) in self.sc.take_out() {
            self.out.extend_from_slice(&p);
            if fd == 1 { &mut self.fd1 } else { &mut self.fd2 }.extend_from_slice(&p);
            for (k, piece) in p.split(|&c| c == b'\n').enumerate() {
                if k > 0 {
                    self.t.feed(b"\r\n");
                }
                self.t.feed(piece);
            }
        }
    }
    fn go<R>(&mut self, f: impl FnOnce(&mut Screen) -> R) -> R {
        let r = f(&mut self.sc);
        self.pump();
        r
    }
    fn say(&mut self, style: Style, p: &[u8]) {
        self.go(|s| s.say(style, p));
    }
    fn act(&mut self, a: Action, now: i64) {
        self.go(|s| s.action(&a, now));
    }
    fn ev(&mut self, e: Event) {
        self.go(|s| s.event(&e));
    }
    fn draw(&mut self, now: i64, busy: bool, prompt_ok: bool, status_ok: bool) {
        self.go(|s| s.draw(now, busy, prompt_ok, status_ok));
    }
    fn typ(&mut self, s: &str) {
        for c in s.bytes() {
            self.go(|sc| sc.key(Key::Char(c as u32)));
        }
    }
    fn take(&mut self) -> Vec<u8> {
        self.go(|s| s.take())
    }
    fn question(&mut self, tool: &[u8], desc: &[u8]) {
        self.go(|s| s.question(tool, desc));
    }
    fn answer(&mut self, a: Answer) {
        self.go(|s| s.answer(a));
    }
    fn hide(&mut self) {
        self.go(|s| s.hide());
    }
    fn row(&self, r: usize) -> String {
        self.t.row(r)
    }
    fn find_row(&self, text: &str) -> Option<usize> {
        (0..TR).find(|&r| self.row(r).contains(text))
    }
    fn has(&self, text: &[u8]) -> bool {
        contains(&self.out, text)
    }
    fn top(&self) -> usize {
        self.t.r - self.sc.ed.row
    }
    fn dump(&self, why: &str) -> String {
        let mut s = format!("--- {} (cursor {},{} bad {})\n", why, self.t.r, self.t.c, self.t.bad);
        let last = (0..TR).filter(|&r| !self.row(r).is_empty()).max().unwrap_or(0);
        for r in 0..(last + 2).min(TR) {
            s += &format!("{:2}|{}|\n", r, self.row(r));
        }
        s
    }
    fn empty_from(&self, r: usize) -> bool {
        (r..TR).all(|i| self.row(i).is_empty())
    }

    fn check_block(&self, top: usize, activity: &str, line: &str, footer_left: &str, hint: &str) {
        let d = self.dump(activity);
        assert_eq!(self.row(top), activity, "{d}");
        let r = self.row(top + 1);
        assert_eq!(cols(&r), (self.t.width - 1).min(160), "{d}");
        assert!(r.starts_with("──"), "{d}");
        assert_eq!(self.row(top + 2), line, "{d}");
        let f = self.row(top + 3);
        assert!(f.starts_with(footer_left), "{d}");
        if !hint.is_empty() {
            assert!(f.ends_with(hint), "{d}");
            assert_eq!(cols(&f), self.t.width - 1, "{d}");
        } else {
            assert_eq!(f, footer_left, "{d}");
        }
        assert!(self.empty_from(top + 4), "{d}");
    }
}

// ---- Pipe: as barb always wrote it ----

#[test]
fn pipe() {
    let mut x = setup(Mode::Pipe, false, 80);
    x.go(|s| s.banner(b"0.2.0"));
    x.say(Style::Tool, b"bash ls");
    x.say(Style::Tool, b"  exit 0");
    x.say(Style::Info, b"a note");
    x.say(Style::Error, b"  error: nope");
    x.question(b"write_file", b"create a.txt");
    x.answer(Answer::Yes);
    x.act(sayact(Style::Usage, 11, b"[tokens: 5 in, 6 out]"), 0);
    x.say(Style::Assistant, b"**hi**\n");
    assert_eq!(
        String::from_utf8_lossy(&x.fd2),
        "barb 0.2.0, model m1, in /w\n/help for commands; Ctrl-D to exit.\n\
         > bash ls\n  exit 0\na note\n  error: nope\n\
         \nwrite_file wants to:\ncreate a.txt\nAllow? [y]es / [n]o / [a]lways this session: yes\n\
         [tokens: 5 in, 6 out]\n"
    );
    assert_eq!(x.fd1, b"**hi**\n\n");
    assert!(!x.out.contains(&0x1B));
    let before = x.out.len();
    x.act(http(1), 0);
    x.draw(5000, true, false, true);
    assert_eq!(x.out.len(), before); // no status line
    assert_eq!(x.sc.next_ms(5000), -1);
    x.hide();
    assert_eq!(x.out.len(), before);

    // A terminal on standard output alone: replies are drawn as markdown there, the rest is plain.
    let mut x = setup(Mode::Pipe, false, 80);
    x.sc.vt_out = true;
    x.say(Style::Assistant, b"some **bold** words");
    assert!(contains(&x.fd1, b"bold") && !contains(&x.fd1, b"**"));
    assert_ne!(x.fd1[0], b'\n'); // no blank line before the first thing
}

// ---- Lite: styled, with a status line ----

#[test]
fn lite() {
    let mut x = setup(Mode::Lite, true, 80);
    x.go(|s| s.banner(b"0.2.0"));
    assert_eq!(x.row(0), "barb 0.2.0 · m1 · /w");
    assert_eq!(x.row(1), "/help for commands · Ctrl-D to exit");
    x.say(Style::Tool, b"bash ls -la");
    assert_eq!(x.row(2), "");
    assert_eq!(x.row(3), "● bash ls -la");
    assert!(x.has(b"\x1b[1mbash\x1b[0m ls -la"));
    x.say(Style::Tool, b"  exit 0, 3 lines");
    assert_eq!(x.row(4), "  └ exit 0, 3 lines");
    x.say(Style::Error, b"  error: no such file\nsecond line");
    assert_eq!(x.row(5), "  └ error: no such file");
    assert_eq!(x.row(6), "    second line");
    x.say(Style::Assistant, b"Some **bold** text\n");
    assert_eq!(x.row(7), "");
    assert_eq!(x.row(8), "Some bold text");
    assert_eq!(x.row(9), "");
    x.say(Style::Info, b"first note");
    x.say(Style::Info, b"second note");
    x.say(Style::Error, b"an error");
    assert_eq!(x.row(9), "");
    assert_eq!(x.row(10), "first note");
    assert_eq!(x.row(11), "second note");
    assert_eq!(x.row(12), "an error");
    assert!(x.has(b"\x1b[31man error\x1b[0m"));
    assert!(x.t.r == 13 && x.t.c == 0);

    // The status line, while a request is out.
    x.act(http(1), 1000);
    x.draw(1000, true, false, true);
    assert!(x.row(13).contains("waiting for the model"));
    assert!(!x.row(13).contains("·")); // no time until a second has passed
    let before = x.out.len();
    x.draw(1010, true, false, true);
    assert_eq!(x.out.len(), before); // nothing changed, nothing written
    assert_eq!(x.sc.next_ms(1010), 120 - 1010 % 120);
    x.draw(13000, true, false, true);
    assert!(x.row(13).contains("waiting for the model · 12s"));
    assert_eq!(x.t.r, 13);
    x.draw(13000, true, false, false); // a question is coming: it goes
    assert_eq!(x.row(13), "");
    assert_eq!(x.sc.next_ms(13000), -1);
    x.draw(14000, true, false, true);
    x.say(Style::Info, b"a third"); // output takes its place
    assert_eq!(x.row(13), "a third");
    assert!(x.find_row("waiting").is_none());
    x.act(sayact(Style::Usage, 11, b"[tokens: 5 in, 6 out]"), 0);
    assert_eq!(x.row(14), "[tokens: 5 in, 6 out]");
    x.ev(ev_http(1, 200));
    x.draw(15000, false, false, true);
    assert_eq!(x.row(15), "");
    assert_eq!(x.sc.next_ms(15000), -1);
    // A narrow terminal: the status line is cut to fit, and never wraps.
    let mut x = setup(Mode::Lite, true, 20);
    x.act(spawn(2, false, b"a-very-long-command-line --with arguments"), 0);
    x.draw(0, true, false, true);
    assert!(cols(&x.row(0)) <= 19);
    assert_eq!(x.row(1), "");
    assert!(escapes_ok(&x.out));
}

// ---- Region: the block ----

#[test]
fn region() {
    let mut x = setup_full(Mode::Region, true, 100, true, false, b"m1", b"/home/t/proj", Some(b"/home/t"));
    x.go(|s| s.banner(b"0.2.0"));
    assert_eq!(x.row(0), "barb 0.2.0 · m1 · ~/proj");
    x.draw(0, false, true, true);
    x.check_block(2, "", ">", "  m1 · ~/proj", "/help for commands");
    assert!(x.t.r == 4 && x.t.c == 2);
    assert!(x.has(b"\x1b[1;35m> \x1b[0m"));
    x.typ("hi");
    assert_eq!(x.row(4), "> hi");

    // Working: the activity line, a dim prompt, the hint.
    x.act(http(1), 0);
    x.draw(5000, true, true, true);
    x.check_block(2, "⠙ waiting for the model · 5s", "> hi", "  m1", "Enter queues it");
    assert!(x.t.r == 4 && x.t.c == 4);
    x.go(|s| s.key(Key::Clear));
    x.draw(5000, true, true, true);
    x.check_block(2, "⠙ waiting for the model · 5s", ">", "  m1", "Ctrl-C stops it");
    assert!(x.has(b"\x1b[2m> \x1b[0m"));
    assert_eq!(x.sc.next_ms(5000), 120 - 5000 % 120);

    // Output goes above the block, which comes back under it.
    x.say(Style::Assistant, b"Hello there");
    assert!(!x.sc.ed.shown);
    assert_eq!(x.row(2), "");
    assert_eq!(x.row(3), "Hello there");
    assert!(x.empty_from(4));
    x.act(sayact(Style::Usage, 12400, b"[tokens: 12000 in, 400 out]"), 0);
    assert!(x.find_row("[tokens").is_none()); // not printed: the footer has it
    x.ev(ev_http(1, 200));
    x.draw(6000, false, true, true);
    x.check_block(4, "", ">", "  m1 · ~/proj · 12.4k tokens", "/help for commands");
    assert_eq!(x.sc.next_ms(6000), -1);

    // Enter: the line stays as the record, under a blank row; the next line follows it directly.
    x.typ("do it");
    assert_eq!(x.take(), b"do it");
    assert_eq!(x.row(4), "");
    assert_eq!(x.row(5), "> do it");
    assert!(x.t.r == 6 && x.t.c == 0);
    assert!(x.empty_from(6));
    x.draw(6000, true, true, true);
    x.typ("more");
    assert_eq!(x.take(), b"more");
    assert_eq!(x.row(6), "> more");
    x.say(Style::Info, b"(queued: it goes to the model when the current step is done)");
    assert_eq!(x.row(7), "(queued: it goes to the model when the current step is done)");
    x.draw(6000, true, true, true);
    x.check_block(8, "⠋ working", ">", "  m1", "Ctrl-C stops it");

    // Hidden for good (the end of input): the block goes and nothing is left of it.
    x.draw(6000, true, false, true);
    assert!(!x.sc.ed.shown);
    assert!(x.sc.status_shown); // the status line takes over
    assert!(x.row(8).contains("working"));
    assert!(x.empty_from(9));
    x.hide();
    assert_eq!(x.row(8), "");
    assert!(x.t.r == 8 && x.t.c == 0);
    assert!(escapes_ok(&x.out));
}

#[test]
fn activity() {
    let mut x = setup(Mode::Region, true, 100);
    x.act(spawn(3, false, b"ls -la"), 0);
    x.draw(4000, true, true, true);
    assert!(x.row(0).contains(" running ls -la · 4s"));
    x.draw(65000, true, true, true);
    assert!(x.row(0).contains(" running ls -la · 1m 05s"));
    x.draw(3725000, true, true, true);
    assert!(x.row(0).contains(" running ls -la · 1h 02m"));
    x.ev(ev_proc(3));
    x.draw(3725000, false, true, true);
    assert_eq!(x.row(0), "");

    x.act(spawn(4, false, &[b'x'; 200]), 0);
    x.draw(2000, true, true, true);
    let r = x.row(0);
    assert_eq!(cols(&r), 99);
    assert!(r.contains("x… · 2s"));
    x.ev(ev_proc(4));
    x.act(spawn(5, false, b"\n  echo a\necho b"), 0);
    x.draw(0, true, true, true);
    assert!(x.row(0).contains(" running echo a…"));
    x.act(cancel(5), 0);
    x.draw(0, false, true, true);
    assert_eq!(x.row(0), "");

    // A failed request, and the wait before it is tried again.
    x.act(http(6), 0);
    x.ev(ev_http(6, 503));
    x.act(sayact(Style::Info, 0, b"API returned HTTP 503; retrying in 2s (attempt 1 of 5)"), 10000);
    x.act(timer(7, 2000), 10000);
    x.draw(10000, true, true, true);
    let top = x.top();
    assert!(x.row(top).contains(" the request failed; retrying in 2s"));
    x.draw(11500, true, true, true);
    assert!(x.row(top).contains(" retrying in 1s"));
    x.ev(ev_timer(7));
    x.act(http(8), 12000);
    x.draw(12000, true, true, true);
    assert!(x.row(top).contains(" waiting for the model"));
    x.ev(ev_http(8, 400)); // not retried: no retry line from the next timer
    x.act(timer(9, 60000), 0);
    x.draw(12000, false, true, true);
    assert_eq!(x.row(top), "○ a wakeup at 00:01 · idle until then");

    // Background tasks and timers, while the agent waits for them.
    x.act(spawn(10, true, b"make"), 0);
    x.draw(12000, false, true, true);
    assert_eq!(x.row(top), "○ task 10 running, a wakeup at 00:01 · idle until one of them");
    assert_eq!(x.sc.next_ms(12000), -1);
    x.act(timer(11, 120000), 0);
    x.act(spawn(12, true, b"make"), 0);
    x.draw(12000, false, true, true);
    assert_eq!(x.row(top), "○ 2 tasks running, wakeups, the next at 00:01 · idle until one of them");
    x.ev(ev_timer(9));
    x.ev(ev_timer(11));
    x.act(cancel(12), 0);
    x.draw(12000, false, true, true);
    assert_eq!(x.row(top), "○ task 10 running · idle until it ends");
    x.draw(12000, true, true, true); // working beats waiting
    assert!(x.row(top).contains(" working"));
    x.ev(ev_proc(10));
    x.draw(12000, false, true, true);
    assert_eq!(x.row(top), "");

    // The tokens in the footer.
    for (n, s) in [
        (999, "999 tokens"),
        (1000, "1.0k tokens"),
        (12400, "12.4k tokens"),
        (99999, "99.9k tokens"),
        (123456, "123k tokens"),
        (1500000, "1.5M tokens"),
        (250000000, "250M tokens"),
    ] {
        x.act(sayact(Style::Usage, n, b"[tokens]"), 0);
        x.draw(0, false, true, true);
        assert!(x.row(x.t.r + 1).contains(s), "{s}");
    }
    assert!(escapes_ok(&x.out));
}

#[test]
fn cwd() {
    let mut x = setup_full(Mode::Region, true, 100, true, false, b"m1", b"/home/tom", Some(b"/home/t"));
    x.draw(0, false, true, true);
    assert!(x.row(3).starts_with("  m1 · /home/tom "));
    let mut x = setup_full(Mode::Region, true, 100, true, false, b"m1", b"/home/t", Some(b"/home/t/"));
    x.draw(0, false, true, true);
    assert!(x.row(3).starts_with("  m1 · ~ "));
    let mut x = setup_full(Mode::Region, true, 100, true, false, b"m1", b"C:\\Users\\t\\src", Some(b"C:\\Users\\t"));
    x.draw(0, false, true, true);
    assert!(x.row(3).starts_with("  m1 · ~\\src "));
    let mut x = setup_full(Mode::Region, true, 100, true, false, b"m1", b"/srv/a/very/long/path/that/goes/on/and/on/to/the/end", None);
    x.draw(0, false, true, true);
    assert!(x.row(3).contains("  m1 · …/that/goes/on/and/on/to/the/end "));
    // Too narrow for the hint: it goes, and the rest is cut.
    let mut x = setup_full(Mode::Region, true, 30, true, false, b"a-long-model-name", b"/srv/proj", None);
    x.draw(0, false, true, true);
    assert!(!x.row(3).contains("/help"));
    assert!(cols(&x.row(3)) <= 29);
    assert_eq!(x.row(4), "");
}

#[test]
fn question() {
    let mut x = setup(Mode::Region, true, 100);
    x.draw(0, false, true, true);
    x.typ("typed");
    x.question(b"write_file", b"create a.txt\n+ new line\n- old line");
    assert!(!x.sc.ed.shown);
    assert_eq!(x.row(0), "● write_file wants to:");
    assert_eq!(x.row(1), "  create a.txt");
    assert_eq!(x.row(2), "  + new line");
    assert_eq!(x.row(3), "  - old line");
    assert_eq!(x.row(4), "Allow? [y]es / [n]o / [a]lways this session:");
    assert!(x.has(b"\x1b[32m+ new line\x1b[0m") && x.has(b"\x1b[31m- old line\x1b[0m"));
    let before = x.out.len();
    x.draw(0, true, false, false); // while it waits, nothing is drawn
    assert_eq!(x.out.len(), before);
    x.answer(Answer::Always);
    assert_eq!(x.row(4), "Allow? [y]es / [n]o / [a]lways this session: always");
    assert!(x.has(b"\x1b[32malways\x1b[0m"));
    x.draw(0, true, true, true);
    let r5 = x.row(5);
    x.check_block(5, &r5, "> typed", "  m1", "Enter queues it");
    // A question that goes unanswered (the agent cancelled it): its line is ended.
    x.question(b"bash", b"run: ls");
    assert_eq!(x.row(5), "");
    assert_eq!(x.row(6), "● bash wants to:");
    x.hide();
    assert!(x.t.r == 9 && x.t.c == 0);
    x.question(b"bash", b"run: ls");
    x.answer(Answer::No);
    assert!(x.has(b"\x1b[31mno\x1b[0m"));
    assert!(escapes_ok(&x.out));
}

#[test]
fn safety() {
    let mut x = setup_full(Mode::Region, true, 100, true, false, b"m\x1b[31mx", b"/w\x1b]0;t\x07", None);
    x.say(Style::Error, b"bad \x1b]0;pwned\x07 \x1b[2J x");
    x.say(Style::Tool, b"ba\x1b[2Jsh run");
    x.say(Style::Tool, b"  \x1b[1Aup");
    x.say(Style::Info, b"c1 \xC2\x9B2J");
    x.question(b"t\x1b]x", b"d\x1b[B");
    x.answer(Answer::Yes);
    x.say(Style::Assistant, b"reply \x1b[2J");
    x.act(spawn(1, false, b"cmd \x1b[2J \xC2\x9Bx \x07"), 0);
    x.draw(0, true, true, true);
    assert!(escapes_ok(&x.out));
    assert!(x.has(b"^["));
    assert_eq!(x.t.bad, 0);
    assert!(x.row(x.top()).contains("running cmd  [2J ?x"));
    assert!(x.row(x.t.r + 1).contains("  m [31mx · /w ]0;t "));
    // In Pipe mode text goes as it came, unless standard error is a terminal after all.
    let mut x = setup(Mode::Pipe, false, 80);
    x.say(Style::Info, b"raw \x1b[1m");
    assert!(x.has(b"raw \x1b[1m"));
    let mut x = setup(Mode::Pipe, true, 80);
    x.say(Style::Info, b"raw \x1b[1m");
    assert!(!x.out.contains(&0x1B));
}

// Corners the walk-throughs above do not reach.
#[test]
fn corners() {
    // A 429 is retried as a 5xx is: the timer after it is the retry.
    let mut x = setup(Mode::Region, true, 100);
    x.act(http(1), 0);
    x.ev(ev_http(1, 429));
    x.act(timer(2, 3000), 0);
    x.draw(0, true, true, true);
    assert!(x.row(0).contains("the request failed; retrying in 3s"));
    // A failure counts only for the timer set right after it: anything else in between and the
    // next timer is the model's own.
    let mut x = setup(Mode::Region, true, 100);
    x.act(http(1), 0);
    x.ev(ev_http(1, 500));
    x.ev(Event::Input(b""));
    x.act(timer(2, 60000), 0);
    x.draw(0, false, true, true);
    assert_eq!(x.row(0), "○ a wakeup at 00:01 · idle until then");
    // Tasks that end out of order leave the right one.
    let mut x = setup(Mode::Region, true, 100);
    for i in 1..=3 {
        x.act(spawn(i, true, b"make"), 0);
    }
    x.ev(ev_proc(1));
    x.ev(ev_proc(2));
    x.draw(0, false, true, true);
    assert!(x.row(0).contains("task 3 running"));
    // A command cut to fit its store is marked as cut even when what is left fits the row.
    let mut x = setup(Mode::Region, true, 100);
    x.act(spawn(1, false, "€".repeat(60).as_bytes()), 0);
    x.draw(0, true, true, true);
    assert!(x.row(0).contains("€…"));
    // The hint needs three columns of space before it, or it goes.
    let mut x = setup(Mode::Region, true, 29);
    x.draw(0, false, true, true);
    assert!(x.row(3).starts_with("  m1 · /w"));
    assert!(!x.row(3).contains("/help"));
    // An overlong UTF-8 form never reaches the terminal.
    let mut x = setup_full(Mode::Region, true, 100, true, false, b"m\xC0\xAF", b"/w", None);
    x.go(|s| s.banner(b"0.2.0"));
    x.draw(0, false, true, true);
    assert!(!x.out.contains(&0xC0));
    assert!(x.row(x.t.r + 1).contains("  m?? · /w"));

    // While a question waits, nothing is drawn over it, whatever the caller says.
    let mut x = setup(Mode::Lite, true, 80);
    x.act(http(1), 0);
    x.draw(0, true, false, true);
    assert!(x.row(0).contains("waiting for the model"));
    x.question(b"bash", b"run: ls");
    let q = x.find_row("Allow?").expect("the question");
    assert!(q > 0);
    x.draw(0, true, false, false);
    assert_eq!(x.row(q), "Allow? [y]es / [n]o / [a]lways this session:");
    x.draw(0, true, false, true);
    assert_eq!(x.row(q), "Allow? [y]es / [n]o / [a]lways this session:");
    assert_eq!(x.sc.next_ms(0), -1);
    // And a prompt drawn under an open question starts on a row of its own.
    let mut x = setup(Mode::Region, true, 100);
    x.question(b"bash", b"run: ls");
    x.draw(0, false, true, true);
    let q = x.find_row("Allow?").expect("the question");
    assert_eq!(x.row(q), "Allow? [y]es / [n]o / [a]lways this session:");
    x.check_block(q + 1, "", ">", "  m1", "/help for commands");
    assert!(escapes_ok(&x.out));
}

#[test]
fn ascii() {
    let mut x = setup_full(Mode::Region, true, 80, true, true, b"m1", b"/a/very/long/path/that/goes/on/and/on/to/the/end", None);
    x.go(|s| s.banner(b"0.2.0"));
    x.say(Style::Tool, b"bash ls");
    x.say(Style::Tool, b"  exit 0");
    x.say(Style::Assistant, b"- item\n\n---");
    x.question(b"bash", b"run: ls");
    x.answer(Answer::Yes);
    x.act(http(1), 0);
    x.act(spawn(2, true, b"make"), 0);
    x.draw(3000, true, true, true);
    assert!(x.out.iter().all(|&c| c < 0x80));
    assert!(x.row(x.top()).contains("- waiting for the model - 3s"));
    assert!(x.row(x.top() + 1).starts_with("-----"));
    assert!(x.row(x.t.r + 1).contains("..."));
    assert_eq!(x.sc.next_ms(3000), 250 - 3000 % 250);
    x.ev(ev_http(1, 200));
    x.draw(3000, false, true, true);
    assert_eq!(x.row(x.top()), "o task 2 running - idle until it ends");
}

#[test]
fn resize() {
    let mut x = setup(Mode::Region, true, 60);
    x.draw(0, false, true, true);
    assert_eq!(cols(&x.row(1)), 59);
    x.go(|s| s.resize(90));
    x.t.width = 90; // the terminal, rewrapped (the block is gone from it)
    assert!(!x.sc.ed.shown);
    x.draw(0, false, true, true);
    let top = x.top();
    assert_eq!(cols(&x.row(top + 1)), 89);
    x.say(Style::Assistant, b"word word word word word word word word word word word word word word word word word word word");
    for r in 0..TR {
        assert!(cols(&x.row(r)) <= 89);
    }
    assert!(x.find_row("word word word word word word word word word word word word word word word word word").is_some());
}

// ---- random sessions ----

struct Rng(u32);
impl Rng {
    fn next(&mut self) -> u32 {
        self.0 = self.0.wrapping_mul(1103515245).wrapping_add(12345);
        (self.0 >> 8) & 0xFFFFFF
    }
}

const BITS: &[&[u8]] = &[
    b"word", b" ", b"  ", b"\n", b"x", b"**b**", b"`c`", b"- ", b"# ", b"| a | b |", b"\x1b[2J", b"\x1b]0;t\x07", b"\t",
    b"\xC3\xA9", b"\xE4\xB8\xAD", b"\xC2\x9B", b"\xFF", b"longerwordthatgoeson", b"1. ", b"> ", b"+ ", b"\r\n",
];

fn random_text(g: &mut Rng) -> Vec<u8> {
    let mut b = Vec::new();
    let n = g.next() % 12;
    for _ in 0..n {
        let p = BITS[(g.next() as usize) % BITS.len()];
        if b.len() + p.len() + 1 > 512 {
            break;
        }
        b.extend_from_slice(p);
    }
    b
}

fn check_screen(x: &X, mode: Mode, awaiting: bool, what: &str) {
    if mode == Mode::Pipe && !x.sc.vt_err {
        return; // raw bytes, as they came
    }
    let d = || x.dump(what);
    assert_eq!(x.t.bad, 0, "{}", d());
    if x.sc.ed.shown {
        assert!(x.t.r >= x.sc.ed.row, "{}", d());
        let top = x.top();
        assert!(cols(&x.row(top)) < x.t.width, "{}", d());
        assert!(x.row(top + 1).starts_with(if x.sc.ascii { "--" } else { "─" }), "{}", d());
        assert!(x.row(top + 2).starts_with('>'), "{}", d());
        let last = (top..TR).filter(|&i| !x.row(i).is_empty()).max();
        assert!(last.is_some_and(|l| l > top + 2 && x.row(l).starts_with("  m1")), "{}", d());
    } else if !awaiting && !x.sc.status_shown && mode != Mode::Pipe {
        assert_eq!(x.t.c, 0, "{}", d());
        assert!(x.empty_from(x.t.r), "{}", d());
    }
}

#[test]
fn random() {
    const KEYS: &[Key] = &[
        Key::Back, Key::Del, Key::Left, Key::Right, Key::Home, Key::End, Key::Up, Key::Down, Key::KillWord, Key::Clear,
    ];
    const STYLES: [Style; 4] = [Style::Assistant, Style::Tool, Style::Info, Style::Error];
    const ANSWERS: [Answer; 3] = [Answer::No, Answer::Yes, Answer::Always];
    let mut g = Rng(4242);
    for round in 0..300 {
        let mode = [Mode::Pipe, Mode::Lite, Mode::Region][(g.next() % 3) as usize];
        let width = 20 + (g.next() as usize) % (TC - 20 + 1);
        let vt = mode != Mode::Pipe || g.next() % 2 == 1;
        let deferred = g.next() % 2 == 1;
        let ascii = g.next() % 4 == 0;
        let mut x = setup_full(mode, vt, width, deferred, ascii, b"m1", b"/w", None);
        let (mut now, mut id) = (0i64, 1i64);
        let (mut awaiting, mut busy) = (false, false);
        let mut open: Vec<i64> = Vec::new();
        for step in 0..200 {
            now += (g.next() % 3000) as i64;
            let what = g.next() % 14;
            match what {
                0 | 1 => {
                    let t = random_text(&mut g);
                    let st = STYLES[(g.next() % 4) as usize];
                    x.say(st, &t);
                }
                2 | 3 => {
                    let prompt_ok = mode == Mode::Region && !awaiting;
                    x.draw(now, busy, prompt_ok, !awaiting);
                }
                4 => {
                    if !awaiting {
                        let r = g.next() % 4;
                        let k = if r == 0 {
                            KEYS[(g.next() as usize) % KEYS.len()]
                        } else if r == 1 {
                            Key::Char(0x4E2D)
                        } else {
                            Key::Char('a' as u32 + g.next() % 26)
                        };
                        x.go(|s| s.key(k));
                    }
                }
                5 => {
                    if !awaiting && mode == Mode::Region {
                        x.take();
                    }
                }
                6 => {
                    if !awaiting {
                        let t = random_text(&mut g);
                        x.question(b"bash", &t);
                        awaiting = true;
                    } else {
                        x.answer(ANSWERS[(g.next() % 3) as usize]);
                        awaiting = false;
                    }
                }
                7 => {
                    let is_http = g.next() % 2 == 1;
                    let t = random_text(&mut g);
                    let flag = g.next() % 2 == 1;
                    let a = if is_http { http(id) } else { spawn(id, flag, &t) };
                    x.act(a, now);
                    if open.len() < 8 {
                        open.push(id);
                    }
                    id += 1;
                    busy = true;
                }
                8 => {
                    let ms = (g.next() % 100000) as i64;
                    x.act(timer(id, ms), now);
                    if open.len() < 8 {
                        open.push(id);
                    }
                    id += 1;
                }
                9 => {
                    if !open.is_empty() {
                        let i = (g.next() as usize) % open.len();
                        let kind = if g.next() % 2 == 1 { 0 } else if g.next() % 2 == 1 { 1 } else { 2 };
                        let st = if g.next() % 2 == 1 { 503 } else { 200 };
                        let e = match kind {
                            0 => ev_http(open[i], st),
                            1 => ev_proc(open[i]),
                            _ => ev_timer(open[i]),
                        };
                        x.ev(e);
                        open.swap_remove(i);
                    }
                }
                10 => {
                    if !open.is_empty() {
                        let i = (g.next() as usize) % open.len();
                        x.act(cancel(open[i]), now);
                        open.swap_remove(i);
                    }
                }
                11 => {
                    let n = (g.next() % 3000000) as i64;
                    x.act(sayact(Style::Usage, n, b"[tokens: 1 in, 2 out]"), now);
                }
                12 => busy = g.next() % 2 == 1,
                _ => {
                    if g.next() % 4 == 0 {
                        x.hide();
                        awaiting = false;
                    }
                }
            }
            check_screen(&x, mode, awaiting, &format!("round {round} step {step} op {what}"));
            if mode == Mode::Pipe && x.sc.vt_err {
                assert!(!x.fd2.contains(&0x1B)); // made safe, and no styling
            }
            assert!(x.sc.next_ms(now) <= 250);
        }
        assert!(escapes_ok(&x.out) || (mode == Mode::Pipe && !x.sc.vt_err));
    }
}
