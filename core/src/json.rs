//! JSON for the core: a pull reader, a few lookups, and a writer.
//!
//! The reader hands out one token at a time and checks the grammar as it goes (RFC 8259: no
//! trailing commas, no leading zeros, no raw control characters in strings, escapes must be
//! valid) and refuses nesting past [`MAX_DEPTH`], so a hostile or broken reply cannot overflow
//! anything. It does not build a tree. String and key tokens are given still escaped;
//! [`unescape`] turns one into real text.
//!
//! UTF-8 is not checked on the way in; the writer makes sure what goes out is valid (see
//! [`Writer::str`]), because the API rejects a request with a bad byte in it for good, and one
//! binary file read by a tool would otherwise poison every later request of the conversation.

use alloc::vec::Vec;

pub const MAX_DEPTH: usize = 64;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Tok {
    /// The input is not valid JSON; see [`Reader::err`].
    Err,
    /// The one top-level value is complete and nothing follows.
    End,
    Obj,
    ObjEnd,
    Arr,
    ArrEnd,
    /// An object key; [`Reader::tok`] is its text, escaped, without quotes.
    Key,
    /// A string value; the same.
    Str,
    /// A number; [`Reader::tok`] is its text.
    Num,
    True,
    False,
    Null,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Mode {
    Value,
    ValueOrEnd,
    Key,
    KeyOrEnd,
    After,
}

pub struct Reader<'a> {
    src: &'a [u8],
    pos: usize,
    tok: (usize, usize),
    /// Offset of the first byte of the current token (the quote, for a string).
    tok_start: usize,
    depth: usize,
    is_arr: [bool; MAX_DEPTH],
    mode: Mode,
    err: Option<&'static str>,
    err_pos: usize,
}

fn is_ws(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | b'\r')
}

impl<'a> Reader<'a> {
    pub fn new(src: &'a [u8]) -> Self {
        Reader {
            src,
            pos: 0,
            tok: (0, 0),
            tok_start: 0,
            depth: 0,
            is_arr: [false; MAX_DEPTH],
            mode: Mode::Value,
            err: None,
            err_pos: 0,
        }
    }

