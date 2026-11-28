//! Markdown for the terminal: the model's replies, drawn with bold, italics, code, lists, quotes
//! and tables instead of the asterisks and pipes they were written with.
//!
//! Like the editor it does no I/O: what to show collects in [`Md::out`]. It works a line at a time,
//! the way models write (a paragraph is one line), and wraps each line at the width with a hanging
//! indent for list items and quotes. A fenced code block is shown as it is, fences and all, so it
//! can be copied. Every control character in the text is made visible (ESC shows as "^["), so a
//! reply cannot move the cursor or set the title.

use alloc::vec;
use alloc::vec::Vec;

use crate::edit;

/// Bytes of one line, once styled; a longer line is shown plain.
pub const LINE: usize = 8192;
/// Bytes of a table's cells.
pub const TABLE: usize = 16384;
/// A table's rows, the header included.
pub const ROWS: usize = 64;
/// And its columns.
pub const COLS: usize = 12;

const BOLD: u8 = 1;
const DIM: u8 = 2;
const ITAL: u8 = 4;
const ULINE: u8 = 8;
const STRIKE: u8 = 16;
const CODE: u8 = 32;

#[derive(Clone, Copy, Default)]
struct Cell {
    off: usize,
    len: usize,
    cols: usize,
}

pub struct Md {
    /// The terminal's columns.
    pub width: usize,
    /// No box-drawing characters or bullets (the Linux console).
    pub ascii: bool,
    /// What to write to the terminal; the caller takes it.
    pub out: Vec<u8>,
    // Scratch: the line being drawn, a byte of style for each of its bytes, and a table's cells.
    b: Vec<u8>,
    st: Vec<u8>,
    over: bool,
    tb: Vec<u8>,
    tst: Vec<u8>,
    cell: Vec<[Cell; COLS]>,
    cur: u8, // the style the terminal is in
}

/// Write `s` with its control characters made visible: C0 as "^X", DEL as "^?", C1 as U+FFFD.
/// Newlines and tabs pass, and a CR before a newline is dropped.
pub fn safe(s: &[u8], out: &mut Vec<u8>) {
    let n = s.len();
    let mut i = 0;
    while i < n {
        let c = s[i];
        if c == b'\r' && i + 1 < n && s[i + 1] == b'\n' {
            i += 1;
        } else if (c < 0x20 && c != b'\n' && c != b'\t') || c == 0x7F {
            out.push(b'^');
            out.push(if c == 0x7F { b'?' } else { c + 0x40 });
            i += 1;
        } else if c == 0xC2 && i + 1 < n && (0x80..=0x9F).contains(&s[i + 1]) {
            out.extend_from_slice("\u{FFFD}".as_bytes());
            i += 2;
        } else {
            out.push(c);
            i += 1;
        }
    }
}

// ---- UTF-8 and columns ----

/// The code point at the start of `s` and its length, read loosely (the text is the model's): a
/// byte that starts nothing is taken as itself.
fn dec(s: &[u8]) -> (u32, usize) {
    let n = s.len();
    let u = |k: usize| s[k] as u32;
    if u(0) < 0x80 || n < 2 {
        return (u(0), 1);
    }
    if u(0) & 0xE0 == 0xC0 {
        return (((u(0) & 0x1F) << 6) | (u(1) & 0x3F), 2);
    }
    if u(0) & 0xF0 == 0xE0 && n >= 3 {
        return (((u(0) & 0x0F) << 12) | ((u(1) & 0x3F) << 6) | (u(2) & 0x3F), 3);
    }
    if n >= 4 {
        return (((u(0) & 0x07) << 18) | ((u(1) & 0x3F) << 12) | ((u(2) & 0x3F) << 6) | (u(3) & 0x3F), 4);
    }
    (u(0), 1)
}

fn cols(s: &[u8]) -> usize {
    let mut c = 0;
    let mut i = 0;
    while i < s.len() {
        let (ch, len) = dec(&s[i..]);
        c += edit::cols(ch);
        i += len;
    }
    c
}

// ---- inline: code, emphasis, links ----

