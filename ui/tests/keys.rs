// Tests for the terminal key decoder (ui/src/keys.rs): the bytes terminals send for keys, and what
// the prompt must get from them. Each case feeds bytes and compares the keys that come out, written
// as a short text: "a" a character, "<LEFT>" an editor key, "<ENTER>" and so on.
use barb_ui::edit::Key;
use barb_ui::keys::{Decoder, Input};

fn name(k: Input) -> String {
    match k {
        Input::Edit(Key::Char(c)) if (0x20..0x7F).contains(&c) => char::from(c as u8).to_string(),
        Input::Edit(Key::Char(c)) => format!("<U+{c:04X}>"),
        Input::Edit(e) => {
            let s = match e {
                Key::Back => "BACK",
                Key::Del => "DEL",
                Key::Left => "LEFT",
                Key::Right => "RIGHT",
                Key::Home => "HOME",
                Key::End => "END",
                Key::WordLeft => "WLEFT",
                Key::WordRight => "WRIGHT",
                Key::Up => "UP",
                Key::Down => "DOWN",
                Key::KillStart => "KSTART",
                Key::KillEnd => "KEND",
                Key::KillWord => "KWORD",
                Key::Clear => "ESC",
                Key::Char(_) => unreachable!(),
            };
            format!("<{s}>")
        }
        Input::Enter => "<ENTER>".into(),
        Input::Eof => "<EOF>".into(),
        Input::Intr => "<INTR>".into(),
        Input::PasteBegin => "<PASTE>".into(),
        Input::PasteEnd => "</PASTE>".into(),
    }
}

/// Feed the bytes and say what came out. "|" marks where timeout was called because the bytes
/// stopped with a key part-way, which is what a caller does.
fn feed(d: &mut Decoder, bytes: &[u8]) -> String {
    let mut got = String::new();
    for &b in bytes {
        if let Some(k) = d.feed(b) {
            got += &name(k);
        }
    }
    if d.pending() {
        got.push('|');
        if let Some(k) = d.timeout() {
            got += &name(k);
        }
    }
    got
}

#[track_caller]
fn keys(bytes: &[u8], want: &str) {
    let got = feed(&mut Decoder::new(), bytes);
    assert_eq!(got, want, "for bytes {:?}", String::from_utf8_lossy(bytes));
}

#[test]
fn text() {
    keys(b"ab c", "ab c");
    keys(b"\t", "<U+0009>"); // the editor makes it a space
    keys(b"\xC3\xA9", "<U+00E9>"); // e acute
    keys(b"\xE2\x82\xAC\xF0\x9F\x98\x80", "<U+20AC><U+1F600>");
    keys(b"\xC3a", "a"); // a broken character is dropped, not what follows
    keys(b"\x80\xBFz", "z"); // continuation bytes on their own
    keys(b"\xE2\x82", "|"); // cut off: given up at the timeout
    keys(b"\xFFq", "q");
    // What is not a real character is dropped: overlong forms (a way to sneak in a control byte),
    // surrogates, past U+10FFFF, and the C1 controls.
    keys(b"\xC0\x8Aa\xE0\x80\x8Db\xF0\x80\x80\x9Bc", "abc");
    keys(b"\xED\xA0\x80d\xF4\x90\x80\x80e\xC2\x9Bf", "def");
    keys(b"\xE0\x82\xA9g\xF0\x80\x82\xA9h", "gh"); // U+00A9, a printable one, the long way round
    keys(b"\xC2\xA0\xF4\x8F\xBF\xBF\xE0\xA0\x80", "<U+00A0><U+10FFFF><U+0800>"); // the edges that are fine
}

#[test]
fn controls() {
    keys(b"\x01\x02\x05\x06", "<HOME><LEFT><END><RIGHT>");
    keys(b"\x08\x7F", "<BACK><BACK>"); // Backspace is one or the other
    keys(b"\x0B\x15\x17", "<KEND><KSTART><KWORD>");
    keys(b"\x10\x0E", "<UP><DOWN>");
    keys(b"\x03\x04", "<INTR><EOF>");
    keys(b"\x07\x0C\x1A\x00", ""); // the rest mean nothing here
}

#[test]
fn enter() {
    keys(b"\r", "<ENTER>");
    keys(b"\n", "<ENTER>");
    keys(b"\r\n", "<ENTER>"); // one Enter, as Windows text has it
    keys(b"\r\r", "<ENTER><ENTER>");
    keys(b"\n\n", "<ENTER><ENTER>");
    keys(b"\n\r", "<ENTER><ENTER>");
    keys(b"\r\n\r\n", "<ENTER><ENTER>");
    keys(b"a\r\nb", "a<ENTER>b");
    keys(b"\rx\n", "<ENTER>x<ENTER>"); // only an LF right after the CR goes with it
    let mut d = Decoder::new(); // CR at the end of one read, LF at the start of the next
    assert_eq!(feed(&mut d, b"a\r"), "a<ENTER>");
    assert_eq!(feed(&mut d, b"\nb"), "b");
}

