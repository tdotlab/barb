// The core's foundation: byte-string helpers and JSON.
use barb_core::json::{self, Reader, Tok, Writer};
use barb_core::text;

#[test]
fn text_helpers() {
    assert_eq!(text::find(b"hello world", b"o w"), Some(4));
    assert_eq!(text::find(b"hello", b""), Some(0));
    assert_eq!(text::find(b"hello", b"lo!"), None);
    assert_eq!(text::find(b"aab", b"ab"), Some(1));
    assert_eq!(text::find(b"ab", b"abc"), None);
    assert_eq!(text::find(b"xxxxab", b"ab"), Some(4));
    assert_eq!(text::trim(b"  \t x y \r\n"), b"x y");
    assert_eq!(text::trim(b" \n "), b"");
    assert_eq!(text::first_line(b"one\ntwo", 100), b"one");
    assert_eq!(text::first_line(b"abcdef", 3), b"abc");
    let lines: Vec<&[u8]> = text::lines(b"a\n\nb\n").collect();
    assert_eq!(lines, [&b"a"[..], b"", b"b"]);
    assert_eq!(text::lines(b"").count(), 0);
    assert_eq!(text::lines(b"x").count(), 1);

    let mut b = barb_core::cat!("n=", i64::MIN, ' ', u64::MAX, ' ');
    text::pad(&mut b, 42, 6);
    assert_eq!(b, b"n=-9223372036854775808 18446744073709551615     42");
    assert_eq!(barb_core::cat!(0i32, -1i32, &b"x"[..], "y", vec![b'z']), b"0-1xyz");
}

// The kinds of tokens, as the C enum's numbers (Err=0 ... Null=11).
fn tokens(s: &str) -> (bool, String) {
    let mut j = Reader::new(s.as_bytes());
    let mut k = String::new();
    loop {
        let t = j.next();
        k.push((b'0' + t as u8) as char);
        if t == Tok::End {
            return (true, k);
        }
        if t == Tok::Err {
            return (false, k);
        }
    }
}

#[test]
fn reader() {
    let good = [
        "null", "true", "false", "0", "-0", "12", "-1.5e+10", "0.25E-3", "\"\"",
        "\"a\\n\\u00e9\\ud83d\\ude00\"", "[]", "{}", "[1,2,3]", "{\"a\":1}",
        " \n\t{ \"a\" : [ true , null , { \"b\" : \"c\" } ] } \r\n", "[[[[[[[[[[1]]]]]]]]]]",
        "{\"\":\"\"}", "[\"\\/\\\\\\\"\\b\\f\\r\\t\"]",
    ];
    for g in good {
        assert!(json::valid(g.as_bytes()), "should be valid: {g}");
    }
    let bad = [
        "", " ", "nul", "nulll", "truex", "tru", "True", "01", "-", "1.", ".5", "1e", "1e+", "+1",
        "0x10", "--1", "\"abc", "\"a\nb\"", "\"\\x\"", "\"\\u12\"", "\"\\u12g4\"", "\"\\", "[", "]",
        "{", "}", "[1,]", "[,1]", "[1 2]", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{a:1}",
        "{\"a\" 1}", "{1:2}", "[1]]", "{} {}", "1 2", "[1}", "{\"a\":1]", "NaN", "Infinity", "'a'",
        "[\"a\"\"b\"]", "{\"a\":1 \"b\":2}", "\"\x01\"", "true1",
    ];
    for b in bad {
        assert!(!json::valid(b.as_bytes()), "should be invalid: {b:?}");
    }
    // Depth: 64 levels are accepted, 65 refused, with no stack use that grows.
    let deep = |n: usize| "[".repeat(n) + &"]".repeat(n);
    assert!(json::valid(deep(64).as_bytes()));
    assert!(!json::valid(deep(65).as_bytes()));
    let mut j = Reader::new(&[b'['; 100000]);
    while j.next() == Tok::Arr {}
    assert_eq!(j.err(), Some("nested too deeply"));
    assert_eq!(j.depth(), 64);

    assert_eq!(tokens("{\"a\":[1,\"x\",true]}"), (true, "264879531".into()));
    assert!(!tokens("[1,").0);

    // An error sticks, with where it happened.
    let mut j = Reader::new(b"[1,x]");
    while !matches!(j.next(), Tok::Err | Tok::End) {}
    assert_eq!(j.err(), Some("unexpected character"));
    assert_eq!(j.err_pos(), 3);
    assert_eq!(j.next(), Tok::Err);

    // Values come out raw, and capture gives the exact text.
    let mut j = Reader::new(b"  {\"k\": [1, {\"x\": \"y\"}] , \"s\":\"a\\nb\"}");
    assert_eq!(j.next(), Tok::Obj);
    assert_eq!(j.next(), Tok::Key);
    assert_eq!(j.tok(), b"k");
    let t = j.next();
    assert_eq!(t, Tok::Arr);
    assert_eq!(j.capture(t), Some(&b"[1, {\"x\": \"y\"}]"[..]));
    assert_eq!(j.next(), Tok::Key);
    let t = j.next();
    assert_eq!(t, Tok::Str);
    assert_eq!(j.tok(), b"a\\nb");
    assert_eq!(j.capture(t), Some(&b"\"a\\nb\""[..]));
    assert_eq!(j.next(), Tok::ObjEnd);
    assert_eq!(j.next(), Tok::End);

    // Lookup by key.
    let obj = b"{\"a\":1,\"b\":{\"a\":2},\"c\":\"s\",\"a\":3,\"t\":true}";
    assert_eq!(json::get(obj, "a"), Some(&b"1"[..]));
    assert_eq!(json::get(obj, "b"), Some(&b"{\"a\":2}"[..]));
    assert_eq!(json::get(obj, "c"), Some(&b"\"s\""[..]));
    assert_eq!(json::get(obj, "zz"), None);
    assert_eq!(json::get(b"[1]", "a"), None);
    assert_eq!(json::get(b"{\"a\":", "a"), None);
    assert_eq!(json::get(b"{\"a\":1,", "b"), None); // truncated: not found, no crash
    assert_eq!(json::get(obj, "a").and_then(json::as_int), Some(1));
    assert_eq!(json::get(obj, "t").and_then(json::as_bool), Some(true));
    assert_eq!(json::as_bool(b"false"), Some(false));
    assert_eq!(json::as_bool(b"1"), None);
}