fn is_space(c: u8) -> bool {
    c == b' ' || c == b'\t'
}
fn is_alnum(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c >= 0x80
}
fn is_punct(c: u8) -> bool {
    c > 32 && c < 127 && !is_alnum(c)
}
fn special(c: u8) -> bool {
    matches!(c, b'\\' | b'`' | b'*' | b'_' | b'~' | b'[' | b'!' | b'<')
}

/// Is there a closing delimiter `d` after `from`, so that an opening one there means something?
fn closer_ahead(s: &[u8], from: usize, d: &[u8]) -> bool {
    let (n, dl) = (s.len(), d.len());
    let mut j = from + 1;
    while j + dl <= n {
        let at = j;
        j += 1;
        if &s[at..at + dl] != d || is_space(s[at - 1]) {
            continue;
        }
        if dl == 1 && s[at - 1] == d[0] {
            continue;
        }
        if d[0] == b'_' && at + dl < n && is_alnum(s[at + dl]) {
            continue;
        }
        return true;
    }
    false
}

fn run_of(s: &[u8], i: usize, c: u8) -> usize {
    s[i..].iter().take_while(|&&x| x == c).count()
}

// ---- blocks ----

fn indent_of(s: &[u8]) -> usize {
    s.iter().take_while(|&&c| c == b' ').count()
}

fn is_blank(s: &[u8]) -> bool {
    s.iter().all(|&c| is_space(c))
}

/// A fence: three or more backticks or tildes. Its length and character, or 0.
fn fence_len(s: &[u8]) -> (usize, u8) {
    if s.is_empty() || (s[0] != b'`' && s[0] != b'~') {
        return (0, 0);
    }
    let k = run_of(s, 0, s[0]);
    if k < 3 {
        return (0, 0);
    }
    if s[0] == b'`' && s[k..].contains(&b'`') {
        return (0, 0); // `` ``` x ``` `` is inline
    }
    (k, s[0])
}

fn is_rule(s: &[u8]) -> bool {
    let c = match s.first() {
        Some(&c @ (b'-' | b'*' | b'_')) => c,
        _ => return false,
    };
    let mut k = 0;
    for &x in s {
        if x == c {
            k += 1;
        } else if !is_space(x) {
            return false;
        }
    }
    k >= 3
}

/// The cells of a table row: offsets and lengths, trimmed, and whether there were more than `max`
/// (the first `max` are given then).
fn split_cells(s: &[u8], max: usize) -> (Vec<(usize, usize)>, bool) {
    let (mut a, mut b) = (0, s.len());
    while a < b && is_space(s[a]) {
        a += 1;
    }
    while b > a && is_space(s[b - 1]) {
        b -= 1;
    }
    if a < b && s[a] == b'|' {
        a += 1;
    }
    if b > a && s[b - 1] == b'|' && !(b - 1 > a && s[b - 2] == b'\\') {
        b -= 1;
    }
    let mut cells = Vec::new();
    let mut code = false;
    let mut start = a;
    let mut i = a;
    while i <= b {
        if i < b && s[i] == b'\\' {
            i += 2;
            continue;
        }
        if i < b && s[i] == b'`' {
            code = !code;
            i += 1;
            continue;
        }
        if i < b && (s[i] != b'|' || code) {
            i += 1;
            continue;
        }
        if cells.len() == max {
            return (cells, true);
        }
        let (mut x, mut y) = (start, i);
        while x < y && is_space(s[x]) {
            x += 1;
        }
        while y > x && is_space(s[y - 1]) {
            y -= 1;
        }
        cells.push((x, y - x));
        start = i + 1;
        i += 1;
    }
    (cells, false)
}

/// The row under a table's header: |---|:--:|--:|. Each column's alignment (0 left, 1 centre,
/// 2 right), or none if it is not one.
fn delim_row(s: &[u8]) -> Vec<u8> {
    if !s.contains(&b'-') {
        return Vec::new();
    }
    let (cells, too_many) = split_cells(s, COLS);
    if too_many {
        return Vec::new();
    }
    let mut align = Vec::new();
    for (off, l) in cells {
        let p = &s[off..off + l];
        let left = l > 0 && p[0] == b':';
        let right = l > 0 && p[l - 1] == b':';
        let end = l - right as usize;
        let mut i = left as usize;
        let mut dashes = 0;
        while i < end && p[i] == b'-' {
            i += 1;
            dashes += 1;
        }
        if dashes == 0 || i != end {
            return Vec::new();
        }
        align.push(if left && right { 1 } else if right { 2 } else { 0 });
    }
    align
}