    /// The text of the current key, string or number token.
    pub fn tok(&self) -> &'a [u8] {
        &self.src[self.tok.0..self.tok.1]
    }
    pub fn err(&self) -> Option<&'static str> {
        self.err
    }
    pub fn err_pos(&self) -> usize {
        self.err_pos
    }
    pub fn depth(&self) -> usize {
        self.depth
    }

    fn fail(&mut self, msg: &'static str) -> Tok {
        if self.err.is_none() {
            self.err = Some(msg);
            self.err_pos = self.pos;
        }
        Tok::Err
    }

    fn fail_at(&mut self, i: usize, msg: &'static str) -> bool {
        self.pos = i;
        self.fail(msg);
        false
    }

    // pos is at the opening quote. Leaves pos after the closing one.
    fn scan_string(&mut self) -> bool {
        let s = self.src;
        let n = s.len();
        let start = self.pos + 1;
        let mut i = start;
        loop {
            if i >= n {
                return self.fail_at(i, "unterminated string");
            }
            let c = s[i];
            if c == b'"' {
                break;
            }
            if c < 0x20 {
                return self.fail_at(i, "control character in string");
            }
            if c == b'\\' {
                if i + 1 >= n {
                    return self.fail_at(i, "unterminated string");
                }
                let e = s[i + 1];
                if e == b'u' {
                    if i + 5 >= n {
                        return self.fail_at(i, "short \\u escape");
                    }
                    if !s[i + 2..i + 6].iter().all(u8::is_ascii_hexdigit) {
                        return self.fail_at(i, "bad \\u escape");
                    }
                    i += 6;
                    continue;
                }
                if !matches!(e, b'"' | b'\\' | b'/' | b'b' | b'f' | b'n' | b'r' | b't') {
                    return self.fail_at(i, "bad escape");
                }
                i += 2;
                continue;
            }
            i += 1;
        }
        self.tok = (start, i);
        self.pos = i + 1;
        true
    }

    fn scan_number(&mut self) -> bool {
        let s = self.src;
        let n = s.len();
        let start = self.pos;
        let mut i = start;
        let digit = |i: usize| i < n && s[i].is_ascii_digit();
        if i < n && s[i] == b'-' {
            i += 1;
        }
        if !digit(i) {
            return self.fail_at(i, "bad number");
        }
        if s[i] == b'0' {
            i += 1;
            if digit(i) {
                return self.fail_at(i, "leading zero in number");
            }
        } else {
            while digit(i) {
                i += 1;
            }
        }
        if i < n && s[i] == b'.' {
            i += 1;
            if !digit(i) {
                return self.fail_at(i, "bad number");
            }
            while digit(i) {
                i += 1;
            }
        }
        if i < n && (s[i] == b'e' || s[i] == b'E') {
            i += 1;
            if i < n && (s[i] == b'+' || s[i] == b'-') {
                i += 1;
            }
            if !digit(i) {
                return self.fail_at(i, "bad number");
            }
            while digit(i) {
                i += 1;
            }
        }
        self.tok = (start, i);
        self.pos = i;
        true
    }

    fn scan_word(&mut self, w: &[u8]) -> bool {
        if !self.src[self.pos..].starts_with(w) {
            self.fail("bad literal");
            return false;
        }
        self.pos += w.len();
        // "true1" would read as two tokens, so insist on a delimiter here.
        if let Some(&c) = self.src.get(self.pos) {
            if !(is_ws(c) || c == b',' || c == b']' || c == b'}') {
                self.fail("bad literal");
                return false;
            }
        }
        true
    }

    fn after_value(&mut self, t: Tok) -> Tok {
        self.mode = Mode::After;
        t
    }

    fn skip_ws(&mut self) {
        while self.pos < self.src.len() && is_ws(self.src[self.pos]) {
            self.pos += 1;
        }
    }

    pub fn next(&mut self) -> Tok {
        if self.err.is_some() {
            return Tok::Err;
        }
        loop {
            self.skip_ws();
            if self.pos >= self.src.len() {
                if self.depth == 0 && self.mode == Mode::After {
                    return Tok::End;
                }
                return self.fail("unexpected end of input");
            }
            let c = self.src[self.pos];
            self.tok_start = self.pos;

            if self.mode == Mode::After {
                if self.depth == 0 {
                    return self.fail("data after the value");
                }
                let arr = self.is_arr[self.depth - 1];
                if c == b',' {
                    self.pos += 1;
                    self.mode = if arr { Mode::Value } else { Mode::Key };
                    continue;
                }
                if c == if arr { b']' } else { b'}' } {
                    self.pos += 1;
                    self.depth -= 1;
                    return self.after_value(if arr { Tok::ArrEnd } else { Tok::ObjEnd });
                }
                return self.fail(if arr { "expected , or ]" } else { "expected , or }" });
            }

            if self.mode == Mode::Key || self.mode == Mode::KeyOrEnd {
                if c == b'}' && self.mode == Mode::KeyOrEnd {
                    self.pos += 1;
                    self.depth -= 1;
                    return self.after_value(Tok::ObjEnd);
                }
                if c != b'"' {
                    return self.fail("expected a key");
                }
                if !self.scan_string() {
                    return Tok::Err;
                }
                self.skip_ws();
                if self.pos >= self.src.len() || self.src[self.pos] != b':' {
                    return self.fail("expected :");
                }
                self.pos += 1;
                self.mode = Mode::Value;
                return Tok::Key;
            }

            if c == b']' && self.mode == Mode::ValueOrEnd {
                self.pos += 1;
                self.depth -= 1;
                return self.after_value(Tok::ArrEnd);
            }
            return match c {
                b'{' | b'[' => {
                    if self.depth >= MAX_DEPTH {
                        return self.fail("nested too deeply");
                    }
                    self.is_arr[self.depth] = c == b'[';
                    self.depth += 1;
                    self.pos += 1;
                    if c == b'[' {
                        self.mode = Mode::ValueOrEnd;
                        Tok::Arr
                    } else {
                        self.mode = Mode::KeyOrEnd;
                        Tok::Obj
                    }
                }
                b'"' => {
                    if !self.scan_string() {
                        return Tok::Err;
                    }
                    self.after_value(Tok::Str)
                }
                b't' => self.word(b"true", Tok::True),
                b'f' => self.word(b"false", Tok::False),
                b'n' => self.word(b"null", Tok::Null),
                b'-' | b'0'..=b'9' => {
                    if !self.scan_number() {
                        return Tok::Err;
                    }
                    self.after_value(Tok::Num)
                }
                _ => self.fail("unexpected character"),
            };
        }
    }

    fn word(&mut self, w: &[u8], t: Tok) -> Tok {
        if !self.scan_word(w) {
            return Tok::Err;
        }
        self.after_value(t)
    }

    /// Having just read the first token of a value (`Obj`, `Arr` or a scalar), skip the rest of
    /// it. False if the input turned out to be invalid.
    pub fn skip(&mut self, first: Tok) -> bool {
        match first {
            Tok::Err | Tok::End => false,
            Tok::Obj | Tok::Arr => {
                let target = self.depth - 1;
                while self.depth > target {
                    if matches!(self.next(), Tok::Err | Tok::End) {
                        return false;
                    }
                }
                true
            }
            _ => true,
        }
    }

    /// The same, giving the value's exact text (a string's includes its quotes).
    pub fn capture(&mut self, first: Tok) -> Option<&'a [u8]> {
        let start = self.tok_start;
        if !self.skip(first) {
            return None;
        }
        Some(&self.src[start..self.pos])
    }
}

