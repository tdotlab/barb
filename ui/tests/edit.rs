// Tests for the prompt's line editor (ui/src/edit.rs).
//
// The editor only produces bytes for a terminal, so the test plays them on a small terminal
// emulator and looks at the screen: what each row shows and where the cursor is. It does this for
// both of the ways terminals treat a row filled to the last column (move to the next row at once,
// or wait for the next character), because the editor must keep track of the cursor on either.
// After the scripted cases, long random runs check the screen after every key.
mod common;

use barb_ui::edit::{self, Editor, Key, ABOVE, DECOR, MAX};
use common::{utf8, Term};

const TR: usize = 160;
const TC: usize = 40;

// ---- what the screen should show ----

struct Layout {
    rows: Vec<String>, // the prompt and the line, without trailing spaces
    cr: usize,         // where the cursor should be, from the prompt's row
    cc: usize,
}

fn trim(mut b: Vec<u8>) -> String {
    while b.last() == Some(&b' ') {
        b.pop();
    }
    String::from_utf8_lossy(&b).into_owned()
}

// Worked out independently of the editor: the prompt, then each character on the row it fits in,
// a row filled to the last column putting the cursor at the start of the next.
fn layout(prompt: &str, prompt_cols: usize, line: &[u8], cur: usize, width: usize) -> Layout {
    let mut rows = vec![prompt.as_bytes().to_vec()];
    let (mut r, mut c) = (0, prompt_cols);
    let (mut cr, mut cc) = (0, prompt_cols);
    let mut i = 0;
    while i < line.len() {
        let (cp, len) = utf8(&line[i..]);
        let w = edit::cols(cp);
        if w > 0 && c + w > width {
            rows.push(Vec::new());
            r += 1;
            c = 0;
        }
        rows[r].extend_from_slice(&line[i..i + len]);
        c += w;
        i += len;
        if i == cur {
            cr = r;
            cc = c;
        }
    }
    if cc >= width {
        cr += 1;
        cc = 0;
    }
    Layout { rows: rows.into_iter().map(trim).collect(), cr, cc }
}

// What a decoration row should show: escapes gone, control characters as spaces, cut to leave the
// last column free, without the spaces at its end.
fn decor_text(s: &[u8], width: usize) -> String {
    let mut b = Vec::new();
    let mut cols = 0;
    let mut i = 0;
    while i < s.len() {
        if s[i] == 0x1B {
            if i + 1 < s.len() && s[i + 1] == b'[' {
                i += 2;
                while i < s.len() && !(0x40..=0x7E).contains(&s[i]) {
                    i += 1;
                }
                i += 1;
            } else {
                i += 2;
            }
            continue;
        }
        let (cp, len) = utf8(&s[i..]);
        let ctl = cp < 0x20 || cp == 0x7F;
        let w = if ctl { 1 } else { edit::cols(cp) };
        if cols + w > width - 1 {
            break;
        }
        if ctl {
            b.push(b' ');
        } else {
            b.extend_from_slice(&s[i..i + len]);
        }
        cols += w;
        i += len;
    }
    trim(b)
}

// ---- driving the editor ----

struct Fx {
    t: Term,
    e: Editor,
    top: usize,          // the prompt's row on the terminal
    prompt_text: String, // the prompt as it shows, without its colours
    prompt_cols: usize,
}

impl Fx {
    fn start(width: usize, deferred: bool) -> Fx {
        let mut t = Term::new(TR, TC, width, deferred);
        t.feed(b"above\r\n");
        let mut e = Editor::new(width);
        e.show();
        t.feed(&e.out);
        Fx { t, e, top: 1, prompt_text: "> ".into(), prompt_cols: 2 }
    }

    fn apply(&mut self) {
        self.t.feed(&self.e.out);
    }

    fn key(&mut self, k: Key) {
        self.e.key(k);
        self.apply();
    }

    fn typ(&mut self, s: &str) {
        for ch in s.chars() {
            self.key(Key::Char(ch as u32));
        }
    }

    fn refresh(&mut self) {
        self.e.refresh();
        self.apply();
    }

    fn show(&mut self) {
        self.e.show();
        self.apply();
    }

    fn hide(&mut self) {
        self.e.hide();
        self.apply();
    }

