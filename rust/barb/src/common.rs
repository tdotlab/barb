//! What the two platforms' programs share: the command line, the events waiting for the agent, a
//! command's kept output, and talking to curl.

use barb_core::agent::{Answer, Event, Exit, Fs};
use std::ffi::{OsStr, OsString};

pub const VERSION: &str = "0.2.0";
pub const DEFAULT_MODEL: &str = "claude-sonnet-5-5";

pub const NOT_ALLOWED: &[u8] =
    b"(not allowed: there is no one to ask. Run with --yes to let barb write files and run commands unasked.)";

// ---- events waiting for the agent ----

/// An event that owns its bytes, so it can wait in the queue. The agent is never called while
/// its actions are being carried out: what they produce waits here.
#[derive(Debug, PartialEq)]
pub enum Ev {
    Input(Vec<u8>),
    Eof,
    Http { id: i64, status: i32, body: Vec<u8>, note: Vec<u8> },
    File { id: i64, status: Fs, data: Vec<u8>, note: Vec<u8> },
    Answer { id: i64, answer: Answer },
    Proc { id: i64, exit: Exit, out: Vec<u8>, ms: i64, note: Vec<u8> },
    Timer { id: i64 },
    Interrupt,
}

impl Ev {
    pub fn event(&self) -> Event<'_> {
        match self {
            Ev::Input(l) => Event::Input(l),
            Ev::Eof => Event::Eof,
            Ev::Http { id, status, body, note } => Event::Http { id: *id, status: *status, body, note },
            Ev::File { id, status, data, note } => Event::File { id: *id, status: *status, data, note },
            Ev::Answer { id, answer } => Event::Answer { id: *id, answer: *answer },
            Ev::Proc { id, exit, out, ms, note } => Event::Proc { id: *id, exit: *exit, out, ms: *ms, note },
            Ev::Timer { id } => Event::Timer { id: *id },
            Ev::Interrupt => Event::Interrupt,
        }
    }

    /// An operation that could not be started.
    pub fn failed_http(id: i64, why: &str) -> Ev {
        Ev::Http { id, status: 0, body: Vec::new(), note: why.as_bytes().to_vec() }
    }

    pub fn failed_proc(id: i64, why: &str) -> Ev {
        Ev::Proc { id, exit: Exit::Failed, out: Vec::new(), ms: 0, note: why.as_bytes().to_vec() }
    }

    pub fn file(id: i64, status: Fs, note: &str) -> Ev {
        Ev::File { id, status, data: Vec::new(), note: note.as_bytes().to_vec() }
    }
}

// ---- the command line ----

#[derive(Debug, Default, PartialEq)]
pub struct Args {
    pub prompt: Option<OsString>,
    pub model: Option<OsString>,
    pub cwd: Option<OsString>,
    pub yes: bool,
    pub max_turns: i32,
    pub no_instructions: bool,
}

#[derive(Debug, PartialEq)]
pub enum Parsed {
    Run(Args),
    Help,
    Version,
    /// The option that was wrong.
    Bad(OsString),
}

/// The arguments after the program's name.
pub fn parse(argv: &[OsString]) -> Parsed {
    let mut a = Args::default();
    let mut i = 0;
    while i < argv.len() {
        let s = argv[i].as_os_str();
        let val = argv.get(i + 1).cloned();
        let is = |x: &str| s == OsStr::new(x);
        if is("-h") || is("--help") {
            return Parsed::Help;
        } else if is("-V") || is("--version") {
            return Parsed::Version;
        } else if is("-y") || is("--yes") {
            a.yes = true;
        } else if is("--no-instructions") {
            a.no_instructions = true;
        } else if (is("-p") || is("--prompt")) && val.is_some() {
            a.prompt = val;
            i += 1;
        } else if (is("-m") || is("--model")) && val.is_some() {
            a.model = val;
            i += 1;
        } else if (is("-C") || is("--cwd")) && val.is_some() {
            a.cwd = val;
            i += 1;
        } else if is("--max-turns") && val.is_some() {
            a.max_turns = atoi(&val.unwrap().to_string_lossy());
            i += 1;
        } else {
            return Parsed::Bad(argv[i].clone());
        }
        i += 1;
    }
    Parsed::Run(a)
}

