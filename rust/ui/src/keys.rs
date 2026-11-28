//! The keys a terminal sends, read from the bytes it sends them as.
//!
//! A terminal in raw mode reports a key press as bytes: a character as its UTF-8, an arrow as an
//! escape sequence (ESC [ D), Ctrl-A as byte 1. The decoder turns them back into the editor's keys
//! and the few that the prompt handles itself: Enter, Ctrl-D, Ctrl-C, and the start and end of a
//! bracketed paste (ESC [ 200 ~ ... ESC [ 201 ~), which a terminal sends around pasted text once it
//! is asked to.
//!
//! Like the editor, it does no I/O. Feed it bytes as they arrive. A lone ESC is ambiguous (the
//! Escape key, or the start of a sequence whose rest has not arrived yet), so when
//! [`Decoder::pending`] says a key is part-way, wait a moment for more bytes and then call
//! [`Decoder::timeout`].

use crate::edit::Key;

/// What a key press means to the prompt.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Input {
    /// An editor key.
    Edit(Key),
    /// CR, LF or CR LF.
    Enter,
    /// Ctrl-D.
    Eof,
    /// Ctrl-C (only seen when the terminal does not make it a signal).
    Intr,
    PasteBegin,
    PasteEnd,
}

const SEQ: usize = 32;

#[derive(Default, Clone)]
pub struct Decoder {
    seq: [u8; SEQ], // an escape sequence so far, from its ESC
    seqn: usize,
    cp: u32,         // a UTF-8 character so far
    need: u32,       // its continuation bytes still to come
    least: u32,      // the smallest value its length may carry (less is an overlong form)
    pasting: bool,   // between the brackets of a paste: everything is text
    after_cr: bool,  // the last byte was CR, so an LF right after it is the same Enter
}

fn edit(k: Key) -> Option<Input> {
    Some(Input::Edit(k))
}

/// The first two numbers of a sequence's parameters ("1;5" is 1 and 5); 0 where there is none.
fn params(s: &[u8]) -> (u32, u32) {
    let mut v = [0u32; 2];
    let mut at = 0;
    for &c in s {
        if at >= 2 {
            break;
        }
        if c.is_ascii_digit() {
            if v[at] < 100000 {
                v[at] = v[at] * 10 + (c - b'0') as u32;
            }
        } else if c == b';' {
            at += 1;
        }
    }
    (v[0], v[1])
}

impl Decoder {
    pub fn new() -> Decoder {
        Decoder::default()
    }

    /// A finished escape sequence: the key it means, or None for one the prompt has no use for.
    fn sequence(&mut self) -> Option<Input> {
        let n = self.seqn;
        let s = &self.seq[..n];
        self.seqn = 0;
        if self.pasting {
            // pasted text is text: only the end of the paste counts
            if s == b"\x1b[201~" {
                self.pasting = false;
                return Some(Input::PasteEnd);
            }
            return None;
        }
        if s[1] == b'[' || s[1] == b'O' {
            let (p1, p2) = params(&s[2..n - 1]);
            // xterm adds a modifier as 1 + its bits (Shift 1, Alt 2, Ctrl 4); Alt or Ctrl moves by word.
            let word = p2 > 1 && (p2 - 1) & 6 != 0;
            return match s[n - 1] {
                b'A' => edit(Key::Up),
                b'B' => edit(Key::Down),
                b'C' => edit(if word { Key::WordRight } else { Key::Right }),
                b'D' => edit(if word { Key::WordLeft } else { Key::Left }),
                b'H' => edit(Key::Home),
                b'F' => edit(Key::End),
                b'~' if s[1] == b'[' => match p1 {
                    1 | 7 => edit(Key::Home),
                    4 | 8 => edit(Key::End),
                    3 => edit(Key::Del),
                    200 => {
                        self.pasting = true;
                        Some(Input::PasteBegin)
                    }
                    _ => None,
                },
                _ => None,
            };
        }
        match s[1] {
            // ESC and one byte: that key with Alt (Meta)
            b'b' | b'B' => edit(Key::WordLeft),
            b'f' | b'F' => edit(Key::WordRight),
            0x7F | 0x08 => edit(Key::KillWord),
            _ => None,
        }
    }