#[test]
fn numbers_and_strings() {
    assert_eq!(json::as_int(b"0"), Some(0));
    assert_eq!(json::as_int(b"-0"), Some(0));
    assert_eq!(json::as_int(b"9223372036854775807"), Some(i64::MAX));
    assert_eq!(json::as_int(b"-9223372036854775808"), Some(i64::MIN));
    for bad in [
        &b"9223372036854775808"[..], b"-9223372036854775809", b"99999999999999999999999", b"1.5",
        b"1e3", b"-", b"",
    ] {
        assert_eq!(json::as_int(bad), None, "{:?}", bad);
    }

    let s = |raw: &str| json::as_str(raw.as_bytes());
    assert_eq!(
        s("\"a\\\"b\\\\c\\/d\\n\\t\\u0041\\u00e9\\u20ac\\b\\f\"").unwrap(),
        b"a\"b\\c/d\n\tA\xC3\xA9\xE2\x82\xAC\x08\x0c"
    );
    assert_eq!(s("\"\\ud83d\\ude00\"").unwrap(), b"\xF0\x9F\x98\x80"); // a pair is one character
    assert_eq!(s("\"\\ud83dx\"").unwrap(), b"\xEF\xBF\xBDx"); // a lone high surrogate
    assert_eq!(s("\"\\ud83d\"").unwrap(), b"\xEF\xBF\xBD"); // at the very end
    assert_eq!(s("\"\\ude00\"").unwrap(), b"\xEF\xBF\xBD"); // a lone low surrogate
    assert_eq!(s("\"\\ud83d\\u0041\"").unwrap(), b"\xEF\xBF\xBDA"); // high then plain
    assert_eq!(s("\"\\u0000\"").unwrap(), b"\0"); // an embedded NUL survives
    assert_eq!(s("\"\\u07ff\\uffff\"").unwrap(), "\u{7ff}\u{ffff}".as_bytes());
    assert_eq!(s("abc"), None);
    assert_eq!(s("\""), None);
    assert_eq!(s("\"\"").unwrap(), b"");
    // Malformed escapes the reader would refuse do not crash the decoder.
    assert_eq!(json::unescape(b"a\\"), b"a\\");
    assert_eq!(json::unescape(b"\\u12"), b"\\u12");
}

fn write_str(b: &[u8]) -> Vec<u8> {
    let mut w = Writer::new();
    w.str(b);
    w.out
}

