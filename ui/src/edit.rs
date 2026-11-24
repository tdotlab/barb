//! The line editor for barb's prompt: the line being typed, the cursor in it, the history, and
//! the terminal output that keeps the screen showing them.
//!
//! It does no I/O, so it is as portable as the core: the platform turns key presses into
//! [`Editor::key`] calls and writes [`Editor::out`] to the terminal after each call. Windows and
//! POSIX both use it (through the screen), with the terminal in raw mode.
//!
//! The screen model: the editor draws a block of rows at the bottom of what is on the screen.
//! From the top: `nabove` rows of decoration (the screen puts the activity line and a rule
//! there), the prompt and the line, which may wrap onto the rows below, and an optional footer
//! row. Decoration rows are cut to fit one row, so they never wrap. `row` is the row the
//! terminal's cursor is on, counted from the top of the block, so the editor can always get
//! back to the top to redraw or erase what it drew.

use alloc::collections::VecDeque;
use alloc::vec::Vec;

/// Bytes in a line, plus one (the C kept a NUL; the limits stay the same).
pub const MAX: usize = 8192;
/// Bytes of history, each line counted with one byte more; the oldest lines go first.
pub const HIST: usize = 65536;
/// Decoration rows above the prompt, at most.
pub const ABOVE: usize = 2;
/// Bytes in a decoration row, escapes included, plus one.
pub const DECOR: usize = 512;
/// Bytes in the prompt, escapes included, plus one.
pub const PROMPT: usize = 64;

/// A key, as the editor sees it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Key {
    /// Insert a Unicode code point.
    Char(u32),
    /// The character before the cursor.
    Back,
    /// The character under the cursor.
    Del,
    Left,
    Right,
    Home,
    End,
    WordLeft,
    WordRight,
    /// History.
    Up,
    Down,
    /// Ctrl-U: everything before the cursor.
    KillStart,
    /// Ctrl-K: everything from the cursor on.
    KillEnd,
    /// Ctrl-W: the word before the cursor.
    KillWord,
    /// Escape: the whole line.
    Clear,
}

// ---- UTF-8 ----

/// The code point at the start of `s` (which is not empty) and its length. Bytes that are not
/// UTF-8 come out as themselves, one at a time, as terminals show them.
pub fn decode(s: &[u8]) -> (u32, usize) {
    let n = s.len();
    let u0 = s[0] as u32;
    if u0 < 0x80 || n < 2 {
        return (u0, 1);
    }
    let c = |i: usize| (s[i] & 0x3F) as u32;
    if u0 & 0xE0 == 0xC0 {
        return (((u0 & 0x1F) << 6) | c(1), 2);
    }
    if u0 & 0xF0 == 0xE0 && n >= 3 {
        return (((u0 & 0x0F) << 12) | (c(1) << 6) | c(2), 3);
    }
    if n >= 4 {
        return (((u0 & 0x07) << 18) | (c(1) << 12) | (c(2) << 6) | c(3), 4);
    }
    (u0, 1)
}

/// `c` as UTF-8 in `b`; its length.
pub fn encode(c: u32, b: &mut [u8; 4]) -> usize {
    if c < 0x80 {
        b[0] = c as u8;
        1
    } else if c < 0x800 {
        b[0] = (0xC0 | (c >> 6)) as u8;
        b[1] = (0x80 | (c & 0x3F)) as u8;
        2
    } else if c < 0x10000 {
        b[0] = (0xE0 | (c >> 12)) as u8;
        b[1] = (0x80 | ((c >> 6) & 0x3F)) as u8;
        b[2] = (0x80 | (c & 0x3F)) as u8;
        3
    } else {
        b[0] = (0xF0 | (c >> 18)) as u8;
        b[1] = (0x80 | ((c >> 12) & 0x3F)) as u8;
        b[2] = (0x80 | ((c >> 6) & 0x3F)) as u8;
        b[3] = (0x80 | (c & 0x3F)) as u8;
        4
    }
}

fn is_cont(c: u8) -> bool {
    c & 0xC0 == 0x80
}