/// As C's atoi: leading blanks, a sign, digits; 0 when there are none.
pub fn atoi(s: &str) -> i32 {
    let b = s.trim_start_matches([' ', '\t', '\n', '\r', '\x0b', '\x0c']).as_bytes();
    let (neg, b) = match b.first() {
        Some(b'-') => (true, &b[1..]),
        Some(b'+') => (false, &b[1..]),
        _ => (false, b),
    };
    let mut v: i64 = 0;
    for &c in b {
        if !c.is_ascii_digit() {
            break;
        }
        v = (v * 10 + (c - b'0') as i64).min(i32::MAX as i64 + 1);
    }
    let v = if neg { -v } else { v };
    v.clamp(i32::MIN as i64, i32::MAX as i64) as i32
}

/// The API's address without trailing slashes, and whether to warn that it is plain http to
/// somewhere other than this machine.
pub fn base_url(env: Option<String>) -> (String, bool) {
    let mut b = match env {
        Some(s) if !s.is_empty() => s,
        _ => "https://api.anthropic.com".to_string(),
    };
    while b.ends_with('/') {
        b.pop();
    }
    let warn = match b.get(..7) {
        Some(s) if s.eq_ignore_ascii_case("http://") => !is_this_machine(host(&b[7..])),
        _ => false,
    };
    (b, warn)
}

/// The host of what follows `scheme://`: no user, no port, no path. (`localhost.example.com` is
/// someone else's machine, and so is `localhost@example.com`.)
fn host(rest: &str) -> &str {
    let auth = &rest[..rest.find(['/', '?', '#']).unwrap_or(rest.len())];
    let h = &auth[auth.rfind('@').map_or(0, |i| i + 1)..];
    if h.starts_with('[') {
        &h[..h.find(']').map_or(h.len(), |i| i + 1)]
    } else {
        &h[..h.find(':').unwrap_or(h.len())]
    }
}

fn is_this_machine(h: &str) -> bool {
    h.eq_ignore_ascii_case("localhost") || h == "127.0.0.1" || h == "[::1]"
}

pub const PLAIN_HTTP_WARNING: &str =
    "barb: warning: ANTHROPIC_BASE_URL is plain http, so your API key will cross the network unencrypted.\n";

/// The system's message for an error, without std's " (os error N)".
#[cfg_attr(windows, allow(dead_code))] // Windows asks FormatMessageW itself (win32::sys::why)
pub fn strerror(e: &std::io::Error) -> String {
    let s = e.to_string();
    match s.rfind(" (os error ") {
        Some(i) if s.ends_with(')') => s[..i].to_string(),
        _ => s,
    }
}

/// A date as YYYY-MM-DD.
pub fn ymd(year: i32, mon: i32, mday: i32) -> String {
    format!("{:04}-{:02}-{:02}", year, mon, mday)
}

/// A time as HH:MM.
pub fn hhmm(hour: i32, min: i32) -> Vec<u8> {
    format!("{:02}:{:02}", hour, min).into_bytes()
}

// ---- a command's output ----

pub const HEAD_KEEP: usize = 1 << 20;
pub const TAIL_KEEP: usize = 1 << 20;

/// What a command printed: the first part as it came and a ring of the most recent bytes, with
/// the middle of a huge output dropped (and said so).
pub struct Output {
    head_keep: usize,
    tail_keep: usize,
    head: Vec<u8>,
    ring: Vec<u8>,
    pos: usize,
    total: u64,
}

impl Default for Output {
    fn default() -> Output {
        Output::new(HEAD_KEEP, TAIL_KEEP)
    }
}

