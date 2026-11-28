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

/// Which conversation a run takes up.
#[derive(Debug, Default, PartialEq)]
pub enum Start {
    /// A new one.
    #[default]
    New,
    /// The one saved last in this directory.
    Continue,
    /// A saved one, by name or path.
    Resume(OsString),
}

#[derive(Debug, Default, PartialEq)]
pub struct Args {
    pub prompt: Option<OsString>,
    pub model: Option<OsString>,
    pub cwd: Option<OsString>,
    pub yes: bool,
    pub max_turns: i32,
    pub no_instructions: bool,
    pub start: Start,
    pub no_save: bool,
    /// List the saved conversations and exit.
    pub sessions: bool,
    pub verbose: bool,
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
        } else if is("-c") || is("--continue") {
            a.start = Start::Continue;
        } else if (is("-r") || is("--resume")) && val.is_some() {
            a.start = Start::Resume(val.unwrap());
            i += 1;
        } else if is("--no-save") {
            a.no_save = true;
        } else if is("--sessions") {
            a.sessions = true;
        } else if is("-v") || is("--verbose") {
            a.verbose = true;
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

// ---- saved conversations ----
//
// Each one is a file in .barb/sessions in the working directory, named for when it started
// (YYYYMMDD-HHMMSS.json); the format is in docs/SESSIONS.md. .barb/ gets a .gitignore of "*"
// when the first one is saved, so it stays out of the project's git.

pub const SESSIONS: &str = ".barb/sessions";

/// Where this run saves its conversation (empty: it does not), and the saved one it takes up.
#[derive(Debug, Default, PartialEq)]
pub struct Plan {
    pub path: Vec<u8>,
    /// The file's contents, when resuming.
    pub data: Option<Vec<u8>>,
    /// The model it was saved with.
    pub model: Vec<u8>,
}

fn session_files(dir: &std::path::Path) -> Vec<(std::time::SystemTime, String)> {
    let mut v = Vec::new();
    let Ok(rd) = std::fs::read_dir(dir) else { return v };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if !name.ends_with(".json") || name.starts_with('.') {
            continue;
        }
        let Ok(md) = e.metadata() else { continue };
        if md.is_file() {
            v.push((md.modified().unwrap_or(std::time::UNIX_EPOCH), name));
        }
    }
    v.sort_by(|a, b| b.cmp(a)); // newest first
    v
}

/// The name a new conversation started at this local time gets, if `taken` says it is free:
/// YYYYMMDD-HHMMSS, then with -b, -c, ... after it.
pub fn new_name(t: (i32, i32, i32, i32, i32), sec: u32, taken: impl Fn(&str) -> bool) -> String {
    let base = format!("{:04}{:02}{:02}-{:02}{:02}{:02}", t.0, t.1, t.2, t.3, t.4, sec % 60);
    if !taken(&base) {
        return base;
    }
    for c in b'b'..=b'z' {
        let n = format!("{}-{}", base, c as char);
        if !taken(&n) {
            return n;
        }
    }
    format!("{}-{}", base, std::process::id())
}

/// The file `arg` names: a path if it has a slash in it, else a name in .barb/sessions (with or
/// without .json).
pub fn session_file(arg: &str) -> String {
    if arg.contains('/') || (cfg!(windows) && arg.contains('\\')) {
        return arg.to_string();
    }
    let n = if arg.ends_with(".json") { arg.to_string() } else { format!("{}.json", arg) };
    format!("{}/{}", SESSIONS, n)
}

/// What this run does about saved conversations, given the local time (for a new one's name).
pub fn plan(a: &Args, now: (i32, i32, i32, i32, i32), sec: u32) -> Result<Plan, String> {
    let dir = std::path::Path::new(SESSIONS);
    let path = match &a.start {
        Start::New => {
            if a.no_save {
                return Ok(Plan::default());
            }
            let name = new_name(now, sec, |n| dir.join(format!("{}.json", n)).exists());
            return Ok(Plan { path: format!("{}/{}.json", SESSIONS, name).into_bytes(), ..Plan::default() });
        }
        Start::Continue => match session_files(dir).into_iter().next() {
            Some((_, name)) => format!("{}/{}", SESSIONS, name),
            None => return Err("there is no saved conversation in this directory to continue".into()),
        },
        Start::Resume(r) => session_file(&r.to_string_lossy()),
    };
    let data = std::fs::read(&path).map_err(|e| format!("cannot read {}: {}", path, strerror(&e)))?;
    let s = barb_core::api::load_session(&data).ok_or_else(|| format!("{} is not a saved Barb conversation", path))?;
    Ok(Plan { path: if a.no_save { Vec::new() } else { path.into_bytes() }, data: Some(data), model: s.model })
}