/// The columns a code point takes on a terminal: 0, 1 or 2. Combining marks, zero-width
/// characters and variation selectors take none; East Asian wide characters and most emoji take
/// two. Close enough to what terminals do to keep the cursor in the right place for the text
/// people type.
pub fn cols(c: u32) -> usize {
    if c == 0 {
        return 0;
    }
    let zero: &[(u32, u32)] = &[
        (0x0300, 0x036F), (0x0483, 0x0489), (0x0591, 0x05BD), (0x0610, 0x061A), (0x064B, 0x065F),
        (0x1AB0, 0x1AFF), (0x1DC0, 0x1DFF), (0x200B, 0x200F), (0x20D0, 0x20FF), (0xFE00, 0xFE0F),
        (0xFE20, 0xFE2F), (0xFEFF, 0xFEFF), (0xE0100, 0xE01EF),
    ];
    if zero.iter().any(|&(a, b)| c >= a && c <= b) {
        return 0;
    }
    let wide: &[(u32, u32)] = &[
        (0x1100, 0x115F), (0x2E80, 0x303E), (0x3041, 0x33FF), (0x3400, 0x4DBF), (0x4E00, 0x9FFF),
        (0xA000, 0xA4CF), (0xAC00, 0xD7A3), (0xF900, 0xFAFF), (0xFE30, 0xFE4F), (0xFF00, 0xFF60),
        (0xFFE0, 0xFFE6), (0x1F300, 0x1F64F), (0x1F900, 0x1F9FF), (0x20000, 0x3FFFD),
    ];
    if wide.iter().any(|&(a, b)| c >= a && c <= b) {
        return 2;
    }
    1
}

/// The length of the escape sequence at the start of `s` (ESC [ ... and its final byte, or ESC
/// and one byte), or 0.
pub fn esc_len(s: &[u8]) -> usize {
    if s.len() < 2 || s[0] != 0x1B {
        return 0;
    }
    if s[1] != b'[' {
        return 2;
    }
    let mut i = 2;
    while i < s.len() && !(0x40..=0x7E).contains(&s[i]) {
        i += 1;
    }
    if i < s.len() { i + 1 } else { s.len() }
}

/// The columns `s` takes on a terminal, escape sequences skipped (they take none).
pub fn text_cols(s: &[u8]) -> usize {
    let mut n = 0;
    let mut i = 0;
    while i < s.len() {
        let k = esc_len(&s[i..]);
        if k > 0 {
            i += k;
            continue;
        }
        let (c, len) = decode(&s[i..]);
        n += cols(c);
        i += len;
    }
    n
}

// ---- output ----