fn line_end(s: &[u8], i: usize) -> usize {
    i + s[i..].iter().take_while(|&&c| c != b'\n').count()
}

fn trim_cr(s: &[u8], a: usize, e: usize) -> usize {
    if e > a && s[e - 1] == b'\r' { e - 1 } else { e }
}

impl Md {
    pub fn new(width: usize, ascii: bool) -> Md {
        Md {
            width,
            ascii,
            out: Vec::new(),
            b: Vec::with_capacity(LINE),
            st: Vec::with_capacity(LINE),
            over: false,
            tb: Vec::new(),
            tst: Vec::new(),
            cell: vec![[Cell::default(); COLS]; ROWS],
            cur: 0,
        }
    }

    // ---- writing ----

    fn w(&mut self, s: &[u8]) {
        self.out.extend_from_slice(s);
    }

    /// Put the terminal in style `s`: a reset, then each attribute it has.
    fn style(&mut self, s: u8) {
        if s == self.cur {
            return;
        }
        self.out.extend_from_slice(b"\x1b[0");
        for (bit, code) in [(BOLD, &b";1"[..]), (DIM, b";2"), (ITAL, b";3"), (ULINE, b";4"), (STRIKE, b";9"), (CODE, b";36")] {
            if s & bit != 0 {
                self.out.extend_from_slice(code);
            }
        }
        self.out.push(b'm');
        self.cur = s;
    }

    /// Bytes with their styles, a run of one style at a time.
    fn span(&mut self, b: &[u8], st: &[u8]) {
        let mut i = 0;
        while i < b.len() {
            let j = i + st[i..].iter().take_while(|&&x| x == st[i]).count();
            self.style(st[i]);
            self.w(&b[i..j]);
            i = j;
        }
    }

    /// A run of the line being built.
    fn span_line(&mut self, from: usize, to: usize) {
        let b = core::mem::take(&mut self.b);
        let st = core::mem::take(&mut self.st);
        self.span(&b[from..to], &st[from..to]);
        self.b = b;
        self.st = st;
    }

    fn end_line(&mut self) {
        self.style(0);
        self.w(b"\n");
    }

    fn spaces(&mut self, n: usize) {
        self.style(0);
        self.out.resize(self.out.len() + n, b' ');
    }

    // ---- the line being built ----

    fn add1(&mut self, c: u8, st: u8) {
        if self.b.len() >= LINE {
            self.over = true;
            return;
        }
        self.b.push(c);
        self.st.push(st);
    }

    /// Text into the line, its control characters made visible as `safe` does (a tab is a space).
    fn add(&mut self, s: &[u8], st: u8) {
        let mut i = 0;
        while i < s.len() {
            let c = s[i];
            if c == b'\t' {
                self.add1(b' ', st);
            } else if c < 0x20 || c == 0x7F {
                self.add1(b'^', st);
                self.add1(if c == 0x7F { b'?' } else { c + 0x40 }, st);
            } else if c == 0xC2 && i + 1 < s.len() && (0x80..=0x9F).contains(&s[i + 1]) {
                for &x in "\u{FFFD}".as_bytes() {
                    self.add1(x, st);
                }
                i += 1;
            } else {
                self.add1(c, st);
            }
            i += 1;
        }
    }

    // ---- inline ----