    fn frame(&mut self, n: usize, below: bool) {
        self.e.frame(n, below);
        self.apply();
    }

    fn set_prompt(&mut self, s: &str, shows: &str, cols: usize) {
        self.e.set_prompt(s.as_bytes(), cols);
        self.prompt_text = shows.into();
        self.prompt_cols = cols;
    }

    fn line(&self) -> String {
        String::from_utf8_lossy(&self.e.line).into_owned()
    }

    fn check_line(&self, want: &str) {
        assert_eq!(self.line(), want);
    }

    fn layout(&self, cur: usize) -> Layout {
        layout(&self.prompt_text, self.prompt_cols, &self.e.line, cur, self.t.width)
    }

    fn fail(&self, what: &str, detail: &str) -> ! {
        panic!(
            "{what} ({detail}; width {}, {} wrap; line {:?}, cursor at byte {})",
            self.t.width,
            if self.t.deferred { "deferred" } else { "immediate" },
            self.line(),
            self.e.cur
        );
    }

    // The screen shows exactly the block: the decoration rows, the prompt and the line, the
    // footer, with the cursor in place; or, hidden, nothing.
    fn check_screen(&mut self, when: &str) {
        if self.t.bad > 0 {
            self.t.bad = 0;
            self.fail("the terminal was sent something wrong", when);
        }
        if self.t.row(0) != "above" {
            self.fail("the output above the prompt was touched", when);
        }
        if self.t.style {
            self.fail("the editor left a colour on", when);
        }
        let (t, e, top) = (&self.t, &self.e, self.top);
        if !e.shown {
            if (top..TR).any(|r| !t.row(r).is_empty()) {
                self.fail("a hidden prompt left something on the screen", when);
            }
            if t.r != top || t.c != 0 || t.pend {
                self.fail("hidden, the cursor is not where the prompt was", when);
            }
            return;
        }
        let na = e.nabove;
        for i in 0..na {
            let (got, want) = (t.row(top + i), decor_text(&e.above[i], t.width));
            if got != want {
                self.fail(
                    "the screen does not show the decoration",
                    &format!("{when}: decoration row {i} is {got:?}, should be {want:?}"),
                );
            }
        }
        let end_row = self.layout(e.line.len()).cr; // the line's end, on the row after a full one
        let l = self.layout(e.cur);
        let line0 = top + na;
        for (i, want) in l.rows.iter().enumerate() {
            // the line is plain text: a colour from the decoration or the prompt must not reach it
            let from = if i == 0 { self.prompt_cols.min(t.width) } else { 0 };
            if t.styled[line0 + i][from..].iter().any(|&s| s) {
                self.fail("the line is drawn in a colour", &format!("{when}: row {i}"));
            }
            let got = t.row(line0 + i);
            if &got != want {
                self.fail(
                    "the screen does not show the line",
                    &format!("{when}: row {i} is {got:?}, should be {want:?}"),
                );
            }
        }
        let foot = if e.has_below { Some(line0 + end_row + 1) } else { None };
        for r in line0 + l.rows.len()..TR {
            let got = t.row(r);
            if Some(r) == foot {
                let want = decor_text(&e.below, t.width);
                if got != want {
                    self.fail(
                        "the screen does not show the footer",
                        &format!("{when}: the footer is {got:?}, should be {want:?}"),
                    );
                }
                continue;
            }
            if !got.is_empty() {
                self.fail("something is left below the line", when);
            }
        }
        if t.pend || t.r != line0 + l.cr || t.c != l.cc {
            self.fail(
                "the cursor is in the wrong place",
                &format!(
                    "{when}: at row {} col {}{}, should be row {} col {}",
                    t.r as i64 - line0 as i64,
                    t.c,
                    if t.pend { " waiting to wrap" } else { "" },
                    l.cr,
                    l.cc
                ),
            );
        }
        if e.row != na + l.cr {
            self.fail("the editor lost track of the cursor's row", when);
        }
    }