#[test]
fn sequences() {
    keys(b"\x1b[A\x1b[B\x1b[C\x1b[D", "<UP><DOWN><RIGHT><LEFT>");
    keys(b"\x1bOA\x1bOB\x1bOC\x1bOD", "<UP><DOWN><RIGHT><LEFT>"); // the application keypad's
    keys(b"\x1b[H\x1b[F\x1bOH\x1bOF", "<HOME><END><HOME><END>");
    keys(b"\x1b[1~\x1b[7~\x1b[4~\x1b[8~", "<HOME><HOME><END><END>");
    keys(b"\x1b[3~", "<DEL>");
    keys(b"\x1b[1;5C\x1b[1;5D", "<WRIGHT><WLEFT>"); // Ctrl
    keys(b"\x1b[1;3C\x1b[1;3D", "<WRIGHT><WLEFT>"); // Alt
    keys(b"\x1b[1;2C\x1b[1;2D", "<RIGHT><LEFT>"); // Shift is no different
    keys(b"\x1b[1;7D", "<WLEFT>"); // Ctrl and Alt
    keys(b"\x1b[2~\x1b[5~\x1b[6~\x1b[15~\x1b[Z", ""); // Insert, page keys, F5, Shift-Tab: nothing
    keys(b"\x1bb\x1bf\x1b\x7F\x1b\x08", "<WLEFT><WRIGHT><KWORD><KWORD>"); // Alt-b, Alt-f, Alt-Backspace
    keys(b"\x1bxy", "y"); // an Alt key with no meaning here
    keys(b"x\x1b[Dy", "x<LEFT>y");
}

#[test]
fn escape() {
    keys(b"\x1b", "|<ESC>"); // a lone Escape, known only when nothing follows
    keys(b"\x1b\x1b", "<ESC>|<ESC>");
    keys(b"\x1b\x1b[D", "<ESC><LEFT>");
    keys(b"\x1b[", "|"); // a sequence cut off is dropped, not typed
    keys(b"\x1b[1;", "|");
    keys(b"\x1b\r", "<ENTER>"); // a control byte breaks a sequence off, and counts
    keys(b"\x1b[1\ra", "<ENTER>a");
    keys(b"\x1b[1\x1b[C", "<RIGHT>");
    keys(b"\x1b[1\x01b\x1b\x15", "<HOME>b<KSTART>");
    keys(b"\x1b\xC3\xA9", "<U+00E9>");
    // A sequence too long to be a real one is read to its end and dropped, not typed.
    keys(b"\x1b[11111111111111111111111111111111111111111111111Ax", "x");
    keys(b"\x1b[9999999999;99999D", "<WLEFT>"); // big numbers do not overflow
    keys(b"\x1b[1;4294967297D", "<WLEFT>"); // (2^32 + 1: wrapped, it would be 1, no modifier)
}

#[test]
fn paste() {
    keys(b"\x1b[200~hi\x1b[201~", "<PASTE>hi</PASTE>");
    keys(b"\x1b[200~one\rtwo\r\n\nthree\x1b[201~", "<PASTE>one<ENTER>two<ENTER><ENTER>three</PASTE>");
    // Inside a paste, control characters and keys' sequences are not commands.
    keys(b"\x1b[200~a\x01\x03\x04\x15\x7Fb\x1b[D\x1b[3~\x1bbc\x1b[201~", "<PASTE>abc</PASTE>");
    keys(b"\x1b[200~a\tb\x1b[201~", "<PASTE>a<U+0009>b</PASTE>");
    keys(b"\x1b[200~\xC3\xA9\x1b[201~", "<PASTE><U+00E9></PASTE>");
    keys(b"\x1b[200~\x1b", "<PASTE>|"); // no Escape key from inside a paste
    keys(b"\x1b[200~\x1b\x1b[201~x", "<PASTE></PASTE>x");
    keys(b"\x1b[200~x\x1b[201~\x01", "<PASTE>x</PASTE><HOME>"); // after it, keys are keys again
    keys(b"\x1b[201~a", "a"); // an end with no start is nothing
    let mut d = Decoder::new(); // a paste split across reads, even its end marker
    assert_eq!(feed(&mut d, b"\x1b[200~ab\x1b[20"), "<PASTE>ab|");
    let mut d = Decoder::new();
    let mut got = String::new();
    for &b in b"\x1b[200~ab\x1b[201~c" {
        // byte by byte, no timeouts
        if let Some(k) = d.feed(b) {
            got += &name(k);
        }
    }
    assert_eq!(got, "<PASTE>ab</PASTE>c");
}

/// Random bytes: every key that comes out is one the prompt knows, with a real character. (The
/// kinds are an enum here, and the decoder's own bounds are checked by indexing, which panics.)
#[test]
fn random() {
    let mut seed: u32 = 12345;
    let mut d = Decoder::new();
    let (mut n, mut bad) = (0, 0);
    for _ in 0..400000 {
        seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
        let r = (seed >> 16) & 0xFFFF;
        let b = if r % 7 == 0 {
            0x1B
        } else if r % 11 == 0 {
            b"[O~;0123456789ABCDHF"[((r >> 4) % 20) as usize]
        } else {
            (r >> 3) as u8
        };
        if let Some(k) = d.feed(b) {
            n += 1;
            if let Input::Edit(Key::Char(c)) = k {
                if c > 0x10FFFF || (c < 0x20 && c != '\t' as u32) || c == 0x7F || (0x80..0xA0).contains(&c) {
                    bad += 1;
                }
            }
        }
        if r % 97 == 0 && d.pending() && d.timeout().is_some() {
            n += 1;
        }
    }
    assert!(n > 10000, "only {n} keys");
    assert_eq!(bad, 0);
}