    fn inl(&mut self, s: &[u8], base: u8) {
        let n = s.len();
        let mut open = 0u8;
        let mut i = 0;
        while i < n {
            let c = s[i];
            let st = base | open;
            if c == b'\\' && i + 1 < n && is_punct(s[i + 1]) {
                self.add(&s[i + 1..i + 2], st);
                i += 2;
                continue;
            }
            if c == b'`' {
                let k = run_of(s, i, b'`');
                let mut j = i + k;
                let mut found = None;
                while j < n {
                    if s[j] != b'`' {
                        j += 1;
                        continue;
                    }
                    let r = run_of(s, j, b'`');
                    if r == k {
                        found = Some(j);
                        break;
                    }
                    j += r;
                }
                if let Some(f) = found {
                    let mut p = &s[i + k..f];
                    // one space of padding at each end goes, unless it is all spaces
                    if p.len() >= 2 && p[0] == b' ' && p[p.len() - 1] == b' ' && p.iter().any(|&x| x != b' ') {
                        p = &p[1..p.len() - 1];
                    }
                    self.add(p, st | CODE);
                    i = f + k;
                } else {
                    self.add(&s[i..i + k], st);
                    i += k;
                }
                continue;
            }
            if c == b'*' || c == b'_' || (c == b'~' && i + 1 < n && s[i + 1] == b'~') {
                let dl = if i + 1 < n && s[i + 1] == c { 2 } else { 1 };
                let bit = if c == b'~' { STRIKE } else if dl == 2 { BOLD } else { ITAL };
                let d = [c, c];
                if open & bit != 0
                    && i > 0
                    && !is_space(s[i - 1])
                    && (c != b'_' || i + dl >= n || !is_alnum(s[i + dl]))
                {
                    open &= !bit;
                    i += dl;
                    continue;
                }
                if open & bit == 0
                    && i + dl < n
                    && !is_space(s[i + dl])
                    && (c != b'_' || i == 0 || !is_alnum(s[i - 1]))
                    && closer_ahead(s, i + dl, &d[..dl])
                {
                    open |= bit;
                    i += dl;
                    continue;
                }
                self.add(&s[i..i + dl], st);
                i += dl;
                continue;
            }
            if c == b'[' || (c == b'!' && i + 1 < n && s[i + 1] == b'[') {
                let a = if c == b'!' { i + 2 } else { i + 1 };
                let mut close = a;
                let mut depth = 1;
                while close < n {
                    if s[close] == b'\\' {
                        close += 2;
                        continue;
                    }
                    if s[close] == b'[' {
                        depth += 1;
                    } else if s[close] == b']' {
                        depth -= 1;
                        if depth == 0 {
                            break;
                        }
                    }
                    close += 1;
                }
                if close + 1 < n && s[close] == b']' && s[close + 1] == b'(' {
                    let u = close + 2;
                    let mut e = u;
                    let mut pd = 1;
                    while e < n && !is_space(s[e]) {
                        if s[e] == b'(' {
                            pd += 1;
                        } else if s[e] == b')' {
                            pd -= 1;
                            if pd == 0 {
                                break;
                            }
                        }
                        e += 1;
                    }
                    if e < n && s[e] == b')' {
                        let (text, url) = (&s[a..close], &s[u..e]);
                        if c == b'!' {
                            if text.is_empty() {
                                self.add(b"image", st);
                            } else {
                                self.inl(text, st);
                            }
                        } else {
                            self.inl(text, st | ULINE);
                        }
                        if c == b'!' || text != url {
                            self.add(b" (", DIM);
                            self.add(url, DIM);
                            self.add(b")", DIM);
                        }
                        i = e + 1;
                        continue;
                    }
                }
            }
            if c == b'<' {
                let r = &s[i + 1..];
                if r.starts_with(b"http://") || r.starts_with(b"https://") || r.starts_with(b"mailto:") {
                    let mut e = i + 1;
                    while e < n && s[e] != b'>' && s[e] != b'<' && !is_space(s[e]) {
                        e += 1;
                    }
                    if e < n && s[e] == b'>' {
                        self.add(&s[i + 1..e], st | ULINE);
                        i = e + 1;
                        continue;
                    }
                }
            }
            let j = i + 1 + s[i + 1..].iter().take_while(|&&x| !special(x)).count();
            self.add(&s[i..j], st);
            i = j;
        }
    }

    // ---- wrapping ----

    fn wrap_width(&self) -> usize {
        self.width.saturating_sub(1).max(20)
    }