    // Enter: the line stays on the screen and the cursor goes to the start of the row after it.
    fn enter(&mut self) -> String {
        let was_shown = self.e.shown;
        // With the cursor at the end it is on the line's last row, or (column 0) already on the
        // row after a full one; either way the next prompt goes on the row after the line.
        let l = self.layout(self.e.line.len());
        let rows = if l.cc > 0 { l.cr + 1 } else { l.cr };
        let s = self.e.take();
        self.apply();
        if was_shown {
            self.top += rows;
            assert!(self.t.r == self.top && self.t.c == 0 && !self.t.pend, "after Enter");
        }
        self.check_screen("after Enter");
        String::from_utf8(s).unwrap()
    }
}

// ---- the cases ----

fn typing_and_wrapping(deferred: bool) {
    let mut f = Fx::start(10, deferred);
    f.check_screen("a new prompt");
    f.typ("hello");
    f.check_line("hello");
    f.check_screen("typing");
    assert_eq!(f.t.c, 7);
    let mut f = Fx::start(10, deferred);
    f.typ("abcdefgh"); // with the prompt, exactly the width
    f.check_screen("a full row");
    assert_eq!(f.e.row, 1);
    assert!(f.t.r == f.top + 1 && f.t.c == 0);
    f.typ("i");
    f.check_screen("the next row");
    f.key(Key::Back);
    f.check_screen("back to a full row");
    f.key(Key::Home);
    f.check_screen("home from below");
    assert!(f.t.r == f.top && f.t.c == 2);
    f.key(Key::End);
    f.check_screen("end of a full row");
    f.typ("ijklmnopqrstuvwxyz");
    f.check_screen("three rows");
    for _ in 0..12 {
        f.key(Key::Left);
    }
    f.check_screen("left across rows");
    f.typ("_");
    f.check_screen("inserting in the middle of wrapped text");
    f.check_line("abcdefghijklmn_opqrstuvwxyz");
}

#[test]
fn typing() {
    typing_and_wrapping(false);
    typing_and_wrapping(true);
}

#[test]
fn moving_and_killing() {
    let mut f = Fx::start(30, true);
    f.typ("helo");
    f.key(Key::Left);
    f.typ("l");
    f.check_line("hello");
    assert_eq!(f.e.cur, 4);
    f.check_screen("insert");
    f.key(Key::Del);
    f.check_line("hell");
    f.key(Key::Del); // at the end: nothing
    assert!(f.e.out.is_empty());
    f.key(Key::Right); // nowhere to go: nothing drawn
    assert!(f.e.out.is_empty());

    let mut f = Fx::start(30, true);
    f.typ("one two  three");
    let steps = [
        (Key::WordLeft, 9),
        (Key::WordLeft, 4),
        (Key::WordLeft, 0),
    ];
    for (k, at) in steps {
        f.key(k);
        assert_eq!(f.e.cur, at);
    }
    f.key(Key::WordLeft);
    assert!(f.e.out.is_empty());
    for at in [3, 7, 14] {
        f.key(Key::WordRight);
        assert_eq!(f.e.cur, at);
    }
    f.check_screen("word moves");

    f.key(Key::KillWord);
    f.check_line("one two  ");
    f.key(Key::KillWord);
    f.check_line("one ");
    f.check_screen("kill word");
    f.typ("two three");
    f.key(Key::Home);
    f.key(Key::Right);
    f.key(Key::KillEnd);
    f.check_line("o");
    assert_eq!(f.e.cur, 1);
    f.key(Key::KillEnd);
    assert!(f.e.out.is_empty());
    f.typ("ne two");
    f.key(Key::WordLeft);
    f.key(Key::KillStart);
    f.check_line("two");
    assert_eq!(f.e.cur, 0);
    f.key(Key::KillStart);
    assert!(f.e.out.is_empty());
    f.check_screen("kill to the start");
    f.key(Key::Clear);
    f.check_line("");
    f.check_screen("clear");
    f.key(Key::Clear);
    assert!(f.e.out.is_empty());
    f.key(Key::Back);
    assert!(f.e.out.is_empty());
}

