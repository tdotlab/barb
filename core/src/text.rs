//! Byte strings. Everything Barb handles (files, command output, paths, the model's text) is
//! bytes that are usually UTF-8 but need not be, so the core works on `[u8]` and leaves checking
//! UTF-8 to the one place that must: the JSON writer.

use alloc::vec::Vec;

/// What the core calls a byte string.
pub type Bytes = Vec<u8>;

/// Something that can be appended to a byte string. The `cat!` macro is built on it.
pub trait Put {
    fn put(&self, out: &mut Vec<u8>);
}

impl Put for [u8] {
    fn put(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(self);
    }
}
impl<const N: usize> Put for [u8; N] {
    fn put(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(self);
    }
}
impl Put for Vec<u8> {
    fn put(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(self);
    }
}
impl Put for str {
    fn put(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(self.as_bytes());
    }
}
impl Put for char {
    fn put(&self, out: &mut Vec<u8>) {
        let mut b = [0u8; 4];
        out.extend_from_slice(self.encode_utf8(&mut b).as_bytes());
    }
}
impl<T: Put + ?Sized> Put for &T {
    fn put(&self, out: &mut Vec<u8>) {
        (**self).put(out);
    }
}

macro_rules! put_int {
    ($($t:ty),*) => {$(
        impl Put for $t {
            fn put(&self, out: &mut Vec<u8>) {
                put_i128(out, *self as i128);
            }
        }
    )*};
}
put_int!(i32, i64, u32, u64, usize);

fn put_i128(out: &mut Vec<u8>, v: i128) {
    let mut digits = [0u8; 40];
    let mut n = 0;
    let mut u = v.unsigned_abs();
    loop {
        digits[n] = b'0' + (u % 10) as u8;
        n += 1;
        u /= 10;
        if u == 0 {
            break;
        }
    }
    if v < 0 {
        out.push(b'-');
    }
    out.extend(digits[..n].iter().rev());
}

/// Concatenate pieces (byte strings, `&str`, integers) into a new `Vec<u8>`.
#[macro_export]
macro_rules! cat {
    ($($x:expr),* $(,)?) => {{
        let mut v = $crate::text::Bytes::new();
        $( $crate::text::Put::put(&$x, &mut v); )*
        v
    }};
}

/// Append pieces to an existing byte string.
#[macro_export]
macro_rules! push {
    ($v:expr; $($x:expr),* $(,)?) => {{
        $( $crate::text::Put::put(&$x, &mut $v); )*
    }};
}

/// `n` right-aligned in `width` columns, as read_file numbers its lines.
pub fn pad(out: &mut Vec<u8>, n: u64, width: usize) {
    let mut d = Vec::new();
    n.put(&mut d);
    for _ in d.len()..width {
        out.push(b' ');
    }
    out.extend_from_slice(&d);
}

fn is_space(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\r' | b'\n')
}

/// The text without spaces, tabs and line breaks at either end.
pub fn trim(s: &[u8]) -> &[u8] {
    let start = s.iter().position(|&c| !is_space(c)).unwrap_or(s.len());
    let end = s.iter().rposition(|&c| !is_space(c)).map_or(start, |i| i + 1);
    &s[start..end]
}

/// Where `needle` first occurs in `hay`. An empty needle is found at 0.
pub fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() {
        return Some(0);
    }
    if needle.len() > hay.len() {
        return None;
    }
    let first = needle[0];
    let mut i = 0;
    while i + needle.len() <= hay.len() {
        match hay[i..=hay.len() - needle.len()].iter().position(|&c| c == first) {
            None => return None,
            Some(k) => i += k,
        }
        if &hay[i..i + needle.len()] == needle {
            return Some(i);
        }
        i += 1;
    }
    None
}

pub fn contains(hay: &[u8], needle: &[u8]) -> bool {
    find(hay, needle).is_some()
}

/// Up to `max` bytes of the first line of `s`.
pub fn first_line(s: &[u8], max: usize) -> &[u8] {
    let n = s.iter().take(max).position(|&c| c == b'\n').unwrap_or(s.len().min(max));
    &s[..n]
}

/// Split into lines on '\n'. Unlike `split`, text that ends with a newline gives no empty last
/// line, and the empty text gives none at all.
pub fn lines(s: &[u8]) -> impl Iterator<Item = &[u8]> {
    let mut rest = Some(s).filter(|s| !s.is_empty());
    core::iter::from_fn(move || {
        let r = rest?;
        match r.iter().position(|&c| c == b'\n') {
            Some(i) => {
                rest = Some(&r[i + 1..]).filter(|s| !s.is_empty());
                Some(&r[..i])
            }
            None => {
                rest = None;
                Some(r)
            }
        }
    })
}

/// Is `c` a byte in the middle of a UTF-8 character?
pub fn is_continuation(c: u8) -> bool {
    c & 0xC0 == 0x80
}