fn is_value_start(t: Tok) -> bool {
    !matches!(t, Tok::Err | Tok::End | Tok::ObjEnd | Tok::ArrEnd | Tok::Key)
}

/// In the object `obj`, the raw text of the value under `key` (the first match wins). None if
/// there is no such key, or `obj` is not a valid object.
pub fn get<'a>(obj: &'a [u8], key: &str) -> Option<&'a [u8]> {
    let mut j = Reader::new(obj);
    if j.next() != Tok::Obj {
        return None;
    }
    loop {
        if j.next() != Tok::Key {
            return None;
        }
        let matched = j.tok() == key.as_bytes();
        let v = j.next();
        if !is_value_start(v) {
            return None;
        }
        let raw = j.capture(v)?;
        if matched {
            return Some(raw);
        }
    }
}

fn put_utf8(out: &mut Vec<u8>, cp: u32) {
    if cp < 0x80 {
        out.push(cp as u8);
    } else if cp < 0x800 {
        out.push((0xC0 | (cp >> 6)) as u8);
        out.push((0x80 | (cp & 0x3F)) as u8);
    } else if cp < 0x10000 {
        out.push((0xE0 | (cp >> 12)) as u8);
        out.push((0x80 | ((cp >> 6) & 0x3F)) as u8);
        out.push((0x80 | (cp & 0x3F)) as u8);
    } else {
        out.push((0xF0 | (cp >> 18)) as u8);
        out.push((0x80 | ((cp >> 12) & 0x3F)) as u8);
        out.push((0x80 | ((cp >> 6) & 0x3F)) as u8);
        out.push((0x80 | (cp & 0x3F)) as u8);
    }
}

fn hex4(p: &[u8]) -> Option<u32> {
    if p.len() < 4 {
        return None;
    }
    let mut v = 0u32;
    for &c in &p[..4] {
        v = v * 16 + (c as char).to_digit(16)?;
    }
    Some(v)
}