fn unicode_cases(deferred: bool) {
    let mut f = Fx::start(12, deferred);
    f.typ("aé");
    assert_eq!(f.e.line.len(), 3);
    f.key(Key::Back);
    f.check_line("a"); // the whole character, not half of it
    f.typ("漢");
    assert_eq!(f.t.c, 5); // "> a" and two columns
    f.check_screen("a wide character");
    f.typ("😀");
    assert_eq!(f.e.line.len(), 8);
    f.key(Key::Left);
    assert_eq!(f.e.cur, 4);
    f.key(Key::Back);
    f.check_line("a😀");
    f.key(Key::End);
    f.key(Key::Back);
    f.check_line("a");
    f.typ("e\u{301}"); // e and a combining acute: one column
    assert_eq!(f.t.c, 4);
    f.check_screen("a combining mark");
    f.key(Key::Back);
    f.check_line("ae"); // backspace takes the mark, then the letter

    let mut f = Fx::start(10, deferred);
    f.typ("漢漢漢漢"); // 2 + 8: exactly full
    f.check_screen("wide characters filling a row");
    assert!(f.t.r == f.top + 1 && f.t.c == 0);
    f.typ("\u{301}"); // a mark on the last of them, which is on the row above
    f.check_screen("a mark after a full row");
    f.typ("漢");
    f.check_screen("a wide character on the next row");
    let mut f = Fx::start(11, deferred);
    f.typ("漢漢漢漢漢"); // 2 + 8 leaves one column, too narrow for the fifth
    f.check_screen("a wide character that does not fit");
    assert_eq!(f.e.row, 1);

    let mut f = Fx::start(20, deferred);
    for c in [1, 0x7F, 0xD800, 0x110000] {
        f.key(Key::Char(c));
    }
    f.check_line("");
    assert!(f.e.out.is_empty());
    f.key(Key::Char('\t' as u32));
    f.check_line(" ");
}

#[test]
fn unicode() {
    unicode_cases(false);
    unicode_cases(true);
    for (c, w) in [('a' as u32, 1), (0, 0), (0x301, 0), (0x200B, 0), (0x4E00, 2), (0xAC00, 2), (0x1F600, 2), (0xE9, 1)] {
        assert_eq!(edit::cols(c), w, "U+{c:04X}");
    }
}

fn hidden_cases(deferred: bool) {
    let mut f = Fx::start(10, deferred);
    f.typ("old"); // something in the history to move through
    f.enter();
    f.show();
    f.typ("abcdefghijk");
    f.hide();
    f.check_screen("hidden");
    f.e.hide();
    assert!(f.e.out.is_empty());
    f.typ("lm"); // while output is going by: no drawing
    assert!(f.e.out.is_empty());
    f.key(Key::Up);
    f.check_line("old");
    assert!(f.e.out.is_empty());
    f.key(Key::Down);
    f.check_line("abcdefghijklm");
    f.key(Key::Home);
    assert!(f.e.out.is_empty());
    f.check_screen("edited while hidden");
    f.show();
    f.check_line("abcdefghijklm");
    f.check_screen("shown again");
    assert_eq!(f.e.cur, 0);
    f.e.show();
    assert!(f.e.out.is_empty());
}

#[test]
fn hidden() {
    hidden_cases(false);
    hidden_cases(true);
}

fn history_cases(deferred: bool) {
    let mut f = Fx::start(10, deferred);
    assert_eq!(f.enter(), "");
    f.show();
    f.typ("one");
    assert_eq!(f.enter(), "one");
    f.check_line("");
    assert_eq!(f.e.cur, 0);
    f.show();
    f.typ("abcdefgh"); // exactly full: no blank row after it
    f.key(Key::Home);
    f.enter();
    f.show();
    f.typ("two");
    f.enter();
    f.show();
    f.typ("two"); // the same again is not kept twice
    f.enter();
    f.show();
    f.check_screen("the prompt after a few lines");
    f.typ("dr");
    f.key(Key::Up);
    f.check_line("two");
    f.check_screen("history");
    f.key(Key::Up);
    f.check_line("abcdefgh");
    f.key(Key::Up);
    f.check_line("one");
    f.key(Key::Up);
    f.check_line("one"); // the oldest; the empty line was not kept
    assert!(f.e.out.is_empty());
    f.key(Key::Down);
    f.check_line("abcdefgh");
    f.check_screen("history down");
    f.key(Key::Down);
    f.check_line("two");
    f.key(Key::Down);
    f.check_line("dr"); // the draft comes back
    f.key(Key::Down);
    assert!(f.e.out.is_empty());
    f.key(Key::Up);
    f.typ("!"); // editing a history line, then Enter, keeps both
    assert_eq!(f.enter(), "two!");
    f.show();
    f.key(Key::Up);
    f.check_line("two!");
    f.key(Key::Up);
    f.check_line("two");
    f.key(Key::Clear); // Escape leaves the history: a fresh, empty line
    f.key(Key::Down);
    f.check_line("");
    f.key(Key::Up);
    f.check_line("two!");
}