fn put_csi(out: &mut Vec<u8>, n: usize, fin: u8) {
    out.extend_from_slice(b"\x1b[");
    let mut d = [0u8; 20];
    let mut k = 0;
    let mut v = n;
    loop {
        d[k] = b'0' + (v % 10) as u8;
        k += 1;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    out.extend(d[..k].iter().rev());
    out.push(fin);
}

fn put_up(out: &mut Vec<u8>, n: usize) {
    if n > 0 {
        put_csi(out, n, b'A');
    }
}

fn put_down(out: &mut Vec<u8>, n: usize) {
    if n > 0 {
        put_csi(out, n, b'B');
    }
}

/// A decoration row, cut to leave the last column free (so it never wraps) and with its colours
/// reset after it. Control characters become spaces. The columns it took.
fn put_decor(out: &mut Vec<u8>, width: usize, s: &[u8]) -> usize {
    let mut n = 0;
    let mut styled = false;
    let mut i = 0;
    while i < s.len() {
        let k = esc_len(&s[i..]);
        if k > 0 {
            out.extend_from_slice(&s[i..i + k]);
            styled = true;
            i += k;
            continue;
        }
        let (c, len) = decode(&s[i..]);
        let ctl = c < 0x20 || c == 0x7F;
        let w = if ctl { 1 } else { cols(c) };
        if n + w > width - 1 {
            break;
        }
        if ctl {
            out.push(b' ');
        } else {
            out.extend_from_slice(&s[i..i + len]);
        }
        n += w;
        i += len;
    }
    if styled {
        out.extend_from_slice(b"\x1b[0m");
    }
    n
}

/// Copy `s` into `dst`, cut between whole characters to fewer than `cap` bytes. Whether it changed.
fn set_text(dst: &mut Vec<u8>, cap: usize, s: &[u8]) -> bool {
    let mut n = s.len();
    if n > cap - 1 {
        n = cap - 1;
        while n > 0 && is_cont(s[n]) {
            n -= 1;
        }
    }
    if dst[..] == s[..n] {
        return false;
    }
    dst.clear();
    dst.extend_from_slice(&s[..n]);
    true
}

/// The prompt's line editor.
pub struct Editor {
    /// The line, UTF-8.
    pub line: Vec<u8>,
    /// The cursor, in bytes.
    pub cur: usize,
    /// The terminal's columns.
    pub width: usize,
    /// The block is on the screen.
    pub shown: bool,
    /// The cursor's row below the block's first row.
    pub row: usize,
    hist: VecDeque<Vec<u8>>,
    hist_bytes: usize,
    /// How many lines back in the history the line is (0: the one being typed).
    back: usize,
    /// The line being typed, kept while the history is shown.
    draft: Vec<u8>,
    /// `> ` unless set; it may carry colour escapes.
    pub prompt: Vec<u8>,
    pub prompt_cols: usize,
    /// Decoration rows above the prompt (0 to ABOVE).
    pub nabove: usize,
    /// A footer row below the line.
    pub has_below: bool,
    pub above: [Vec<u8>; ABOVE],
    pub below: Vec<u8>,
    /// What changed since it was drawn: 1 the rows above, 2 the prompt or footer.
    dirty: u8,
    /// The columns each decoration row took when drawn.
    pub above_cols: [usize; ABOVE],
    /// What to write to the terminal after the call.
    pub out: Vec<u8>,
}

impl Editor {
    pub fn new(width: usize) -> Editor {
        let mut e = Editor {
            line: Vec::new(),
            cur: 0,
            width: 0,
            shown: false,
            row: 0,
            hist: VecDeque::new(),
            hist_bytes: 0,
            back: 0,
            draft: Vec::new(),
            prompt: b"> ".to_vec(),
            prompt_cols: 2,
            nabove: 0,
            has_below: false,
            above: [Vec::new(), Vec::new()],
            below: Vec::new(),
            dirty: 0,
            above_cols: [0; ABOVE],
            out: Vec::new(),
        };
        e.set_width(width);
        e
    }

    /// While hidden; see [`Editor::resize`].
    pub fn set_width(&mut self, width: usize) {
        self.width = width.max(10);
    }

    // ---- where things are ----

    /// Where the terminal's cursor is after the prompt and the first `upto` bytes of the line:
    /// the row below the prompt's, and the column, which is `width` when the row is full and the
    /// terminal is waiting to wrap with the next character.
    fn at(&self, upto: usize) -> (usize, usize) {
        let (mut r, mut c) = (0, self.prompt_cols);
        let mut i = 0;
        while i < upto {
            let (ch, len) = decode(&self.line[i..]);
            let w = cols(ch);
            if c + w > self.width {
                r += 1;
                c = 0;
            }
            c += w;
            i += len;
        }
        (r, c)
    }

    /// The cursor's place for byte `upto`, from the prompt's row: at the start of the next row
    /// when the one it is on is full.
    fn cursor_at(&self, upto: usize) -> (usize, usize) {
        let (r, c) = self.at(upto);
        if c >= self.width { (r + 1, 0) } else { (r, c) }
    }

    /// A row filled to the last column leaves the cursor there on some terminals, waiting to
    /// wrap, and on the next row on others. A space and a carriage return put it at the start of
    /// the next row on both, so the editor always knows where it is.
    fn settle(&mut self, row: usize, col: usize) -> usize {
        if col < self.width {
            return row;
        }
        self.out.extend_from_slice(b" \r");
        row + 1
    }

    fn prev_char(&self, mut i: usize) -> usize {
        if i > 0 {
            i -= 1;
        }
        while i > 0 && is_cont(self.line[i]) {
            i -= 1;
        }
        i
    }

    fn next_char(&self, mut i: usize) -> usize {
        let n = self.line.len();
        if i < n {
            i += 1;
        }
        while i < n && is_cont(self.line[i]) {
            i += 1;
        }
        i
    }

    // ---- drawing ----

    fn hide_block(&mut self) {
        if !self.shown {
            return;
        }
        put_up(&mut self.out, self.row);
        self.out.extend_from_slice(b"\r\x1b[J");
        self.shown = false;
        self.row = 0;
    }

    /// From the start of the prompt's row: the prompt, the line and the footer, then the cursor
    /// put in place.
    fn draw_line(&mut self) {
        self.out.extend_from_slice(&self.prompt);
        self.out.extend_from_slice(&self.line);
        let (er, ec) = self.at(self.line.len());
        let mut er = self.settle(er, ec);
        if self.has_below {
            // below the line, or below the empty row after a full one
            self.out.extend_from_slice(b"\r\n");
            er += 1;
            put_decor(&mut self.out, self.width, &self.below);
        }
        self.shown = true;
        self.dirty &= !2;
        let (cr, cc) = self.cursor_at(self.cur);
        self.row = self.nabove + cr;
        if self.cur == self.line.len() && !self.has_below {
            return; // at the end of the line: already there
        }
        put_up(&mut self.out, er - cr);
        self.out.push(b'\r');
        if cc > 0 {
            put_csi(&mut self.out, cc, b'C');
        }
    }

    /// Draw the whole block from the start of the cursor's row.
    fn draw(&mut self) {
        for i in 0..self.nabove {
            self.above_cols[i] = put_decor(&mut self.out, self.width, &self.above[i]);
            self.out.extend_from_slice(b"\r\n");
        }
        self.dirty &= !1;
        self.draw_line();
    }

    /// The prompt, the line and the footer again; the rows above stay as they are.
    fn redraw(&mut self) {
        if !self.shown {
            return;
        }
        put_up(&mut self.out, self.row - self.nabove);
        self.out.extend_from_slice(b"\r\x1b[J");
        self.draw_line();
    }

    /// Rewrite the rows above in place and come back to the cursor.
    fn redraw_above(&mut self) {
        self.dirty &= !1;
        if self.nabove == 0 {
            return;
        }
        put_up(&mut self.out, self.row);
        for i in 0..self.nabove {
            if i > 0 {
                put_down(&mut self.out, 1);
            }
            self.out.push(b'\r');
            self.above_cols[i] = put_decor(&mut self.out, self.width, &self.above[i]);
            self.out.extend_from_slice(b"\x1b[K");
        }
        put_down(&mut self.out, self.row - (self.nabove - 1));
        self.out.push(b'\r');
        let (_, cc) = self.cursor_at(self.cur);
        if cc > 0 {
            put_csi(&mut self.out, cc, b'C');
        }
    }

    /// Erase the block, leaving the cursor where its first row was.
    pub fn hide(&mut self) {
        self.out.clear();
        self.hide_block();
    }

    /// Draw the block if it is not on the screen.
    pub fn show(&mut self) {
        self.out.clear();
        if !self.shown {
            self.draw();
        }
    }

    /// Bring the screen up to date with the decoration, redrawing as little as it can (the rows
    /// above alone are rewritten in place).
    pub fn refresh(&mut self) {
        self.out.clear();
        if !self.shown {
            return;
        }
        if self.dirty & 1 != 0 {
            self.redraw_above();
        }
        if self.dirty & 2 != 0 {
            self.redraw();
        }
    }

    /// How many rows of decoration; redraws if shown.
    pub fn frame(&mut self, nabove: usize, below: bool) {
        self.out.clear();
        let nabove = nabove.min(ABOVE);
        if nabove == self.nabove && below == self.has_below {
            return;
        }
        let shown = self.shown;
        self.hide_block();
        self.nabove = nabove;
        self.has_below = below;
        if shown {
            self.draw();
        }
    }

    /// Setting a row only records it; [`Editor::refresh`] draws it. Out of range: ignored.
    pub fn set_above(&mut self, i: usize, s: &[u8]) {
        if i < ABOVE && set_text(&mut self.above[i], DECOR, s) {
            self.dirty |= 1;
        }
    }

    pub fn set_below(&mut self, s: &[u8]) {
        if set_text(&mut self.below, DECOR, s) {
            self.dirty |= 2;
        }
    }

    /// The prompt and the columns it shows as (2 when outside 0 to 20).
    pub fn set_prompt(&mut self, s: &[u8], cols: usize) {
        let cols = if cols > 20 { 2 } else { cols };
        if set_text(&mut self.prompt, PROMPT, s) {
            self.dirty |= 2;
        }
        if cols != self.prompt_cols {
            self.prompt_cols = cols;
            self.dirty |= 2;
        }
    }

    /// The terminal changed width. If the block is shown it is erased first, on the assumption
    /// that the terminal has reflowed it to the new width (most do), and left hidden.
    pub fn resize(&mut self, width: usize) {
        self.out.clear();
        let width = width.max(10);
        if self.shown && width != self.width {
            // Where the cursor is now, if the terminal rewrapped every row of the block to the
            // new width: a decoration row wider than it takes more rows, and the line wraps afresh.
            let mut up = 0;
            for i in 0..self.nabove {
                let c = self.above_cols[i];
                up += if c > width { c.div_ceil(width) } else { 1 };
            }
            self.width = width;
            let (cr, _) = self.cursor_at(self.cur);
            self.row = up + cr;
        }
        self.hide_block();
        self.width = width;
    }

    // ---- history ----

    fn hist_add(&mut self) {
        if self.line.is_empty() {
            return;
        }
        if self.hist.back() == Some(&self.line) {
            return; // the same as the one before
        }
        let need = self.line.len() + 1;
        if need > HIST {
            return;
        }
        while self.hist_bytes + need > HIST {
            let old = self.hist.pop_front().unwrap();
            self.hist_bytes -= old.len() + 1;
        }
        self.hist.push_back(self.line.clone());
        self.hist_bytes += need;
    }

    fn hist_move(&mut self, up: bool) {
        let to = if up {
            if self.back == self.hist.len() {
                return; // already at the oldest
            }
            self.back + 1
        } else {
            if self.back == 0 {
                return;
            }
            self.back - 1
        };
        if self.back == 0 {
            self.draft.clone_from(&self.line);
        }
        self.back = to;
        if to == 0 {
            self.line.clone_from(&self.draft);
        } else {
            self.line.clone_from(&self.hist[self.hist.len() - to]);
        }
        self.cur = self.line.len();
        self.redraw();
    }

    // ---- editing ----

    fn cut(&mut self, from: usize, to: usize) {
        self.line.drain(from..to);
        if self.cur > to {
            self.cur -= to - from;
        } else if self.cur > from {
            self.cur = from;
        }
    }

    fn is_space_at(&self, i: usize) -> bool {
        self.line[i] == b' ' || self.line[i] == b'\t'
    }

    fn word_left(&self) -> usize {
        let mut i = self.cur;
        while i > 0 && self.is_space_at(self.prev_char(i)) {
            i = self.prev_char(i);
        }
        while i > 0 && !self.is_space_at(self.prev_char(i)) {
            i = self.prev_char(i);
        }
        i
    }

    fn word_right(&self) -> usize {
        let mut i = self.cur;
        let n = self.line.len();
        while i < n && self.is_space_at(i) {
            i = self.next_char(i);
        }
        while i < n && !self.is_space_at(i) {
            i = self.next_char(i);
        }
        i
    }

    fn insert(&mut self, ch: u32) {
        let ch = if ch == b'\t' as u32 { b' ' as u32 } else { ch };
        if ch < 0x20 || ch == 0x7F || (0xD800..=0xDFFF).contains(&ch) || ch > 0x10FFFF {
            return;
        }
        let mut b = [0u8; 4];
        let k = encode(ch, &mut b);
        if self.line.len() + k > MAX - 1 {
            return;
        }
        // Typing at the end needs only the character, unless it is one that takes no column
        // (that belongs to the character before, which may be on the row above the cursor), or,
        // with a footer below, one that moves the end of the line onto another row.
        // (The end on a full row needs no check of its own: any character with a width wraps it
        // onto the next row.)
        let (r0, _) = self.at(self.line.len());
        let mut at_end = self.cur == self.line.len() && cols(ch) > 0;
        let cur = self.cur;
        self.line.splice(cur..cur, b[..k].iter().copied());
        self.cur += k;
        if !self.shown {
            return;
        }
        let (r, c) = self.at(self.line.len());
        if at_end && self.has_below && (r != r0 || c >= self.width) {
            at_end = false;
        }
        if at_end {
            self.out.extend_from_slice(&b[..k]);
            self.row = self.nabove + self.settle(r, c);
        } else {
            self.redraw();
        }
    }

    /// A key. Editing while the block is hidden changes it silently; [`Editor::show`] then draws
    /// it as it is.
    pub fn key(&mut self, key: Key) {
        self.out.clear();
        let was = self.cur;
        let n = self.line.len();
        match key {
            Key::Char(ch) => return self.insert(ch),
            Key::Back => {
                if self.cur == 0 {
                    return;
                }
                self.cut(self.prev_char(self.cur), self.cur);
            }
            Key::Del => {
                if self.cur == n {
                    return;
                }
                self.cut(self.cur, self.next_char(self.cur));
            }
            Key::Left => self.cur = self.prev_char(self.cur),
            Key::Right => self.cur = self.next_char(self.cur),
            Key::Home => self.cur = 0,
            Key::End => self.cur = n,
            Key::WordLeft => self.cur = self.word_left(),
            Key::WordRight => self.cur = self.word_right(),
            Key::Up => return self.hist_move(true),
            Key::Down => return self.hist_move(false),
            Key::KillStart => {
                if self.cur == 0 {
                    return;
                }
                self.cut(0, self.cur);
            }
            Key::KillEnd => {
                if self.cur == n {
                    return;
                }
                self.cut(self.cur, n);
            }
            Key::KillWord => {
                if self.cur == 0 {
                    return;
                }
                self.cut(self.word_left(), self.cur);
            }
            Key::Clear => {
                if n == 0 {
                    return;
                }
                self.line.clear();
                self.cur = 0;
                self.back = 0;
            }
        }
        let moves = matches!(
            key,
            Key::Left | Key::Right | Key::Home | Key::End | Key::WordLeft | Key::WordRight
        );
        if moves && self.cur == was {
            return; // nowhere to go
        }
        self.redraw();
    }

    /// Enter: the line. If shown, the block gives way to the prompt and the line alone, which
    /// stay on the screen as a record, and the cursor moves to the row after them. The line goes
    /// into the history, and the editor is empty and hidden.
    pub fn take(&mut self) -> Vec<u8> {
        self.out.clear();
        let n = self.line.len();
        if self.shown {
            if self.nabove > 0 || self.has_below {
                // The decoration goes; the prompt and the line are written again where the
                // block was.
                self.hide_block();
                let (nab, below) = (self.nabove, self.has_below);
                self.nabove = 0;
                self.has_below = false;
                self.cur = n;
                self.draw_line();
                self.nabove = nab;
                self.has_below = below;
            } else if self.cur != n {
                self.cur = n;
                self.redraw();
            }
            let (_, c) = self.at(n);
            if c < self.width {
                // a full last row has already put it on a new one
                self.out.extend_from_slice(b"\r\n");
            }
        }
        self.shown = false;
        self.row = 0;
        self.hist_add();
        self.cur = 0;
        self.back = 0;
        self.draft.clear();
        core::mem::take(&mut self.line)
    }
}