/// Decode the text of a string token (no quotes). Every escape is decoded, `\u` surrogate pairs
/// into one character and a lone surrogate into U+FFFD. The input is expected to have come from
/// the reader; a malformed escape (which the reader would have refused) is kept as it is.
pub fn unescape(raw: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(raw.len());
    let mut i = 0;
    while i < raw.len() {
        let c = raw[i];
        if c != b'\\' || i + 1 >= raw.len() {
            out.push(c);
            i += 1;
            continue;
        }
        let e = raw[i + 1];
        i += 2;
        match e {
            b'b' => out.push(8),
            b'f' => out.push(12),
            b'n' => out.push(b'\n'),
            b'r' => out.push(b'\r'),
            b't' => out.push(b'\t'),
            b'u' => {
                let Some(mut cp) = hex4(&raw[i..]) else {
                    out.extend_from_slice(b"\\u");
                    continue;
                };
                i += 4;
                if (0xD800..=0xDBFF).contains(&cp) {
                    let lo = if raw[i..].starts_with(b"\\u") { hex4(&raw[i + 2..]) } else { None };
                    match lo {
                        Some(lo @ 0xDC00..=0xDFFF) => {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                        _ => cp = 0xFFFD,
                    }
                } else if (0xDC00..=0xDFFF).contains(&cp) {
                    cp = 0xFFFD;
                }
                put_utf8(&mut out, cp);
            }
            _ => out.push(e), // " \ /
        }
    }
    out
}

/// `raw` is a string value as captured, with its quotes. None if it is not one.
pub fn as_str(raw: &[u8]) -> Option<Vec<u8>> {
    if raw.len() < 2 || raw[0] != b'"' || raw[raw.len() - 1] != b'"' {
        return None;
    }
    Some(unescape(&raw[1..raw.len() - 1]))
}

/// A whole number in i64 range, no fraction or exponent.
pub fn as_int(raw: &[u8]) -> Option<i64> {
    let (neg, digits) = match raw.first() {
        Some(b'-') => (true, &raw[1..]),
        _ => (false, raw),
    };
    if digits.is_empty() {
        return None;
    }
    let mut v: u64 = 0;
    for &c in digits {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((c - b'0') as u64)?;
    }
    if neg {
        if v > i64::MAX as u64 + 1 {
            return None;
        }
        Some((v as i64).wrapping_neg())
    } else {
        if v > i64::MAX as u64 {
            return None;
        }
        Some(v as i64)
    }
}

/// true or false, as captured.
pub fn as_bool(raw: &[u8]) -> Option<bool> {
    match raw {
        b"true" => Some(true),
        b"false" => Some(false),
        _ => None,
    }
}

/// Is the whole text one valid JSON value?
pub fn valid(s: &[u8]) -> bool {
    let mut j = Reader::new(s);
    let t = j.next();
    if !is_value_start(t) || !j.skip(t) {
        return false;
    }
    j.next() == Tok::End
}

// ---- writer ----

pub const WRITER_MAX_DEPTH: usize = 64;

/// Builds JSON text into `out`. Calls that do not nest properly set `bad` instead of failing.
pub struct Writer {
    pub out: Vec<u8>,
    first: [bool; WRITER_MAX_DEPTH + 1],
    depth: usize,
    after_key: bool,
    pub bad: bool,
}

impl Default for Writer {
    fn default() -> Self {
        Self::new()
    }
}

impl Writer {
    pub fn new() -> Self {
        let mut first = [false; WRITER_MAX_DEPTH + 1];
        first[0] = true;
        Writer { out: Vec::new(), first, depth: 0, after_key: false, bad: false }
    }

    // What comes before any value or key: a comma unless it is the first thing at this depth,
    // and nothing at all right after a key.
    fn pre(&mut self) {
        if self.after_key {
            self.after_key = false;
            return;
        }
        if !self.first[self.depth] {
            self.out.push(b',');
        }
        self.first[self.depth] = false;
    }

    fn open(&mut self, c: u8) {
        self.pre();
        self.out.push(c);
        if self.depth >= WRITER_MAX_DEPTH {
            self.bad = true;
            return;
        }
        self.depth += 1;
        self.first[self.depth] = true;
    }

    fn close(&mut self, c: u8) {
        if self.depth == 0 {
            self.bad = true;
            return;
        }
        self.depth -= 1;
        self.out.push(c);
    }

    pub fn obj(&mut self) {
        self.open(b'{');
    }
    pub fn obj_end(&mut self) {
        self.close(b'}');
    }
    pub fn arr(&mut self) {
        self.open(b'[');
    }
    pub fn arr_end(&mut self) {
        self.close(b']');
    }

    pub fn key(&mut self, key: &str) {
        self.pre();
        put_str(&mut self.out, key.as_bytes());
        self.out.push(b':');
        self.after_key = true;
    }

    /// Any bytes: invalid UTF-8 becomes U+FFFD.
    pub fn str(&mut self, s: impl AsRef<[u8]>) {
        self.pre();
        put_str(&mut self.out, s.as_ref());
    }
    pub fn int(&mut self, v: i64) {
        self.pre();
        crate::text::Put::put(&v, &mut self.out);
    }
    pub fn bool(&mut self, v: bool) {
        self.pre();
        self.out.extend_from_slice(if v { b"true" } else { b"false" });
    }
    pub fn null(&mut self) {
        self.pre();
        self.out.extend_from_slice(b"null");
    }
    /// Text that is already valid JSON, written as it is.
    pub fn raw(&mut self, json: &[u8]) {
        self.pre();
        self.out.extend_from_slice(json);
    }
}

/// The length of the valid UTF-8 sequence at the start of `p`, or 0.
pub fn utf8_len(p: &[u8]) -> usize {
    let c = p[0];
    let cont = |k: usize| p.len() > k && p[k] & 0xC0 == 0x80;
    match c {
        0..=0x7F => 1,
        0xC2..=0xDF => {
            if cont(1) {
                2
            } else {
                0
            }
        }
        0xE0..=0xEF => {
            if !(cont(1) && cont(2)) {
                return 0;
            }
            if c == 0xE0 && p[1] < 0xA0 {
                return 0; // overlong
            }
            if c == 0xED && p[1] >= 0xA0 {
                return 0; // a surrogate
            }
            3
        }
        0xF0..=0xF4 => {
            if !(cont(1) && cont(2) && cont(3)) {
                return 0;
            }
            if c == 0xF0 && p[1] < 0x90 {
                return 0; // overlong
            }
            if c == 0xF4 && p[1] >= 0x90 {
                return 0; // past U+10FFFF
            }
            4
        }
        _ => 0,
    }
}

fn put_str(out: &mut Vec<u8>, p: &[u8]) {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    out.push(b'"');
    let mut run = 0; // start of the bytes not yet copied, to copy them in big pieces
    let mut i = 0;
    while i < p.len() {
        let c = p[i];
        if (0x20..0x80).contains(&c) && c != b'"' && c != b'\\' {
            i += 1;
            continue;
        }
        if c >= 0x80 {
            let len = utf8_len(&p[i..]);
            if len > 0 {
                i += len;
                continue;
            }
        }
        out.extend_from_slice(&p[run..i]);
        match c {
            b'"' => out.extend_from_slice(b"\\\""),
            b'\\' => out.extend_from_slice(b"\\\\"),
            b'\n' => out.extend_from_slice(b"\\n"),
            b'\r' => out.extend_from_slice(b"\\r"),
            b'\t' => out.extend_from_slice(b"\\t"),
            0..=0x1F => out.extend_from_slice(&[
                b'\\',
                b'u',
                b'0',
                b'0',
                HEX[(c >> 4) as usize],
                HEX[(c & 15) as usize],
            ]),
            _ => out.extend_from_slice(b"\\ufffd"), // a byte that is not valid UTF-8
        }
        i += 1;
        run = i;
    }
    out.extend_from_slice(&p[run..]);
    out.push(b'"');
}