#[test]
fn enter_and_history() {
    history_cases(false);
    history_cases(true);

    // When the history is full, the oldest whole lines go. A short line first and then nine long
    // ones: 101 + 8 x 8,001 bytes fit in 65,536; the tenth pushes out the short one and then the
    // first long one, so no line is ever cut in two.
    let mut e = Editor::new(80);
    for i in 0..10u8 {
        let len = if i > 0 { 8000 } else { 100 };
        for _ in 0..len {
            e.key(Key::Char((b'a' + i) as u32));
        }
        assert_eq!(e.take().len(), len);
    }
    let mut ups = 0;
    loop {
        let before = e.line.first().copied();
        e.key(Key::Up);
        if e.line.first().copied() == before && ups > 0 {
            break;
        }
        ups += 1;
        if ups > 20 {
            break;
        }
    }
    assert_eq!(ups, 8); // eight lines are left
    assert_eq!(e.line[0], b'c'); // the oldest of them is the third typed
    assert_eq!(e.line.len(), 8000);
}

#[test]
fn limits() {
    let mut e = Editor::new(5);
    assert_eq!(e.width, 10);
    e.set_width(300);
    assert_eq!(e.width, 300);
    for _ in 0..MAX + 50 {
        e.key(Key::Char('x' as u32));
    }
    assert_eq!(e.line.len(), MAX - 1);
    e.key(Key::Char(0xE9));
    assert_eq!(e.line.len(), MAX - 1);
    e.key(Key::Back);
    e.key(Key::Back);
    e.key(Key::Char(0x4E00)); // three bytes, two free: refused whole, not cut in half
    assert_eq!(e.line.len(), MAX - 3);
    e.key(Key::Char(0xE9)); // two bytes fit
    assert_eq!(e.line.len(), MAX - 1);
    assert!(core::str::from_utf8(&e.line).is_ok());
    e.show(); // a whole line's drawing
    assert!(e.out.len() > MAX);
}

const RULE: &str = "\x1b[2m──────────────────────\x1b[0m";

