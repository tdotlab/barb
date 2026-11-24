// Tests for the terminal markdown renderer (ui/src/md.rs).
//
// The renderer writes bytes for a terminal: text and SGR escapes. The tests look at two views of
// it: the text alone (the escapes taken out), which is what a reader sees, and the raw bytes,
// where the styles are. A random run then feeds it markdown-shaped noise and checks what must
// hold for any reply: no escape but a style ever reaches the terminal, no row is wider than the
// terminal, the terminal is left unstyled, and no letter of the reply goes missing.
use barb_ui::edit;
use barb_ui::md::{self, Md};

fn render_w(s: &[u8], width: usize, ascii: bool) -> Vec<u8> {
    let mut m = Md::new(width, ascii);
    m.render(s);
    m.out
}
fn render(s: &[u8]) -> Vec<u8> {
    render_w(s, 40, false)
}

/// The text without its escapes.
fn plain(s: &[u8]) -> Vec<u8> {
    let mut t = Vec::new();
    let mut i = 0;
    while i < s.len() {
        if s[i] == 0x1B && i + 1 < s.len() && s[i + 1] == b'[' {
            i += 2;
            while i < s.len() && !(0x40..=0x7E).contains(&s[i]) {
                i += 1;
            }
            i += 1;
            continue;
        }
        t.push(s[i]);
        i += 1;
    }
    t
}

fn show(b: &[u8]) -> String {
    String::from_utf8_lossy(b).escape_debug().to_string()
}

#[track_caller]
fn same(got: &[u8], want: &[u8], input: &[u8]) {
    assert!(got == want, "for {:?}\n got: \"{}\"\nwant: \"{}\"", show(input), show(got), show(want));
}

#[track_caller]
fn shows(input: &[u8], want: &[u8]) {
    same(&plain(&render(input)), want, input);
}
#[track_caller]
fn shows_w(input: &[u8], width: usize, ascii: bool, want: &[u8]) {
    same(&plain(&render_w(input, width, ascii)), want, input);
}

fn has(hay: &[u8], needle: &[u8]) -> bool {
    hay.windows(needle.len()).any(|w| w == needle)
}
#[track_caller]
fn raw_has(input: &[u8], needle: &[u8]) {
    let r = render(input);
    assert!(has(&r, needle), "output of {:?} lacks \"{}\": \"{}\"", show(input), show(needle), show(&r));
}
#[track_caller]
fn raw_lacks(input: &[u8], needle: &[u8]) {
    let r = render(input);
    assert!(!has(&r, needle), "output of {:?} has \"{}\": \"{}\"", show(input), show(needle), show(&r));
}

/// n box-drawing horizontals.
fn rule(n: usize) -> Vec<u8> {
    "\u{2500}".repeat(n).into_bytes()
}

#[test]
fn text() {
    shows(b"", b"\n");
    shows(b"hello", b"hello\n");
    shows(b"a\nb", b"a\nb\n");
    shows(b"a\r\nb\r\n", b"a\nb\n\n");
    shows(b"a\n\nb", b"a\n\nb\n");
    // Wrapping at width - 1, on words.
    shows(b"aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii", b"aaaa bbbb cccc dddd eeee ffff gggg hhhh\niiii\n");
    shows(b"aaaa bbbb cccc dddd eeee ffff gggg hhhhh", b"aaaa bbbb cccc dddd eeee ffff gggg\nhhhhh\n");
    // A word longer than a row starts a row of its own and breaks where that fills.
    shows(b"x 0123456789012345678901234567890123456789ab", b"x\n012345678901234567890123456789012345678\n9ab\n");
    shows(b"trailing spaces   ", b"trailing spaces\n");
    shows(b"  indented", b"  indented\n");
    // The narrowest wrap is 20 columns.
    shows_w(b"aaaa bbbb cccc dddd eeee", 5, false, b"aaaa bbbb cccc dddd\neeee\n");
    // Wide characters take two columns.
    let ten = "\u{4E2D}".repeat(10);
    shows_w(format!("{ten} x").as_bytes(), 21, false, format!("{ten}\nx\n").as_bytes());
}