impl Output {
    pub fn new(head_keep: usize, tail_keep: usize) -> Output {
        Output { head_keep, tail_keep, head: Vec::new(), ring: Vec::new(), pos: 0, total: 0 }
    }

    pub fn add(&mut self, mut p: &[u8]) {
        self.total += p.len() as u64;
        if self.head.len() < self.head_keep {
            let take = (self.head_keep - self.head.len()).min(p.len());
            self.head.extend_from_slice(&p[..take]);
            p = &p[take..];
        }
        if p.is_empty() {
            return;
        }
        if self.ring.is_empty() {
            self.ring = vec![0; self.tail_keep];
        }
        let n = self.tail_keep;
        if p.len() >= n {
            p = &p[p.len() - n..];
        }
        let at = self.pos % n;
        let first = p.len().min(n - at);
        self.ring[at..at + first].copy_from_slice(&p[..first]);
        self.ring[..p.len() - first].copy_from_slice(&p[first..]);
        self.pos = (at + p.len()) % n;
    }

    /// The output as one string: the head, a note if the middle was dropped, the tail.
    pub fn take(self) -> Vec<u8> {
        let rest = self.total - self.head.len() as u64;
        let tail = rest.min(self.tail_keep as u64) as usize;
        let dropped = rest - tail as u64;
        let mut out = self.head;
        if dropped > 0 {
            out.extend_from_slice(format!("\n[... {} bytes of output dropped here ...]\n", dropped).as_bytes());
        }
        let n = self.tail_keep;
        for i in 0..tail {
            out.push(self.ring[(self.pos + n - tail + i) % n]);
        }
        out
    }
}

// ---- curl ----

/// One line of a curl config file: key = "value", with quotes and backslashes escaped and line
/// breaks dropped, so nothing in a value can start another line.
pub fn cfg_quoted(b: &mut Vec<u8>, key: &str, value: &[u8]) {
    b.extend_from_slice(key.as_bytes());
    b.extend_from_slice(b" = \"");
    for &c in value {
        if c == b'"' || c == b'\\' {
            b.push(b'\\');
        }
        if c == b'\n' || c == b'\r' {
            continue;
        }
        b.push(c);
    }
    b.extend_from_slice(b"\"\n");
}

/// What curl reads on its standard input: the address, the headers and the key, and where the
/// body is. The key is never on a command line, where anyone could read it from the process list.
pub fn curl_config(url: &[u8], key: &[u8], datafile: &[u8]) -> Vec<u8> {
    let mut keyhdr = b"x-api-key: ".to_vec();
    keyhdr.extend_from_slice(key);
    let mut df = b"@".to_vec();
    df.extend_from_slice(datafile);
    let mut cfg = Vec::with_capacity(512);
    cfg_quoted(&mut cfg, "url", url);
    cfg_quoted(&mut cfg, "request", b"POST");
    cfg_quoted(&mut cfg, "header", b"content-type: application/json");
    cfg_quoted(&mut cfg, "header", b"anthropic-version: 2023-06-01");
    cfg_quoted(&mut cfg, "header", &keyhdr);
    cfg_quoted(&mut cfg, "data-binary", &df);
    wipe(&mut keyhdr);
    cfg
}

/// Overwrite what held the key before it is freed.
pub fn wipe(b: &mut Vec<u8>) {
    for c in b.iter_mut() {
        // SAFETY: c is a valid, exclusive reference; the volatile write keeps the store from
        // being removed as dead.
        unsafe { core::ptr::write_volatile(c, 0) };
    }
}

/// curl's options, apart from the config on its standard input. It prints the body and then
/// the status on a line of its own.
pub fn curl_args(newline: &'static str) -> [&'static str; 11] {
    ["-sS", "--stderr", "-", "--connect-timeout", "30", "--max-time", "1200", "-K", "-", "-w", newline]
}

pub const HTTP_MAX: usize = 64 << 20;