fn decoration_cases(deferred: bool) {
    let mut f = Fx::start(20, deferred);
    f.frame(2, true);
    f.check_screen("an empty frame");
    f.e.set_above(0, "\x1b[2m⠋ waiting · 3s\x1b[0m".as_bytes());
    f.e.set_above(1, RULE.as_bytes());
    f.e.set_below("\x1b[2mmodel · ~/x\x1b[0m".as_bytes());
    f.refresh();
    f.check_screen("decorated");
    assert_eq!(f.e.above_cols[1], 19); // a rule wider than the row is cut to leave the last column
    f.refresh();
    assert!(f.e.out.is_empty()); // nothing changed, nothing drawn
    let same = f.e.above[0].clone();
    f.e.set_above(0, &same);
    f.refresh();
    assert!(f.e.out.is_empty()); // the same text again changes nothing
    f.typ("hello");
    f.check_screen("typing above a footer");
    f.e.set_above(0, "\x1b[2m⠙ waiting · 4s\x1b[0m".as_bytes());
    f.refresh();
    f.check_screen("the activity row changed");
    assert!(!f.e.out.contains(&b'J')); // rewritten in place: the line was not erased
    f.key(Key::Home);
    f.e.set_above(1, b"short"); // shorter than before: the rest of the row is cleared
    f.refresh();
    f.check_screen("a shorter decoration row, cursor at home");
    f.key(Key::End);
    f.typ("abcdefghijklm"); // 2 + 18: exactly full, the footer two rows down
    f.check_screen("a full row above a footer");
    assert_eq!(f.e.row, 3);
    f.typ("n");
    f.check_screen("onto the row after a full one");
    f.key(Key::Back);
    f.key(Key::Back);
    f.check_screen("back off the full row");
    f.e.set_below("a\tb\x01c 漢漢漢漢漢漢漢漢".as_bytes());
    f.refresh();
    f.check_screen("a footer with control and wide characters, too long");
    f.e.set_below(b"");
    f.refresh();
    f.check_screen("an empty footer");

    f.set_prompt("\x1b[1;35m>\x1b[0m ", "> ", 2);
    f.refresh();
    f.check_screen("a coloured prompt");
    f.set_prompt("barb> ", "barb> ", 6);
    f.refresh();
    f.check_screen("a wider prompt");
    f.typ("xyz");
    f.check_screen("typing after a wider prompt");
    f.e.set_prompt(b"> ", 21);
    assert_eq!(f.e.prompt_cols, 2);
    f.e.set_prompt(b"> ", usize::MAX);
    assert_eq!(f.e.prompt_cols, 2);
    f.set_prompt("> ", "> ", 2);
    f.refresh();
    f.check_screen("the prompt back");

    f.e.set_above(ABOVE, b"x"); // out of range: ignored
    f.e.set_above(usize::MAX, b"x");
    f.refresh();
    assert!(f.e.out.is_empty());

    f.hide();
    f.check_screen("decorated, hidden");
    f.e.set_above(0, b"while hidden");
    f.refresh();
    assert!(f.e.out.is_empty()); // nothing drawn while hidden
    f.show();
    f.check_screen("shown with what changed while hidden");

    f.frame(1, false);
    f.check_screen("one row above, no footer");
    f.e.frame(1, false);
    assert!(f.e.out.is_empty());
    f.frame(9, true);
    assert_eq!(f.e.nabove, ABOVE);
    assert!(f.e.has_below);
    f.check_screen("the frame clamped");
    f.frame(0, false);
    assert_eq!(f.e.nabove, 0);
    f.check_screen("no frame");
    f.frame(2, true);
    f.check_screen("the frame again");

    // Enter: the decoration goes and the prompt and line stay, as a record of what was sent.
    f.key(Key::Home);
    assert_eq!(f.enter(), "helloabcdefghijklxyz");
    assert_eq!(f.t.row(1), "> helloabcdefghijklx");
    assert_eq!(f.t.row(2), "yz");
    f.show();
    f.check_screen("a decorated prompt after Enter");
    f.typ("abcdefghijklmnopqr"); // full at Enter, decoration and all
    f.enter();
    f.show();
    f.check_screen("after Enter on a full row");

    // A resize erases the block as the terminal has reflowed it: a decoration row wider than the
    // new width takes two rows.
    let mut f = Fx::start(40, deferred);
    f.frame(2, true);
    f.e.set_above(0, b"012345678901234567890123456789");
    f.e.set_above(1, b"short");
    f.refresh();
    f.typ("hello");
    assert_eq!(f.e.row, 2);
    f.e.resize(40); // the same width: erased from where it is
    assert_eq!(f.e.out, b"\x1b[2A\r\x1b[J");
    f.apply();
    f.check_screen("erased for a resize");
    f.show();
    f.e.resize(20);
    assert_eq!(f.e.out, b"\x1b[3A\r\x1b[J");
    assert!(!f.e.shown);
    assert_eq!(f.e.width, 20);
    f.e.resize(3);
    assert_eq!(f.e.width, 10);

    // Text too long for its row is cut between whole characters.
    let mut big: Vec<u8> = "é".repeat(DECOR).into_bytes();
    big.truncate(DECOR * 2 - 1);
    f.e.set_below(&big);
    assert_eq!(f.e.below.len(), DECOR - 2);
    assert_eq!(f.e.below[DECOR - 3], 0xA9);
    f.e.set_prompt(&big, 2);
    assert_eq!(f.e.prompt.len() % 2, 0);
    f.e.set_below(b"");
    assert!(f.e.below.is_empty());

    assert_eq!(edit::text_cols("\x1b[1mab\x1b[0m漢".as_bytes()), 4);
    assert_eq!(edit::text_cols(b"\x1b7x"), 1);
    assert_eq!(edit::text_cols(b"\x1b[1"), 0);
}

#[test]
fn decoration() {
    decoration_cases(false);
    decoration_cases(true);
}

// ---- random runs ----