    /// Show the line built so far, wrapped: `p1` (it takes c1 columns) goes before the first row
    /// and `p2` (c2) before the rest. The prefixes may carry escapes.
    fn flow(&mut self, p1: &[u8], c1: usize, p2: &[u8], c2: usize) {
        let wid = self.wrap_width();
        let n = self.b.len();
        let mut i = 0;
        self.style(0);
        self.w(p1);
        let mut col = c1;
        let mut empty = true;
        while i < n && self.b[i] == b' ' {
            i += 1;
        }
        while i < n {
            let sp = i;
            while i < n && self.b[i] == b' ' {
                i += 1;
            }
            let mut nsp = i - sp;
            if i >= n {
                break; // spaces at the end of the line
            }
            let ws = i;
            while i < n && self.b[i] != b' ' {
                i += 1;
            }
            let wc = cols(&self.b[ws..i]);
            if !empty && col + nsp + wc > wid {
                self.end_line();
                self.w(p2);
                col = c2;
                nsp = 0;
                empty = true;
            }
            if nsp > 0 {
                self.span_line(sp, sp + nsp);
                col += nsp;
            }
            if col + wc <= wid {
                self.span_line(ws, i);
                col += wc;
            } else {
                // longer than a row: break it where it fills one
                let mut k = ws;
                while k < i {
                    let (ch, len) = dec(&self.b[k..i]);
                    let cw = edit::cols(ch);
                    if col + cw > wid && !empty {
                        self.end_line();
                        self.w(p2);
                        col = c2;
                    }
                    self.span_line(k, k + len);
                    col += cw;
                    empty = false;
                    k += len;
                }
            }
            empty = false;
        }
        self.end_line();
    }

    /// A line too long to style: as it is, made safe.
    fn plain_line(&mut self, s: &[u8]) {
        self.style(0);
        safe(s, &mut self.out);
        self.w(b"\n");
    }

    fn para(&mut self, s: &[u8], base: u8, p1: &[u8], c1: usize, p2: &[u8], c2: usize) {
        self.b.clear();
        self.st.clear();
        self.over = false;
        self.inl(s, base);
        if self.over {
            self.plain_line(s);
        } else {
            self.flow(p1, c1, p2, c2);
        }
    }

    fn rule(&mut self) {
        let k = self.wrap_width().min(80);
        self.style(DIM);
        for _ in 0..k {
            self.w(if self.ascii { b"-" } else { "\u{2500}".as_bytes() });
        }
        self.end_line();
    }

    // ---- tables ----

    /// Keep the line built so far as table cell (r, c). False if there is no room.
    fn keep_cell(&mut self, r: usize, c: usize) -> bool {
        if self.over || self.tb.len() + self.b.len() > TABLE {
            return false;
        }
        self.cell[r][c] = Cell { off: self.tb.len(), len: self.b.len(), cols: cols(&self.b) };
        self.tb.extend_from_slice(&self.b);
        self.tst.extend_from_slice(&self.st);
        true
    }

    fn cell_out(&mut self, r: usize, c: usize) {
        let x = self.cell[r][c];
        let tb = core::mem::take(&mut self.tb);
        let tst = core::mem::take(&mut self.tst);
        self.span(&tb[x.off..x.off + x.len], &tst[x.off..x.off + x.len]);
        self.tb = tb;
        self.tst = tst;
    }

    /// Cell (r, c) onto the end of the line being built (if it fits).
    fn cell_to_line(&mut self, r: usize, c: usize) -> bool {
        let x = self.cell[r][c];
        if self.b.len() + x.len > LINE {
            return false;
        }
        self.b.extend_from_slice(&self.tb[x.off..x.off + x.len]);
        self.st.extend_from_slice(&self.tst[x.off..x.off + x.len]);
        true
    }