/// After a file was written: if it was a saved conversation, keep .barb/ out of git.
pub fn wrote(path: &[u8]) {
    if !path.starts_with(SESSIONS.as_bytes()) {
        return;
    }
    let gi = std::path::Path::new(".barb/.gitignore");
    if !gi.exists() {
        let _ = std::fs::write(gi, "# Barb's saved conversations; not part of the project.\n*\n");
    }
}

/// What --sessions prints: the newest saved conversations here, each with its size and how it
/// began.
pub fn list_sessions() -> String {
    let files = session_files(std::path::Path::new(SESSIONS));
    if files.is_empty() {
        return "No saved conversations in this directory.\n".into();
    }
    let mut out = String::new();
    for (_, name) in files.iter().take(20) {
        let stem = name.strip_suffix(".json").unwrap_or(name);
        let s = std::fs::read(format!("{}/{}", SESSIONS, name)).ok().and_then(|d| barb_core::api::load_session(&d));
        let Some(s) = s else {
            out.push_str(&format!("{}  (unreadable)\n", stem));
            continue;
        };
        let first = s.tr.msgs.iter().filter(|m| !m.assistant).flat_map(|m| m.blocks.iter()).find_map(|b| match b {
            barb_core::api::Block::Text(t) if !t.starts_with(b"[barb]") => Some(t.clone()),
            _ => None,
        });
        let first = first.map(|t| barb_core::text::first_line(&t, 60).to_vec()).unwrap_or_default();
        out.push_str(&format!("{}  {:>4} messages  {}\n", stem, s.tr.msgs.len(), String::from_utf8_lossy(&first)));
    }
    if files.len() > 20 {
        out.push_str(&format!("({} older ones not shown)\n", files.len() - 20));
    }
    out
}

/// The line shown when an interactive run ends with its conversation saved.
pub fn saved_line(path: &[u8]) -> Option<String> {
    let p = String::from_utf8_lossy(path).into_owned();
    if p.is_empty() || !std::path::Path::new(&p).is_file() {
        return None;
    }
    let name = p.rsplit('/').next().unwrap_or(&p);
    let name = name.strip_suffix(".json").unwrap_or(name);
    Some(format!("Saved as {}; barb -r {} takes it up again.\n", name, name))
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
    fn session_options() {
        let run = |v: &[&str]| match parse(&os(v)) {
            Parsed::Run(a) => a,
            p => panic!("{:?}", p),
        };
        assert_eq!(run(&[]).start, Start::New);
        assert_eq!(run(&["-c"]).start, Start::Continue);
        assert_eq!(run(&["--continue"]).start, Start::Continue);
        assert_eq!(run(&["-r", "x"]).start, Start::Resume("x".into()));
        assert_eq!(run(&["--resume", "a/b.json"]).start, Start::Resume("a/b.json".into()));
        assert_eq!(parse(&os(&["-r"])), Parsed::Bad("-r".into()), "a name is needed");
        let a = run(&["--no-save", "--sessions", "-v"]);
        assert!(a.no_save && a.sessions && a.verbose);
        assert!(run(&["--verbose"]).verbose);
        let a = run(&[]);
        assert!(!a.no_save && !a.sessions && !a.verbose);
    }

    #[test]
    fn session_names() {
        let t = (2026, 11, 27, 9, 5);
        assert_eq!(new_name(t, 7, |_| false), "20261127-090507");
        assert_eq!(new_name(t, 67, |_| false), "20261127-090507", "seconds wrap");
        assert_eq!(new_name(t, 7, |n| n == "20261127-090507"), "20261127-090507-b");
        assert_eq!(new_name(t, 7, |n| n == "20261127-090507" || n.ends_with("-b")), "20261127-090507-c");
        assert_eq!(session_file("20261127-090507"), ".barb/sessions/20261127-090507.json");
        assert_eq!(session_file("x.json"), ".barb/sessions/x.json");
        assert_eq!(session_file("d/x.json"), "d/x.json", "a path is taken as it is");
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