#[test]
fn inline() {
    shows(b"**bold** and *it* and _it_ and __b__", b"bold and it and it and b\n");
    raw_has(b"**bold**", b"\x1b[0;1mbold\x1b[0m");
    raw_has(b"*it*", b"\x1b[0;3mit\x1b[0m");
    raw_has(b"~~gone~~", b"\x1b[0;9mgone\x1b[0m");
    raw_has(b"***both***", b"\x1b[0;1;3mboth");
    raw_has(b"`code`", b"\x1b[0;36mcode\x1b[0m");
    shows(b"`a*b*c`", b"a*b*c\n");
    shows(b"`` a`b ``", b"a`b\n");
    shows(b"x `` a`b `` y", b"x a`b y\n"); // the padding inside the backticks goes
    shows(b"``x`", b"``x`\n");
    shows(b"a * b * c", b"a * b * c\n");
    shows(b"2*3*4", b"234\n"); // as CommonMark has it: * works inside a word, _ does not
    shows(b"snake_case_name", b"snake_case_name\n");
    shows(b"foo_bar_ baz", b"foo_bar_ baz\n"); // an _ inside a word opens nothing
    shows(b"*not closed", b"*not closed\n");
    shows(b"**not closed", b"**not closed\n");
    shows(b"\\*literal\\*", b"*literal*\n");
    shows(b"back\\slash \\d", b"back\\slash \\d\n");
    shows(b"~single~", b"~single~\n");
    shows(b"[the docs](https://x.org/a)", b"the docs (https://x.org/a)\n");
    raw_has(b"[the docs](https://x.org/a)", b"\x1b[0;4mthe docs\x1b[0;2m (https://x.org/a)");
    shows(b"[https://x.org](https://x.org)", b"https://x.org\n");
    shows(b"![a cat](cat.png)", b"a cat (cat.png)\n");
    shows(b"![](cat.png)", b"image (cat.png)\n");
    shows(b"[not a link] (x)", b"[not a link] (x)\n");
    shows(b"[a [nested] one](u)", b"a [nested] one (u)\n");
    shows(b"[t](f(x))", b"t (f(x))\n");
    shows(b"see <https://x.org> now", b"see https://x.org now\n");
    raw_has(b"<https://x.org>", b"\x1b[0;4mhttps://x.org\x1b[0m");
    shows(b"a <b> c", b"a <b> c\n");
    shows(b"!bang", b"!bang\n");
    // Styles stop at the end of a line.
    raw_has(b"**b**\nnext", b"b\x1b[0m\nnext");
}

#[test]
fn safe() {
    shows(b"a\x1b[2Jb", b"a^[[2Jb\n");
    raw_lacks(b"a\x1b[2Jb", b"\x1b[2J");
    shows(b"bell\x07", b"bell^G\n");
    shows(b"del\x7F", b"del^?\n");
    shows(b"tab\there", b"tab here\n");
    shows(b"c1\xC2\x9B2J", b"c1\xEF\xBF\xBD2J\n");
    shows(b"```\n\x1b]0;title\x07\n```", b"```\n^[]0;title^G\n```\n");
    // md::safe on its own.
    let mut o = Vec::new();
    md::safe(b"x\r\ny\rz\t\x1b\xC2\x85\xC2\xA0", &mut o);
    same(&o, b"x\ny^Mz\t^[\xEF\xBF\xBD\xC2\xA0", b"(safe)");
}