fn trim_end_ws(mut s: &[u8]) -> &[u8] {
    while let Some(&c) = s.last() {
        if c == b'\n' || c == b'\r' || c == b' ' {
            s = &s[..s.len() - 1];
        } else {
            break;
        }
    }
    s
}

/// What curl's exit code and output mean: the status, the body and a note. A curl that failed
/// printed its reason (with --stderr -), followed by the status line, "000" after a failure,
/// which is not part of the reason.
pub fn curl_reply(code: i32, out: &[u8]) -> (i32, Vec<u8>, Vec<u8>) {
    if code != 0 {
        let mut s = trim_end_ws(out);
        if s.ends_with(b"000") {
            s = &s[..s.len() - 3];
        }
        let s = trim_end_ws(s);
        let note = if s.is_empty() {
            format!("curl failed with exit code {}", code).into_bytes()
        } else {
            s[..s.len().min(300)].to_vec()
        };
        return (0, Vec::new(), note);
    }
    let cut = out.iter().rposition(|&c| c == b'\n').map(|i| i + 1).unwrap_or(0);
    let status = if cut > 0 { atoi(&String::from_utf8_lossy(&out[cut..])) } else { 0 };
    let body = if cut > 0 { out[..cut - 1].to_vec() } else { Vec::new() };
    (status, body, Vec::new())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn os(v: &[&str]) -> Vec<OsString> {
        v.iter().map(OsString::from).collect()
    }

    #[test]
    fn options() {
        assert_eq!(parse(&os(&[])), Parsed::Run(Args::default()));
        assert_eq!(parse(&os(&["-x"])), Parsed::Bad("-x".into()));
        assert_eq!(parse(&os(&["-p"])), Parsed::Bad("-p".into()), "a value is needed");
        assert_eq!(parse(&os(&["-y", "-h", "-x"])), Parsed::Help);
        assert_eq!(parse(&os(&["--version"])), Parsed::Version);
        let a = match parse(&os(&["-p", "do it", "-m", "m1", "-C", "/d", "--max-turns", "7", "--yes", "--no-instructions"])) {
            Parsed::Run(a) => a,
            p => panic!("{:?}", p),
        };
        assert_eq!(a.prompt, Some("do it".into()));
        assert_eq!(a.model, Some("m1".into()));
        assert_eq!(a.cwd, Some("/d".into()));
        assert_eq!(a.max_turns, 7);
        assert!(a.yes && a.no_instructions);
        match parse(&os(&["--prompt", "-h"])) {
            Parsed::Run(a) => assert_eq!(a.prompt, Some("-h".into()), "a value is taken as it is"),
            p => panic!("{:?}", p),
        }
    }

    #[test]
    fn atoi_is_cs() {
        assert_eq!(atoi("12"), 12);
        assert_eq!(atoi("  -3x"), -3);
        assert_eq!(atoi("+4"), 4);
        assert_eq!(atoi("x"), 0);
        assert_eq!(atoi(""), 0);
        assert_eq!(atoi("99999999999"), i32::MAX);
        assert_eq!(atoi("-99999999999"), i32::MIN);
        assert_eq!(atoi("200\n"), 200);
    }

    #[test]
    fn base() {
        assert_eq!(base_url(None), ("https://api.anthropic.com".into(), false));
        assert_eq!(base_url(Some(String::new())), ("https://api.anthropic.com".into(), false));
        assert_eq!(base_url(Some("https://x.test//".into())), ("https://x.test".into(), false));
        assert_eq!(base_url(Some("http://localhost:8080/".into())), ("http://localhost:8080".into(), false));
        assert_eq!(base_url(Some("http://127.0.0.1:1".into())).1, false);
        assert_eq!(base_url(Some("http://[::1]:1".into())).1, false);
        assert_eq!(base_url(Some("http://example.com".into())).1, true);
        assert_eq!(base_url(Some("HTTP://LocalHost:9/v1".into())).1, false);
        assert_eq!(base_url(Some("http://localhost.evil.com".into())).1, true, "a name that starts like localhost");
        assert_eq!(base_url(Some("http://localhost@evil.com".into())).1, true, "localhost as the user");
        assert_eq!(base_url(Some("http://127.0.0.1.evil.com:80".into())).1, true);
        assert_eq!(base_url(Some("http://user@127.0.0.1:8080/x".into())).1, false);
    }

    #[test]
    fn errors_read_as_cs() {
        let e = std::io::Error::from_raw_os_error(2);
        let s = strerror(&e);
        assert!(!s.contains("os error") && !s.is_empty(), "{}", s);
        assert_eq!(strerror(&std::io::Error::other("plain")), "plain");
    }

    #[test]
    fn small_output_is_kept_whole() {
        let mut o = Output::new(4, 4);
        o.add(b"abc");
        assert_eq!(o.take(), b"abc");
        let mut o = Output::new(4, 4);
        o.add(b"abcdef");
        o.add(b"gh");
        assert_eq!(o.take(), b"abcdefgh", "head and ring together hold it all");
    }

    #[test]
    fn the_middle_of_a_big_output_is_dropped() {
        for chunk in [1, 2, 3, 5, 7, 100] {
            let all: Vec<u8> = (0..60u8).map(|i| b'a' + i % 26).collect();
            let mut o = Output::new(5, 7);
            for c in all.chunks(chunk) {
                o.add(c);
            }
            let mut want = all[..5].to_vec();
            want.extend_from_slice(b"\n[... 48 bytes of output dropped here ...]\n");
            want.extend_from_slice(&all[53..]);
            assert_eq!(String::from_utf8_lossy(&o.take()), String::from_utf8_lossy(&want), "chunks of {}", chunk);
        }
    }

    #[test]
    fn config_quotes_values() {
        let mut b = Vec::new();
        cfg_quoted(&mut b, "url", b"a\"b\\c\nd\re");
        assert_eq!(b, b"url = \"a\\\"b\\\\cde\"\n");
        let c = curl_config(b"http://h/v1", b"k\"ey", b"/tmp/f");
        let c = String::from_utf8(c).unwrap();
        assert_eq!(
            c,
            "url = \"http://h/v1\"\nrequest = \"POST\"\nheader = \"content-type: application/json\"\n\
             header = \"anthropic-version: 2023-06-01\"\nheader = \"x-api-key: k\\\"ey\"\ndata-binary = \"@/tmp/f\"\n"
        );
    }

    #[test]
    fn replies() {
        assert_eq!(curl_reply(0, b"{\"a\":1}\n200"), (200, b"{\"a\":1}".to_vec(), vec![]));
        assert_eq!(curl_reply(0, b"line1\nline2\n429"), (429, b"line1\nline2".to_vec(), vec![]));
        assert_eq!(curl_reply(0, b"\n500"), (500, vec![], vec![]));
        assert_eq!(curl_reply(0, b"junk"), (0, vec![], vec![]), "no status line");
        assert_eq!(curl_reply(7, b"curl: (7) Failed to connect\n000"), (0, vec![], b"curl: (7) Failed to connect".to_vec()));
        assert_eq!(curl_reply(28, b"\n000 \r\n"), (0, vec![], b"curl failed with exit code 28".to_vec()));
        assert_eq!(curl_reply(-1, b""), (0, vec![], b"curl failed with exit code -1".to_vec()));
        let long = vec![b'x'; 400];
        assert_eq!(curl_reply(6, &long).2.len(), 300);
    }

    #[test]
    fn the_key_is_wiped() {
        let mut cfg = curl_config(b"https://x/v1/messages", b"sk-secret", b"/tmp/body");
        let n = cfg.len();
        wipe(&mut cfg);
        assert_eq!(cfg, vec![0; n], "every byte zeroed, the length kept");
    }
}
