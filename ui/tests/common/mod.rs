// A small terminal for the UI tests: it plays the bytes the editor and the screen write and keeps
// what each cell shows and where the cursor is. It knows the few sequences they use (cursor up,
// down and right, erase to the end of the line or the screen, colours) and counts anything else as
// bad, along with a cursor sent off the screen. Both ways terminals treat a row filled to the last
// column are there: moving to the next row at once, or waiting for the next character.
#![allow(dead_code)]

use barb_ui::edit;

const RIGHT_HALF: &[u8] = b"\x01"; // the right half of a wide character
const CELL: usize = 24;

pub struct Term {
    pub rows: usize,
    pub cols: usize,
    /// The UTF-8 in each cell: empty, or RIGHT_HALF.
    pub cell: Vec<Vec<Vec<u8>>>,
    pub r: usize,
    pub c: usize,
    /// Waiting to wrap with the next character (the deferred kind).
    pub pend: bool,
    pub width: usize,
    pub deferred: bool,
    /// The cell last written, where a combining mark goes.
    last: Option<(usize, usize)>,
    /// Sequences this terminal does not know, or a cursor sent off the screen.
    pub bad: usize,
    /// A new line at the bottom scrolls the screen up (otherwise it is bad).
    pub scroll: bool,
    /// A colour or other style is on (SGR other than a reset).
    pub style: bool,
    /// Each cell was drawn with a style on.
    pub styled: Vec<Vec<bool>>,
}