#[test]
fn writer() {
    let mut w = Writer::new();
    w.obj();
    w.key("s");
    w.str("a\"b\\c\n\r\t\x01\x1f/");
    w.key("n");
    w.int(-42);
    w.key("a");
    w.arr();
    w.int(1);
    w.bool(false);
    w.null();
    w.obj();
    w.obj_end();
    w.arr_end();
    w.key("r");
    w.raw(b"{\"x\":[1,2]}");
    w.obj_end();
    assert!(!w.bad);
    assert_eq!(
        String::from_utf8(w.out.clone()).unwrap(),
        "{\"s\":\"a\\\"b\\\\c\\n\\r\\t\\u0001\\u001f/\",\"n\":-42,\"a\":[1,false,null,{}],\"r\":{\"x\":[1,2]}}"
    );
    assert!(json::valid(&w.out));

    // Valid UTF-8 passes through; every kind of bad sequence becomes U+FFFD.
    let cases: &[(&[u8], &[u8])] = &[
        (b"\xC3\xA9", b"\"\xC3\xA9\""),
        (b"\xE2\x82\xAC", b"\"\xE2\x82\xAC\""),
        (b"\xF0\x9F\x98\x80", b"\"\xF0\x9F\x98\x80\""),
        (b"\xF4\x8F\xBF\xBF", b"\"\xF4\x8F\xBF\xBF\""), // U+10FFFF, the last one
        (b"\x80", b"\"\\ufffd\""),                       // a stray continuation byte
        (b"\xC0\x80", b"\"\\ufffd\\ufffd\""),            // overlong NUL
        (b"\xC1\xBF", b"\"\\ufffd\\ufffd\""),
        (b"\xE0\x80\x80", b"\"\\ufffd\\ufffd\\ufffd\""),
        (b"\xED\xA0\x80", b"\"\\ufffd\\ufffd\\ufffd\""), // a surrogate
        (b"\xF0\x80\x80\x80", b"\"\\ufffd\\ufffd\\ufffd\\ufffd\""), // overlong
        (b"\xF4\x90\x80\x80", b"\"\\ufffd\\ufffd\\ufffd\\ufffd\""), // past U+10FFFF
        (b"\xF8\x88\x80\x80\x80", b"\"\\ufffd\\ufffd\\ufffd\\ufffd\\ufffd\""),
        (b"\xE2\x82", b"\"\\ufffd\\ufffd\""), // cut off
        (b"ab\xFFcd", b"\"ab\\ufffdcd\""),
        (b"\xC3", b"\"\\ufffd\""),
        (b"\x7f", b"\"\x7f\""),
    ];
    for (i, (input, want)) in cases.iter().enumerate() {
        let got = write_str(input);
        assert_eq!(&got[..], *want, "utf-8 case {i}");
        assert!(json::valid(&got));
    }

    // Round trip: any bytes at all, written then read back, give valid UTF-8 JSON that parses,
    // and the decoded text is itself valid UTF-8 (a second round replaces nothing).
    let mut seed: u32 = 12345;
    for round in 0..300 {
        let n = round % 290;
        let buf: Vec<u8> = (0..n)
            .map(|_| {
                seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
                (seed >> 16) as u8
            })
            .collect();
        let got = write_str(&buf);
        assert!(json::valid(&got));
        let back = json::as_str(&got).unwrap();
        assert!(core::str::from_utf8(&back).is_ok());
        let back2 = json::as_str(&write_str(&back)).unwrap();
        assert_eq!(back, back2);
        // Valid text survives exactly.
        let s = String::from_utf8_lossy(&buf).into_owned();
        assert_eq!(json::as_str(&write_str(s.as_bytes())).unwrap(), s.as_bytes());
    }

    // Misuse is noticed, not crashed on.
    let mut w = Writer::new();
    w.obj_end();
    assert!(w.bad);
    let mut w = Writer::new();
    for _ in 0..65 {
        w.arr();
    }
    assert!(w.bad);
}

// Feed the reader and the lookups bytes that are almost JSON: whatever it decides, it must not
// panic (a read past the input would).
#[test]
fn fuzz() {
    let seeds = [
        "{\"id\":\"msg_01\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"hi \\u00e9\"},\
         {\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"bash\",\"input\":{\"command\":\"ls -la\",\"timeout_ms\":5000}}],\
         \"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}",
        "[1,2.5e3,-0,\"x\",[true,false,null],{\"a\":{\"b\":[]}}]",
    ];
    let mut seed: u32 = 99;
    let mut step = || {
        seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
        seed
    };
    let mut ran = 0;
    for s in seeds {
        for _ in 0..20000 {
            let mut copy = s.as_bytes().to_vec();
            let edits = 1 + (step() >> 16) % 4;
            let mut used = copy.len();
            for _ in 0..edits {
                let at = (step() >> 8) as usize % used.max(1);
                let r = step();
                match (r >> 16) % 4 {
                    0 => copy[at] = (r >> 4) as u8,
                    1 => used = at, // truncate
                    2 => copy[at] = b"{}[]\",:\\ 0e-.\0"[((r >> 5) % 14) as usize], // the C table's NUL too
                    _ => {
                        if at + 1 < used {
                            copy[at] = copy[at + 1];
                        }
                    }
                }
            }
            let input = &copy[..used];
            let mut j = Reader::new(input);
            for _ in 0..10000 {
                if matches!(j.next(), Tok::Err | Tok::End) {
                    break;
                }
            }
            if let Some(v) = json::get(input, "content") {
                let _ = json::as_str(v);
            }
            if let Some(v) = json::get(input, "usage") {
                let _ = json::get(v, "input_tokens").and_then(json::as_int);
            }
            json::valid(input);
            let _ = barb_core::api::parse(input);
            ran += 1;
        }
    }
    assert_eq!(ran, 40000);
}