    /// The next byte of an escape sequence: the key, if it finished one that means a key. A
    /// control byte breaks a sequence off; it is then read as itself (the second value).
    fn in_sequence(&mut self, b: u8) -> (Option<Input>, bool) {
        if b == 0x1B {
            // ESC ESC: the first was the Escape key, the second starts afresh
            if self.seqn == 1 && !self.pasting {
                return (edit(Key::Clear), false);
            }
            self.seqn = 1;
            return (None, false);
        }
        if self.seqn == 1 && (b == 0x7F || b == 0x08) {
            self.seq[1] = b;
            self.seqn = 2;
            return (self.sequence(), false);
        }
        if !(0x20..0x7F).contains(&b) {
            self.seqn = 0;
            return (None, true);
        }
        if self.seqn == SEQ {
            // too long to be a real one: read on to its end, and drop it
            self.seq[1] = 0; // (a sequence that means nothing)
            self.seqn -= 1;
        }
        self.seq[self.seqn] = b;
        self.seqn += 1;
        if self.seqn == 2 {
            return (if b == b'[' || b == b'O' { None } else { self.sequence() }, false);
        }
        if self.seq[1] == b'O' || (0x40..=0x7E).contains(&b) {
            return (self.sequence(), false); // the final byte
        }
        (None, false)
    }

    /// One byte in; the key, if it finished one.
    pub fn feed(&mut self, b: u8) -> Option<Input> {
        let after_cr = self.after_cr;
        self.after_cr = false;
        if self.seqn > 0 {
            let (got, again) = self.in_sequence(b);
            if !again {
                return got;
            }
        }
        if self.need > 0 {
            if b & 0xC0 == 0x80 {
                self.cp = (self.cp << 6) | (b & 0x3F) as u32;
                self.need -= 1;
                if self.need > 0 {
                    return None;
                }
                // only a real character: no overlong form, surrogate or control
                let c = self.cp;
                if c < self.least || c > 0x10FFFF || (0xD800..=0xDFFF).contains(&c) || c < 0xA0 {
                    return None;
                }
                return edit(Key::Char(c));
            }
            self.need = 0; // a broken character is dropped, and this byte read afresh
        }
        if (0xC0..0xF8).contains(&b) {
            self.need = if b >= 0xF0 { 3 } else if b >= 0xE0 { 2 } else { 1 };
            self.cp = (b & (0x3F >> self.need)) as u32;
            self.least = match self.need {
                1 => 0x80,
                2 => 0x800,
                _ => 0x10000,
            };
            return None;
        }
        if b >= 0x80 {
            return None; // a continuation byte with nothing to continue
        }
        if (0x20..0x7F).contains(&b) {
            return edit(Key::Char(b as u32));
        }
        if b == b'\r' || b == b'\n' {
            if b == b'\n' && after_cr {
                return None;
            }
            self.after_cr = b == b'\r';
            return Some(Input::Enter);
        }
        if b == 0x1B {
            self.seq[0] = b;
            self.seqn = 1;
            return None;
        }
        if b == b'\t' {
            return edit(Key::Char(b'\t' as u32));
        }
        if self.pasting {
            return None; // a control character in pasted text is not a command
        }
        match b {
            1 => edit(Key::Home),         // Ctrl-A
            2 => edit(Key::Left),         // Ctrl-B
            3 => Some(Input::Intr),       // Ctrl-C
            4 => Some(Input::Eof),        // Ctrl-D
            5 => edit(Key::End),          // Ctrl-E
            6 => edit(Key::Right),        // Ctrl-F
            8 | 0x7F => edit(Key::Back),  // Backspace sends one or the other
            11 => edit(Key::KillEnd),     // Ctrl-K
            14 => edit(Key::Down),        // Ctrl-N
            16 => edit(Key::Up),          // Ctrl-P
            21 => edit(Key::KillStart),   // Ctrl-U
            23 => edit(Key::KillWord),    // Ctrl-W
            _ => None,
        }
    }

    /// Between the brackets of a paste.
    pub fn pasting(&self) -> bool {
        self.pasting
    }

    /// A key is part-way through (an escape sequence or a UTF-8 character).
    pub fn pending(&self) -> bool {
        self.seqn > 0 || self.need > 0
    }

    /// No more bytes came: what is part-way is given up, except a lone ESC, which was the Escape key.
    pub fn timeout(&mut self) -> Option<Input> {
        let lone_esc = self.seqn == 1 && !self.pasting;
        self.seqn = 0;
        self.need = 0;
        if lone_esc { edit(Key::Clear) } else { None }
    }
}