#[test]
fn blocks() {
    shows(b"# Title", b"Title\n");
    raw_has(b"# Title", b"\x1b[0;1;4mTitle");
    raw_has(b"### Sub ###", b"\x1b[0;1mSub\x1b[0m\n");
    shows(b"### Sub ###", b"Sub\n");
    shows(b"## C# is a language", b"C# is a language\n");
    shows(b"#hashtag", b"#hashtag\n");
    shows(b"####### seven", b"####### seven\n");
    shows(b"---", &[rule(39), b"\n".to_vec()].concat());
    shows_w(b"* * *", 25, true, b"------------------------\n");
    shows(b"- one\n- two", "\u{2022} one\n\u{2022} two\n".as_bytes());
    shows_w(b"+ one", 40, true, b"- one\n");
    shows(b"  - nested", "  \u{2022} nested\n".as_bytes());
    shows(b"-not a list", b"-not a list\n");
    shows(b"--", b"--\n"); // two are not a rule
    shows(b"12) aaaa bbbb cccc dddd eeee ffff gggg hhhh", b"12) aaaa bbbb cccc dddd eeee ffff gggg\n    hhhh\n");
    // A hanging indent under the bullet.
    shows(b"- aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii", "\u{2022} aaaa bbbb cccc dddd eeee ffff gggg\n  hhhh iiii\n".as_bytes());
    shows(b"1. first\n10) tenth", b"1. first\n10) tenth\n");
    shows(b"12. aaaa bbbb cccc dddd eeee ffff gggg hhhh", b"12. aaaa bbbb cccc dddd eeee ffff gggg\n    hhhh\n");
    shows(b"2024. was a year", b"2024. was a year\n");
    shows(b"> quoted", "\u{2502} quoted\n".as_bytes());
    shows(b">> twice", "\u{2502} \u{2502} twice\n".as_bytes());
    shows(b"> > spaced", "\u{2502} \u{2502} spaced\n".as_bytes());
    shows_w(b"> q", 40, true, b"| q\n");
    shows(b"> aaaa bbbb cccc dddd eeee ffff gggg hhhh", "\u{2502} aaaa bbbb cccc dddd eeee ffff gggg\n\u{2502} hhhh\n".as_bytes());
    raw_has(b"> **b**", b"\x1b[0;1mb");
}

#[test]
fn fences() {
    shows(b"```c\nint *p = **q;\n# not a heading\n```\nafter *it*", b"```c\nint *p = **q;\n# not a heading\n```\nafter it\n");
    raw_has(b"```\nx\n```", b"\x1b[0;2m```\x1b[0m\n");
    // Long code lines are not wrapped (so they copy whole).
    shows(b"```\naaaa bbbb cccc dddd eeee ffff gggg hhhh iiii\n```", b"```\naaaa bbbb cccc dddd eeee ffff gggg hhhh iiii\n```\n");
    // The closing fence must be as long, and of the same kind.
    shows(b"````\n```\n~~~\n````\n*x*", b"````\n```\n~~~\n````\nx\n");
    shows(b"~~~\n```\n~~~", b"~~~\n```\n~~~\n");
    shows(b"~~~\n```\n*x*\n~~~", b"~~~\n```\n*x*\n~~~\n");
    shows(b"``` not closed\n*x*", b"``` not closed\n*x*\n");
    shows(b"```inline``` code", b"inline code\n");
    shows(b"    ```\n*x*", b"    ```\nx\n");
}