    /// A table whose header line is s[i..e). Where the text after it starts (n + 1 at the end).
    fn table(&mut self, s: &[u8], i: usize, e: usize, align: &[u8]) -> usize {
        let n = s.len();
        let ncols = align.len();
        let mut rows: Vec<(usize, usize)> = vec![(i, trim_cr(s, i, e))];
        let mut fits = true;
        let mut next = line_end(s, e + 1); // the end of the last line taken: the delimiter row
        while next < n {
            let a = next + 1;
            let b = line_end(s, a);
            let t = trim_cr(s, a, b);
            if is_blank(&s[a..t]) || !s[a..t].contains(&b'|') {
                break;
            }
            if rows.len() == ROWS {
                fits = false;
                break;
            }
            rows.push((a, t));
            next = b;
        }
        let after = if next >= n { n + 1 } else { next + 1 };

        self.tb.clear();
        self.tst.clear();
        'rows: for (r, &(a, t)) in rows.iter().enumerate() {
            let p = &s[a..t];
            let (cells, _) = split_cells(p, COLS + 1);
            for c in 0..ncols {
                self.b.clear();
                self.st.clear();
                self.over = false;
                if let Some(&(off, len)) = cells.get(c) {
                    self.inl(&p[off..off + len], if r == 0 { BOLD } else { 0 });
                }
                if !self.keep_cell(r, c) {
                    fits = false;
                    break 'rows;
                }
            }
        }
        if !fits {
            // as plain lines
            let mut a = i;
            loop {
                let b = line_end(s, a);
                self.para(&s[a..trim_cr(s, a, b)], 0, b"", 0, b"", 0);
                if b >= n || b + 1 >= after {
                    break;
                }
                a = b + 1;
            }
            return after;
        }