struct Rng(u32);
impl Rng {
    fn next(&mut self, n: u32) -> u32 {
        self.0 = self.0.wrapping_mul(1103515245).wrapping_add(12345);
        (self.0 >> 16) % n
    }
}

fn valid(e: &Editor) -> bool {
    match core::str::from_utf8(&e.line) {
        Ok(s) => e.cur <= s.len() && s.is_char_boundary(e.cur),
        Err(_) => false,
    }
}

// With `deco`, the decoration changes now and then too: its rows, the footer, the prompt and how
// many rows there are.
fn random_run(rng: &mut Rng, deferred: bool, width: usize, deco: bool) {
    const CHARS: [u32; 10] = ['a' as u32, 'b' as u32, 'x' as u32, ' ' as u32, ' ' as u32, 0xE9, 0x4E00, 0x1F600, 0x301, '-' as u32];
    const TEXTS: [&str; 6] = [
        "",
        "x",
        "\x1b[2m⠋ waiting · 12s\x1b[0m",
        "a much longer line than any terminal here",
        "漢漢漢漢漢漢漢",
        "\x1b[1;32m●\x1b[0m tab\there",
    ];
    const PROMPTS: [(&str, &str, usize); 3] = [("> ", "> ", 2), ("\x1b[1;35m>\x1b[0m ", "> ", 2), ("barb> ", "barb> ", 6)];
    let mut f = Fx::start(width, deferred);
    if deco {
        f.frame(2, true);
    }
    for step in 0..4000 {
        let r = rng.next(100);
        let what;
        if deco && rng.next(8) == 0 {
            let d = rng.next(10);
            let t = TEXTS[rng.next(TEXTS.len() as u32) as usize].as_bytes();
            if d < 4 {
                what = "a decoration row";
                let i = rng.next(ABOVE as u32) as usize;
                f.e.set_above(i, t);
            } else if d < 7 {
                what = "the footer";
                f.e.set_below(t);
            } else if d < 8 {
                what = "the prompt";
                let p = PROMPTS[rng.next(PROMPTS.len() as u32) as usize];
                f.set_prompt(p.0, p.1, p.2);
            } else {
                what = "the frame";
                let n = rng.next(ABOVE as u32 + 1) as usize;
                let below = rng.next(2) == 1;
                f.frame(n, below);
            }
            f.refresh();
        } else {
            let k = match r {
                0..=44 => {
                    if f.e.line.len() < 90 {
                        f.key(Key::Char(CHARS[rng.next(CHARS.len() as u32) as usize]));
                    }
                    "typing"
                }
                45..=51 => { f.key(Key::Back); "backspace" }
                52..=54 => { f.key(Key::Del); "delete" }
                55..=62 => { f.key(Key::Left); "left" }
                63..=68 => { f.key(Key::Right); "right" }
                69..=70 => { f.key(Key::Home); "home" }
                71..=72 => { f.key(Key::End); "end" }
                73..=74 => { f.key(Key::WordLeft); "word left" }
                75..=76 => { f.key(Key::WordRight); "word right" }
                77..=79 => { f.key(Key::Up); "up" }
                80..=81 => { f.key(Key::Down); "down" }
                82 => { f.key(Key::KillStart); "kill to start" }
                83 => { f.key(Key::KillEnd); "kill to end" }
                84..=85 => { f.key(Key::KillWord); "kill word" }
                86 => { f.key(Key::Clear); "clear" }
                87..=90 => { f.hide(); "hide" }
                91..=94 => { f.show(); "show" }
                _ => {
                    f.enter();
                    if f.top > TR - 30 {
                        f.t = Term::new(TR, TC, width, deferred);
                        f.t.feed(b"above\r\n");
                        f.top = 1;
                        f.e.shown = false;
                    }
                    f.show();
                    "enter"
                }
            };
            what = k;
        }
        assert!(valid(&f.e), "the line is not valid UTF-8 after {what} (random run, step {step})");
        f.check_screen(&format!("{what} (random run, step {step})"));
    }
}

#[test]
fn random() {
    let mut rng = Rng(12345);
    for deferred in [false, true] {
        for w in 10..=14 {
            random_run(&mut rng, deferred, w, false);
        }
        for w in 10..=14 {
            random_run(&mut rng, deferred, w, true);
        }
        random_run(&mut rng, deferred, 23, true);
    }
}