#[test]
fn tables() {
    shows(
        b"| a | bb |\n|---|---:|\n| 1 | 2 |\n| 333 | 4 |\nafter",
        format!("a   \u{2502} bb\n{}\u{253C}{}\n1   \u{2502}  2\n333 \u{2502}  4\nafter\n", "\u{2500}".repeat(4), "\u{2500}".repeat(3)).as_bytes(),
    );
    shows_w(b"a|b\n:-:|-\nxxx|y", 40, true, b" a  | b\n----+--\nxxx | y\n");
    raw_has(b"| **h** |\n|---|\n| `c` |", b"\x1b[0;1mh");
    raw_has(b"| h |\n|---|\n| `c` |", b"\x1b[0;36mc");
    // A pipe inside code or escaped is not a column.
    shows_w(b"x | y\n--|--\n`a|b` | c\\|d", 40, true, b"x   | y\n----+----\na|b | c|d\n");
    // Missing cells are empty; extra ones are dropped.
    shows_w(b"a|b\n-|-\n|1\n1|2|3", 40, true, b"a | b\n--+--\n1 | \n1 | 2\n");
    // The header's cells must match the delimiter row's, or it is no table.
    shows(b"a | b\n---\nc", &[b"a | b\n".to_vec(), rule(39), b"\nc\n".to_vec()].concat());
    shows(b"a | b\n|-x-|---|", b"a | b\n|-x-|---|\n");
    shows(b"a|b\n-|::", b"a|b\n-|::\n"); // a delimiter cell needs a dash
    // Columns are counted in columns, not bytes.
    shows_w(b"\xC3\xA9|b\n-|-\nx|y", 40, true, b"\xC3\xA9 | b\n--+--\nx | y\n");
    // A blank line, or a line with no pipe, ends the table.
    shows_w(b"a|b\n-|-\n1|2\n\n3|4", 40, true, b"a | b\n--+--\n1 | 2\n\n3|4\n");
    shows_w(b"a|b\n-|-\n1|2\nno pipe", 40, true, b"a | b\n--+--\n1 | 2\nno pipe\n");
    // Ending the reply.
    shows_w(b"a|b\n-|-", 40, true, b"a | b\n--+--\n");
    shows_w(b"a|b\n-|-\n", 40, true, b"a | b\n--+--\n\n");
    shows_w(b"a|b\n-|-\n1|2\n", 40, true, b"a | b\n--+--\n1 | 2\n\n");
    // Too wide for the terminal: a record for each row.
    shows_w(
        b"| name | what it does |\n|---|---|\n| edit.c | the line editor, which draws the prompt |\n| md.c | markdown |",
        30,
        true,
        b"name: edit.c\nwhat it does: the line\n  editor, which draws the\n  prompt\n\nname: md.c\nwhat it does: markdown\n",
    );
    raw_has(
        b"| name | what it does is quite long here |\n|---|---|\n| edit.c | the line editor, which draws the prompt |",
        b"\x1b[0;1mname\x1b[0m: edit.c",
    );
    shows_w(
        b"| a long header | and another one | and a third |\n|---|---|---|",
        30,
        true,
        b"a long header | and another\none | and a third\n",
    );
}

#[test]
fn limits() {
    // A line too long to style is shown as it is, still made safe.
    let mut big = vec![b'x'; md::LINE + 99];
    big[0] = b'*';
    big[1] = b'*';
    big[100] = 0x1B;
    let n = big.len();
    big[n - 2] = b'*';
    big[n - 1] = b'*';
    let r = render(&big);
    assert!(r[0] == b'*' && r[1] == b'*');
    assert!(has(&r, b"^["));
    assert!(!r.contains(&0x1B));
    assert_eq!(r.len(), big.len() + 2); // two more: the ^, and the newline
    // A table too big to keep is shown as plain lines.
    let mut tbl = String::from("a|b\n-|-\n");
    for i in 0..md::ROWS + 5 {
        tbl += &format!("{i}|*{i}*\n");
    }
    tbl += "after";
    let p = plain(&render(tbl.as_bytes()));
    assert!(p.starts_with(b"a|b\n-|-\n0|0\n1|1\n"), "{}", show(&p));
    assert!(has(&p, b"\n68|68\nafter\n"), "{}", show(&p));
    // A row with more cells than a table may have.
    shows_w(b"a|b\n-|-\n1|2|3|4|5|6|7|8|9|10|11|12|13|14|15", 40, true, b"a | b\n--+--\n1 | 2\n");
    // Thirteen columns are too many: no table.
    shows(b"a|b|c|d|e|f|g|h|i|j|k|l|m\n-|-|-|-|-|-|-|-|-|-|-|-|-", b"a|b|c|d|e|f|g|h|i|j|k|l|m\n-|-|-|-|-|-|-|-|-|-|-|-|-\n");
    // Cells too big for the table's store.
    let mut wide = b"h|i\n-|-\n".to_vec();
    for _ in 0..4 {
        wide.extend(core::iter::repeat_n(b'y', md::TABLE / 4));
        wide.extend_from_slice(b"|z\n");
    }
    let p = plain(&render_w(&wide, 40, true));
    assert!(p.starts_with(b"h|i\n-|-\nyyy"), "{}", show(&p[..40]));
}