/// The code point at the start of `s`, as the terminal reads it: anything it cannot read is U+FFFD.
pub fn utf8(s: &[u8]) -> (u32, usize) {
    let n = s.len();
    let u: Vec<u32> = s.iter().take(4).map(|&b| b as u32).collect();
    if u[0] < 0x80 {
        return (u[0], 1);
    }
    if u[0] & 0xE0 == 0xC0 && n >= 2 {
        return (((u[0] & 0x1F) << 6) | (u[1] & 0x3F), 2);
    }
    if u[0] & 0xF0 == 0xE0 && n >= 3 {
        return (((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F), 3);
    }
    if u[0] & 0xF8 == 0xF0 && n >= 4 {
        return (((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F), 4);
    }
    (0xFFFD, 1)
}

impl Term {
    pub fn new(rows: usize, cols: usize, width: usize, deferred: bool) -> Term {
        Term {
            rows,
            cols,
            cell: vec![vec![Vec::new(); cols]; rows],
            r: 0,
            c: 0,
            pend: false,
            width,
            deferred,
            last: None,
            bad: 0,
            scroll: false,
            style: false,
            styled: vec![vec![false; cols]; rows],
        }
    }

    /// From (r, c) to the end of the screen.
    fn erase(&mut self, r: usize, c: usize) {
        let mut c = c;
        for row in r..self.rows {
            for col in c..self.cols {
                self.cell[row][col].clear();
                self.styled[row][col] = false;
            }
            c = 0;
        }
    }

    /// Below the last row: scroll up, if this terminal does.
    fn bottom(&mut self) {
        while self.scroll && self.r >= self.rows {
            self.cell.remove(0);
            self.cell.push(vec![Vec::new(); self.cols]);
            self.styled.remove(0);
            self.styled.push(vec![false; self.cols]);
            self.r -= 1;
            self.last = match self.last {
                Some((r, c)) if r > 0 => Some((r - 1, c)),
                _ => None,
            };
        }
    }

    fn put(&mut self, cp: u32, b: &[u8]) {
        let w = edit::cols(cp);
        if w == 0 {
            if let Some((r, c)) = self.last {
                if self.cell[r][c].len() + b.len() < CELL {
                    self.cell[r][c].extend_from_slice(b);
                }
            }
            return;
        }
        if self.pend {
            self.r += 1;
            self.c = 0;
            self.pend = false;
        }
        if self.c + w > self.width {
            self.r += 1;
            self.c = 0;
        }
        self.bottom();
        if self.r >= self.rows {
            self.bad += 1;
            self.r = self.rows - 1;
            return;
        }
        self.cell[self.r][self.c] = b.to_vec();
        self.styled[self.r][self.c] = self.style;
        if w == 2 {
            self.cell[self.r][self.c + 1] = RIGHT_HALF.to_vec();
            self.styled[self.r][self.c + 1] = self.style;
        }
        self.last = Some((self.r, self.c));
        self.c += w;
        if self.c >= self.width {
            if self.deferred {
                self.c = self.width - 1;
                self.pend = true;
            } else {
                self.r += 1;
                self.c = 0;
            }
        }
    }

    pub fn feed(&mut self, s: &[u8]) {
        let n = s.len();
        let mut i = 0;
        while i < n {
            let ch = s[i];
            if ch == 0x1B {
                if i + 1 >= n || s[i + 1] != b'[' {
                    self.bad += 1;
                    i += 1;
                    continue;
                }
                let mut j = i + 2;
                let (mut num, mut has, mut semi) = (0usize, false, false);
                while j < n && (s[j].is_ascii_digit() || s[j] == b';') {
                    if s[j] == b';' {
                        semi = true;
                    } else {
                        num = num * 10 + (s[j] - b'0') as usize;
                        has = true;
                    }
                    j += 1;
                }
                if j >= n {
                    self.bad += 1;
                    return;
                }
                let f = s[j];
                i = j + 1;
                if semi && f != b'm' {
                    self.bad += 1;
                    continue;
                }
                let k = if has { num } else { 1 };
                match f {
                    b'A' => {
                        if k > self.r {
                            self.bad += 1;
                            self.r = 0;
                        } else {
                            self.r -= k;
                        }
                        self.pend = false;
                    }
                    b'B' => {
                        self.r += k;
                        if self.r >= self.rows {
                            self.bad += 1;
                            self.r = self.rows - 1;
                        }
                        self.pend = false;
                    }
                    b'C' => {
                        self.c = (self.c + k).min(self.width - 1);
                        self.pend = false;
                    }
                    b'J' => {
                        if num != 0 {
                            self.bad += 1;
                        } else {
                            self.erase(self.r, self.c);
                        }
                    }
                    b'K' => {
                        if num != 0 {
                            self.bad += 1;
                        } else {
                            for c in self.c..self.cols {
                                self.cell[self.r][c].clear();
                                self.styled[self.r][c] = false;
                            }
                        }
                    }
                    // colours: only whether one is on is kept ("[m" and "[0m" turn it off)
                    b'm' => self.style = semi || num != 0,
                    _ => self.bad += 1,
                }
                continue;
            }
            if ch == b'\r' {
                self.c = 0;
                self.pend = false;
                i += 1;
                continue;
            }
            if ch == b'\t' {
                self.c = ((self.c / 8 + 1) * 8).min(self.width - 1);
                self.pend = false;
                i += 1;
                continue;
            }
            if ch == b'\n' {
                self.r += 1;
                self.pend = false;
                self.bottom();
                i += 1;
                continue;
            }
            if ch < 0x20 || ch == 0x7F {
                self.bad += 1;
                i += 1;
                continue;
            }
            let (cp, len) = utf8(&s[i..]);
            let len = len.min(n - i);
            self.put(cp, &s[i..i + len]);
            i += len;
        }
    }

    /// A row as text, without the spaces at its end.
    pub fn row(&self, r: usize) -> String {
        let mut b = Vec::new();
        for x in &self.cell[r] {
            if x == RIGHT_HALF {
                continue;
            }
            if x.is_empty() {
                b.push(b' ');
            } else {
                b.extend_from_slice(x);
            }
        }
        while b.last() == Some(&b' ') {
            b.pop();
        }
        String::from_utf8_lossy(&b).into_owned()
    }
}