        let nrows = rows.len();
        let wid = self.wrap_width();
        let mut width = [0usize; COLS];
        let mut total = 0;
        for c in 0..ncols {
            width[c] = (0..nrows).map(|r| self.cell[r][c].cols).max().unwrap_or(0);
            total += width[c] + if c > 0 { 3 } else { 0 };
        }
        let bar: &[u8] = if self.ascii { b" | " } else { " \u{2502} ".as_bytes() };
        if total <= wid {
            for r in 0..nrows {
                for c in 0..ncols {
                    if c > 0 {
                        self.style(DIM);
                        self.w(bar);
                    }
                    let pad = width[c] - self.cell[r][c].cols;
                    let before = match align[c] {
                        2 => pad,
                        1 => pad / 2,
                        _ => 0,
                    };
                    self.spaces(before);
                    self.cell_out(r, c);
                    if c < ncols - 1 {
                        self.spaces(pad - before);
                    }
                }
                self.end_line();
                if r == 0 {
                    self.style(DIM);
                    let (h, x): (&[u8], &[u8]) =
                        if self.ascii { (b"-", b"-+-") } else { ("\u{2500}".as_bytes(), "\u{2500}\u{253C}\u{2500}".as_bytes()) };
                    for c in 0..ncols {
                        if c > 0 {
                            self.w(x);
                        }
                        for _ in 0..width[c] {
                            self.w(h);
                        }
                    }
                    self.end_line();
                }
            }
            return after;
        }
        // Too wide for the terminal: each row as a record, a field on each line.
        if nrows == 1 {
            // a header and nothing under it: one line
            self.b.clear();
            self.st.clear();
            for c in 0..ncols {
                if c > 0 {
                    self.add(bar, DIM);
                }
                if !self.cell_to_line(0, c) {
                    break;
                }
            }
            self.flow(b"", 0, b"", 0);
            return after;
        }
        for r in 1..nrows {
            if r > 1 {
                self.w(b"\n");
            }
            for c in 0..ncols {
                self.b.clear();
                self.st.clear();
                self.cell_to_line(0, c);
                self.add(b": ", 0);
                self.cell_to_line(r, c);
                self.flow(b"", 0, b"  ", 2);
            }
        }
        after
    }

    // ---- a reply ----

    /// Draw one reply: whole lines, each ended with a newline (so "" draws one empty line).
    pub fn render(&mut self, s: &[u8]) {
        let n = s.len();
        let mut fence = 0;
        let mut fch = 0;
        self.cur = 0;
        let mut i = 0;
        loop {
            let e = line_end(s, i);
            let t = trim_cr(s, i, e);
            let l = &s[i..t];
            let ind = indent_of(l);
            let p = &l[ind..];
            let pn = p.len();
            if fence > 0 {
                // inside a code block: as it is
                let (fl, ch) = if ind <= 3 { fence_len(p) } else { (0, 0) };
                if fl >= fence && ch == fch && is_blank(&p[fl..]) {
                    self.style(DIM);
                    safe(l, &mut self.out);
                    self.end_line();
                    fence = 0;
                } else {
                    self.plain_line(l);
                }
            } else if let (fl @ 1.., ch) = if ind <= 3 { fence_len(p) } else { (0, 0) } {
                self.style(DIM);
                safe(l, &mut self.out);
                self.end_line();
                fence = fl;
                fch = ch;
            } else if is_blank(l) {
                self.w(b"\n");
            } else if ind <= 3 && p[0] == b'#' && run_of(p, 0, b'#') <= 6 && (run_of(p, 0, b'#') == pn || is_space(p[run_of(p, 0, b'#')])) {
                let h = run_of(p, 0, b'#');
                let (mut a, mut b) = (h, pn);
                while a < b && is_space(p[a]) {
                    a += 1;
                }
                while b > a && is_space(p[b - 1]) {
                    b -= 1;
                }
                let mut c = b; // a closing run of #s
                while c > a && p[c - 1] == b'#' {
                    c -= 1;
                }
                if c < b && (c == a || is_space(p[c - 1])) {
                    b = c;
                    while b > a && is_space(p[b - 1]) {
                        b -= 1;
                    }
                }
                self.para(&p[a..b], if h == 1 { BOLD | ULINE } else { BOLD }, b"", 0, b"", 0);
            } else if ind <= 3 && is_rule(p) {
                self.rule();
            } else if ind <= 3 && p[0] == b'>' {
                let mut levels = 0;
                let mut a = 0;
                while a < pn && p[a] == b'>' {
                    levels += 1;
                    a += 1;
                    if a < pn && p[a] == b' ' {
                        a += 1;
                    }
                    while a < pn && p[a] == b' ' && a + 1 < pn && p[a + 1] == b'>' {
                        a += 1;
                    }
                }
                let levels = levels.min(8);
                let mut pre = Vec::from(&b"\x1b[2m"[..]);
                for _ in 0..levels {
                    pre.extend_from_slice(if self.ascii { b"| " } else { "\u{2502} ".as_bytes() });
                }
                pre.extend_from_slice(b"\x1b[0m");
                self.para(&p[a..], 0, &pre, levels * 2, &pre, levels * 2);
            } else {
                let align = if e < n && l.contains(&b'|') {
                    let ne = line_end(s, e + 1);
                    delim_row(&s[e + 1..trim_cr(s, e + 1, ne)])
                } else {
                    Vec::new()
                };
                if !align.is_empty() {
                    let (head, too_many) = split_cells(l, COLS + 1);
                    if !too_many && head.len() == align.len() {
                        i = self.table(s, i, e, &align);
                        if i > n {
                            break;
                        }
                        continue;
                    }
                }
                let dl = p.iter().take(9).take_while(|c| c.is_ascii_digit()).count();
                let ind = ind.min(16);
                let mut pre = vec![b' '; ind];
                if pn >= 2 && matches!(p[0], b'-' | b'*' | b'+') && is_space(p[1]) {
                    pre.extend_from_slice(if self.ascii { b"- " } else { "\u{2022} ".as_bytes() });
                    let rest = vec![b' '; ind + 2];
                    self.para(&p[2..], 0, &pre, ind + 2, &rest, ind + 2);
                } else if dl > 0 && dl + 1 < pn && (p[dl] == b'.' || p[dl] == b')') && is_space(p[dl + 1]) {
                    pre.extend_from_slice(&p[..dl + 1]);
                    pre.push(b' ');
                    let rc = ind + dl + 2;
                    let rest = vec![b' '; rc];
                    self.para(&p[dl + 2..], 0, &pre, rc, &rest, rc);
                } else {
                    self.para(p, 0, &pre.clone(), ind, &pre, ind);
                }
            }
            if e >= n {
                break;
            }
            i = e + 1;
        }
        self.style(0);
    }
}