// ---- random replies ----

const PIECES: &[&[u8]] = &[
    b"*", b"**", b"_", b"__", b"~~", b"`", b"``", b"```", b"~~~", b"[", b"]", b"(", b")", b"![", b"<", b">", b"<https://a.b>",
    b"\\", b"\\*", b"#", b"# ", b"## ", b"- ", b"* ", b"+ ", b"1. ", b"12) ", b"> ", b">> ", b"|", b" | ", b"|---|", b"---", b":--",
    b"--:", b" ", b" ", b" ", b"  ", b"\n", b"\n", b"\n\n", b"\r\n", b"\t", b"\x1b", b"\x07", b"\x7F", b"\xC2\x9B", b"\xC3\xA9",
    b"\xE4\xB8\xAD", b"\xF0\x9F\x98\x80", b"\xCC\x81", b"word", b"a", b"bc", b"xyz", b"longerwordthanmost",
    b"0123456789012345678901234567890123456789",
];

/// Every escape is a style (ESC [ digits and semicolons m); every row fits; the end is unstyled.
/// What is wrong, or None.
fn check_output(o: &[u8], width: usize, wrapped: bool) -> Option<String> {
    let mut i = 0;
    let mut last = None;
    while i < o.len() {
        if o[i] != 0x1B {
            i += 1;
            continue;
        }
        last = Some(i);
        let mut j = i + 1;
        if j >= o.len() || o[j] != b'[' {
            return Some("an escape that is not a style".into());
        }
        j += 1;
        while j < o.len() && (o[j].is_ascii_digit() || o[j] == b';') {
            j += 1;
        }
        if j >= o.len() || o[j] != b'm' {
            return Some("an escape that is not a style".into());
        }
        i = j;
    }
    if o.last() != Some(&b'\n') {
        return Some("no newline at the end".into());
    }
    if let Some(l) = last {
        if !o[l..].starts_with(b"\x1b[0m") {
            return Some("left styled".into());
        }
    }
    if wrapped {
        let p = plain(o);
        let w = width.saturating_sub(1).max(20);
        for row in p.split(|&c| c == b'\n') {
            let c: usize = String::from_utf8_lossy(row).chars().map(|ch| edit::cols(ch as u32)).sum();
            if c > w {
                return Some(format!("a row of {c} columns at width {width}"));
            }
        }
    }
    None
}

#[test]
fn random() {
    let mut seed: u32 = 12345;
    let mut rnd = move || {
        seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
        (seed >> 8) & 0xFFFFFF
    };
    let mut fails = 0;
    for _ in 0..40000 {
        let mut input = Vec::new();
        let np = 1 + rnd() % 40;
        for _ in 0..np {
            input.extend_from_slice(PIECES[rnd() as usize % PIECES.len()]);
        }
        let width = 20 + (rnd() % 60) as usize;
        let ascii = rnd() % 2 == 1;
        let o = render_w(&input, width, ascii);
        // Code is not wrapped, and a code line may be wider than the terminal.
        let wrapped = !has(&input, b"```") && !has(&input, b"~~~");
        if let Some(why) = check_output(&o, width, wrapped) {
            eprintln!("FAIL: {why} for:\n{}\n---\n{}", show(&input), show(&plain(&o)));
            fails += 1;
        }
        // Every letter and digit of the reply is shown, in order, unless a table repeats its
        // headers or an image without a description gets one.
        if !input.contains(&b'|') && !has(&input, b"![") {
            let p = plain(&o);
            let mut j = 0;
            for &c in input.iter().filter(|c| c.is_ascii_alphanumeric()) {
                while j < p.len() && p[j] != c {
                    j += 1;
                }
                if j == p.len() {
                    eprintln!("FAIL: letters lost from:\n{}\n---\n{}", show(&input), show(&p));
                    fails += 1;
                    break;
                }
                j += 1;
            }
        }
        if fails > 20 {
            break;
        }
    }
    assert_eq!(fails, 0);
}
