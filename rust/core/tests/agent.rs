// Scenario tests for the agent: a scripted model and a fake world. The world carries out the
// agent's actions the way a platform would, feeds the answers back as events, and checks every
// request the agent would send against the rules the real API enforces.
use barb_core::agent::{Action, Agent, Answer, Config, Event, Exit, Fs, Style};
use barb_core::json::{self, Reader, Tok};
use std::collections::VecDeque;

// ---- the fake world ----

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
    Http,
    Read,
    Write,
    Spawn,
    Timer,
    Ask,
}

#[derive(Debug)]
struct Op {
    id: i64,
    kind: Kind,
    bg: bool,
    n: i64,
    /// The path (Read, Write) or command (Spawn).
    a: String,
    /// What was written (Write), or the log (Spawn).
    c: String,
    cancelled: bool,
}

enum Ev {
    Input(String),
    Eof,
    Http { id: i64, status: i32, body: Vec<u8>, note: String },
    File { id: i64, status: Fs, data: Vec<u8> },
    Answer { id: i64, answer: Answer },
    Proc { id: i64, exit: Exit, out: Vec<u8>, ms: i64, note: String },
    Timer(i64),
    Interrupt,
}

const PROC_TIMEDOUT: i32 = 1;
const PROC_SIGNALED: i32 = 2;
const PROC_FAILED: i32 = 4;

fn exit_of(code: i32, flags: i32) -> Exit {
    if flags & PROC_FAILED != 0 {
        Exit::Failed
    } else if flags & PROC_TIMEDOUT != 0 {
        Exit::TimedOut
    } else if flags & PROC_SIGNALED != 0 {
        Exit::Signaled
    } else {
        Exit::Code(code)
    }
}

struct World {
    g: Agent,
    q: VecDeque<Ev>,
    replies: Vec<(String, i32)>,
    irep: usize,
    unexpected_requests: usize,
    /// path → contents; None for a directory.
    files: Vec<(String, Option<Vec<u8>>)>,
    /// A command containing `.0` prints `.1` and exits with `.2`, flags `.3`.
    procs: Vec<(&'static str, Vec<u8>, i32, i32)>,
    says: Vec<(Style, String)>,
    usage: i64,
    bodies: Vec<String>,
    ops: Vec<Op>,
    asks: Vec<String>,
    /// None: leave questions unanswered.
    ask_answer: Option<Answer>,
    hold_http: bool,
    hold_fs: bool,
    hold_proc: bool,
    done: bool,
    exit_code: i32,
    /// Shown when a check fails (the chaos test's seed).
    ctx: String,
    /// Writes fail with Denied.
    fail_writes: bool,
}

fn test_cfg() -> Config {
    Config {
        model: b"claude-test".to_vec(),
        cwd: b"/work".to_vec(),
        platform: b"TestOS".to_vec(),
        date: b"2026-11-21".to_vec(),
        auto_approve: true,
        ..Config::default()
    }
}

fn s(b: &[u8]) -> String {
    String::from_utf8_lossy(b).into_owned()
}

/// The elements of a JSON array.
fn elems(arr: &[u8]) -> Option<Vec<&[u8]>> {
    let mut j = Reader::new(arr);
    if j.next() != Tok::Arr {
        return None;
    }
    let mut v = Vec::new();
    loop {
        let t = j.next();
        if t == Tok::ArrEnd {
            return Some(v);
        }
        v.push(j.capture(t)?);
    }
}

fn get_str(obj: &[u8], key: &str) -> Option<Vec<u8>> {
    json::as_str(json::get(obj, key)?)
}

fn mk_reply(stop: &str, content: &str) -> String {
    format!(
        "{{\"id\":\"msg_x\",\"type\":\"message\",\"role\":\"assistant\",\"content\":{content},\
         \"stop_reason\":\"{stop}\",\"usage\":{{\"input_tokens\":100,\"output_tokens\":20}}}}"
    )
}

fn tool(id: &str, name: &str, input: &str) -> String {
    format!("{{\"type\":\"tool_use\",\"id\":\"{id}\",\"name\":\"{name}\",\"input\":{input}}}")
}

const OVERLOADED: &str = "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}";

impl World {
    fn new_cfg(auto_approve: bool, max_turns: u32, instructions: bool) -> World {
        let cfg = Config {
            model: b"claude-test".to_vec(),
            max_tokens: 0,
            cwd: b"/work".to_vec(),
            platform: b"TestOS".to_vec(),
            date: b"2026-11-21".to_vec(),
            auto_approve,
            max_turns,
            instructions,
            session: Vec::new(),
            verbose: false,
        };
        World::with(cfg)
    }
    fn with(cfg: Config) -> World {
        World {
            g: Agent::new(cfg),
            q: VecDeque::new(),
            replies: Vec::new(),
            irep: 0,
            unexpected_requests: 0,
            files: Vec::new(),
            procs: Vec::new(),
            says: Vec::new(),
            usage: 0,
            bodies: Vec::new(),
            ops: Vec::new(),
            asks: Vec::new(),
            ask_answer: None,
            hold_http: false,
            hold_fs: false,
            hold_proc: false,
            done: false,
            exit_code: 0,
            ctx: String::new(),
            fail_writes: false,
        }
    }
    /// A world whose conversation is saved to `path`.
    fn saving(path: &str) -> World {
        let mut w = World::new(true);
        let mut cfg = test_cfg();
        cfg.session = path.as_bytes().to_vec();
        w.g = Agent::new(cfg);
        w
    }
    fn new(auto_approve: bool) -> World {
        World::new_cfg(auto_approve, 0, false)
    }

    fn put_file(&mut self, path: &str, data: Option<&[u8]>) {
        let d = data.map(|d| d.to_vec());
        match self.files.iter_mut().find(|f| f.0 == path) {
            Some(f) => f.1 = d,
            None => self.files.push((path.to_string(), d)),
        }
    }
    fn put(&mut self, path: &str, data: &str) {
        self.put_file(path, Some(data.as_bytes()));
    }
    fn file(&self, path: &str) -> Option<String> {
        self.files.iter().find(|f| f.0 == path).and_then(|f| f.1.as_deref()).map(s)
    }

    fn reply(&mut self, body: &str, status: i32) {
        self.replies.push((body.to_string(), status));
    }
    fn reply_text(&mut self, text: &str) {
        let r = mk_reply("end_turn", &format!("[{{\"type\":\"text\",\"text\":\"{text}\"}}]"));
        self.reply(&r, 200);
    }
    fn reply_tools(&mut self, blocks: &[String]) {
        let r = mk_reply("tool_use", &format!("[{}]", blocks.join(",")));
        self.reply(&r, 200);
    }

    // ---- the rules the API enforces ----

    fn check_body(&self, body: &[u8]) {
        let ctx = &self.ctx;
        let bs = s(body);
        assert!(json::valid(body), "{ctx}: request is not JSON: {bs}");
        assert!(json::get(body, "model").is_some(), "{ctx}: no model");
        let msgs = elems(json::get(body, "messages").expect("messages")).expect("messages is an array");
        // tool_use ids the last assistant message left unanswered, and whether each has been
        let mut pending: Vec<(Vec<u8>, bool)> = Vec::new();
        for (idx, m) in msgs.iter().enumerate() {
            let role = get_str(m, "role").expect("role");
            let assistant = role == b"assistant";
            assert_eq!(assistant, idx % 2 == 1, "{ctx}: roles alternate, user first: {bs}");
            let blocks = elems(json::get(m, "content").expect("content")).expect("content is an array");
            let mut seen_non_result = false;
            for blk in &blocks {
                let ty = get_str(blk, "type").expect("block type");
                match &ty[..] {
                    b"text" => {
                        let text = get_str(blk, "text").expect("text");
                        assert!(!text.is_empty(), "{ctx}: an empty text block: {bs}");
                        if !assistant {
                            seen_non_result = true;
                        }
                    }
                    b"tool_use" => {
                        assert!(assistant, "{ctx}: tool_use from the user");
                        let id = get_str(blk, "id").expect("tool_use id");
                        assert!(json::get(blk, "input").is_some(), "{ctx}: tool_use without input");
                        pending.push((id, false));
                    }
                    b"tool_result" => {
                        assert!(!assistant, "{ctx}: tool_result from the assistant");
                        assert!(!seen_non_result, "{ctx}: tool_result after text: {bs}");
                        let id = get_str(blk, "tool_use_id").expect("tool_use_id");
                        let p = pending.iter_mut().find(|p| p.0 == id && !p.1);
                        assert!(p.is_some(), "{ctx}: a result for an unknown call {}: {bs}", s(&id));
                        p.unwrap().1 = true;
                    }
                    _ => {}
                }
            }
            assert!(!blocks.is_empty(), "{ctx}: an empty message: {bs}");
            if !assistant && idx > 0 {
                assert!(pending.iter().all(|p| p.1), "{ctx}: a tool_use left unanswered: {bs}");
                pending.clear();
            }
        }
        // A request ends on a user message, or the API would be asked to continue its own turn.
        assert!(msgs.len() % 2 == 1, "{ctx}: the request does not end on a user message: {bs}");
    }

    // ---- carrying out actions ----

    fn op(&mut self, id: i64, kind: Kind) -> &mut Op {
        self.ops.push(Op { id, kind, bg: false, n: 0, a: String::new(), c: String::new(), cancelled: false });
        self.ops.last_mut().unwrap()
    }

    fn act(&mut self, a: Action) {
        match a {
            Action::Say { style, text, tokens } => {
                self.says.push((style, s(&text)));
                if style == Style::Usage {
                    self.usage = tokens;
                }
            }
            Action::Http { id, path, body } => {
                self.check_body(&body);
                assert_eq!(path, "/v1/messages");
                self.bodies.push(s(&body));
                self.op(id, Kind::Http);
                if !self.hold_http {
                    if self.irep >= self.replies.len() {
                        self.unexpected_requests += 1;
                        return;
                    }
                    let (body, status) = self.replies[self.irep].clone();
                    self.irep += 1;
                    let note = if status == 0 { "could not connect" } else { "" };
                    self.q.push_back(Ev::Http { id, status, body: body.into_bytes(), note: note.into() });
                }
            }
            Action::Read { id, path } => {
                let path = s(&path);
                self.op(id, Kind::Read).a = path.clone();
                if self.hold_fs {
                    return;
                }
                let ev = match self.files.iter().find(|f| f.0 == path) {
                    None => Ev::File { id, status: Fs::NotFound, data: vec![] },
                    Some((_, None)) => Ev::File { id, status: Fs::IsDir, data: vec![] },
                    Some((_, Some(d))) => Ev::File { id, status: Fs::Ok, data: d.clone() },
                };
                self.q.push_back(ev);
            }
            Action::Write { id, path, data } => {
                let path = s(&path);
                let o = self.op(id, Kind::Write);
                o.a = path.clone();
                o.c = s(&data);
                if self.hold_fs {
                    return;
                }
                if self.fail_writes {
                    self.q.push_back(Ev::File { id, status: Fs::Denied, data: vec![] });
                    return;
                }
                self.put_file(&path, Some(&data));
                self.q.push_back(Ev::File { id, status: Fs::Ok, data: vec![] });
            }
            Action::Spawn { id, cmd, timeout_ms, background, log } => {
                let cmd = s(&cmd);
                let o = self.op(id, Kind::Spawn);
                o.bg = background;
                o.n = timeout_ms;
                o.a = cmd.clone();
                o.c = s(&log);
                if !background && !self.hold_proc {
                    let (out, code, flags) = self
                        .procs
                        .iter()
                        .find(|p| cmd.contains(p.0))
                        .map(|p| (p.1.clone(), p.2, p.3))
                        .unwrap_or_default();
                    let note = if flags & PROC_FAILED != 0 { "no such shell" } else { "" };
                    self.q.push_back(Ev::Proc { id, exit: exit_of(code, flags), out, ms: 10, note: note.into() });
                }
            }
            Action::Timer { id, ms } => self.op(id, Kind::Timer).n = ms,
            Action::Cancel { id } => {
                for o in self.ops.iter_mut().filter(|o| o.id == id) {
                    o.cancelled = true;
                }
            }
            Action::Ask { id, tool, what } => {
                assert!(!tool.is_empty());
                self.asks.push(s(&what));
                self.op(id, Kind::Ask);
                if let Some(answer) = self.ask_answer {
                    self.q.push_back(Ev::Answer { id, answer });
                }
            }
            Action::Done { status } => {
                self.done = true;
                self.exit_code = status;
            }
        }
    }

    fn step(&mut self, e: Ev) {
        let acts = match &e {
            Ev::Input(t) => self.g.step(&Event::Input(t.as_bytes())),
            Ev::Eof => self.g.step(&Event::Eof),
            Ev::Http { id, status, body, note } => {
                self.g.step(&Event::Http { id: *id, status: *status, body, note: note.as_bytes() })
            }
            Ev::File { id, status, data } => self.g.step(&Event::File { id: *id, status: *status, data, note: b"" }),
            Ev::Answer { id, answer } => self.g.step(&Event::Answer { id: *id, answer: *answer }),
            Ev::Proc { id, exit, out, ms, note } => {
                self.g.step(&Event::Proc { id: *id, exit: *exit, out, ms: *ms, note: note.as_bytes() })
            }
            Ev::Timer(id) => self.g.step(&Event::Timer { id: *id }),
            Ev::Interrupt => self.g.step(&Event::Interrupt),
        };
        for a in acts {
            self.act(a);
        }
    }

    /// Process queued events until nothing is left (held operations stay unanswered).
    fn run(&mut self) {
        let mut guard = 0;
        while let Some(e) = self.q.pop_front() {
            guard += 1;
            assert!(guard < 2000, "{}: runaway event loop", self.ctx);
            self.step(e);
        }
    }

    fn event(&mut self, e: Ev) {
        self.q.push_back(e);
        self.run();
    }
    fn input(&mut self, line: &str) {
        self.event(Ev::Input(line.into()));
    }
    fn eof(&mut self) {
        self.event(Ev::Eof);
    }
    fn interrupt(&mut self) {
        self.event(Ev::Interrupt);
    }
    fn http(&mut self, id: i64, status: i32, body: &str) {
        self.event(Ev::Http { id, status, body: body.as_bytes().to_vec(), note: String::new() });
    }
    fn file_ev(&mut self, id: i64, status: Fs, data: &str) {
        self.event(Ev::File { id, status, data: data.as_bytes().to_vec() });
    }
    fn answer(&mut self, id: i64, answer: Answer) {
        self.event(Ev::Answer { id, answer });
    }
    fn proc_ev(&mut self, id: i64, code: i32, flags: i32, out: &str) {
        let exit = exit_of(code, flags);
        self.event(Ev::Proc { id, exit, out: out.as_bytes().to_vec(), ms: 5000, note: String::new() });
    }
    fn timer(&mut self, id: i64) {
        self.event(Ev::Timer(id));
    }

    fn last_op(&self, kind: Kind) -> i64 {
        self.ops.iter().rev().find(|o| o.kind == kind).map_or(0, |o| o.id)
    }
    fn op_of(&self, id: i64) -> &Op {
        self.ops.iter().find(|o| o.id == id).expect("no such op")
    }
    fn count_reads(&self, path: &str) -> usize {
        self.ops.iter().filter(|o| o.kind == Kind::Read && o.a == path).count()
    }

    // ---- looking at what happened ----

    fn said(&self, needle: &str) -> bool {
        self.says.iter().any(|x| x.1.contains(needle))
    }

    fn last_body(&self) -> &str {
        self.bodies.last().map_or("", |b| b.as_str())
    }

    /// The tool_result for `id` in the most recent request, and whether it is an error.
    fn result_of(&self, id: &str) -> Option<(String, bool)> {
        let body = self.bodies.last()?.as_bytes();
        let mut out = None;
        for m in elems(json::get(body, "messages")?)? {
            let Some(content) = json::get(m, "content") else { continue };
            for blk in elems(content)? {
                if get_str(blk, "tool_use_id").as_deref() != Some(id.as_bytes()) {
                    continue;
                }
                let text = get_str(blk, "content").map(|t| s(&t)).unwrap_or_default();
                out = Some((text, json::get(blk, "is_error").is_some()));
            }
        }
        out
    }
    fn result(&self, id: &str) -> String {
        self.result_of(id).map_or("(none)".into(), |r| r.0)
    }
    fn is_error(&self, id: &str) -> bool {
        self.result_of(id).expect("no such result").1
    }
    #[track_caller]
    fn result_has(&self, id: &str, needle: &str) {
        let r = self.result(id);
        assert!(r.contains(needle), "result of {id} lacks {needle:?}: {r:?}");
    }

    /// All the text blocks of the last message of the last request, joined by '|'.
    fn last_user_text(&self) -> String {
        let body = self.last_body().as_bytes();
        let msgs = elems(json::get(body, "messages").unwrap()).unwrap();
        let mut parts = Vec::new();
        if let Some(last) = msgs.last() {
            for blk in elems(json::get(last, "content").unwrap()).unwrap() {
                if get_str(blk, "type").as_deref() == Some(b"text") {
                    parts.push(s(&get_str(blk, "text").unwrap()));
                }
            }
        }
        parts.join("|")
    }
}

// ---- the scenarios ----

#[test]
fn chat() {
    let mut w = World::new(true);
    w.reply_text("Hello there.");
    w.input("hi");
    assert_eq!(w.bodies.len(), 1);
    assert!(w.said("Hello there."));
    assert!(w.said("[tokens: 100 in, 20 out]"));
    assert_eq!(w.says.last().unwrap().0, Style::Usage);
    assert_eq!(w.usage, 120);
    let b = &w.bodies[0];
    assert!(b.contains("You are Barb"));
    assert!(b.contains("Working directory: /work"));
    assert!(b.contains("\"name\":\"read_file\""));
    assert!(b.contains("\"model\":\"claude-test\""));
    assert_eq!(w.last_user_text(), "hi");
    assert_eq!(w.bodies.len(), 1); // nothing more is asked of the model: it is idle
    assert!(!w.done);
    w.reply(
        "{\"id\":\"m2\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\
         \"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":5,\"cache_creation_input_tokens\":7,\
         \"cache_read_input_tokens\":1000,\"output_tokens\":30}}",
        200,
    );
    w.input("again");
    assert!(w.said("[tokens: 1012 in (1000 cached), 30 out]"));
    assert_eq!(w.usage, 1042); // all of it is in the conversation the next request sends
    w.eof();
    assert!(w.done && w.exit_code == 0);
}

#[test]
fn read_file() {
    let mut w = World::new(true);
    w.put("a.txt", "one\ntwo\nthree\n");
    w.put("ten.txt", "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\n");
    w.put("empty.txt", "");
    w.put_file("dir", None);
    w.put("crlf.txt", "a\r\nb\r\n");
    w.put_file("bin.dat", Some(b"ab\0cd"));
    w.put("long.txt", &"x".repeat(2500));
    w.reply_tools(&[
        tool("t1", "read_file", r#"{"path":"./a.txt"}"#),
        tool("t2", "read_file", r#"{"path":"ten.txt","offset":3,"limit":2}"#),
        tool("t3", "read_file", r#"{"path":"empty.txt"}"#),
        tool("t4", "read_file", r#"{"path":"nope.txt"}"#),
        tool("t5", "read_file", r#"{"path":"dir"}"#),
        tool("t6", "read_file", r#"{"path":"crlf.txt"}"#),
        tool("t7", "read_file", r#"{"path":"bin.dat"}"#),
        tool("t8", "read_file", r#"{"path":"long.txt"}"#),
        tool("t9", "read_file", r#"{"path":"ten.txt","offset":11}"#),
    ]);
    w.reply_text("ok");
    w.input("read things");
    assert_eq!(w.bodies.len(), 2);
    assert_eq!(w.result("t1"), "     1\tone\n     2\ttwo\n     3\tthree\n");
    assert_eq!(
        w.result("t2"),
        "     3\tl3\n     4\tl4\n\n(showing lines 3-4 of 10; call read_file again with offset 5 to continue)"
    );
    assert_eq!(w.result("t3"), "(the file is empty)");
    w.result_has("t4", "No such file: nope.txt");
    w.result_has("t5", "is a directory");
    assert_eq!(w.result("t6"), "     1\ta\n     2\tb\n");
    w.result_has("t7", "looks like a binary file");
    w.result_has("t8", "[line cut: it is 2500 bytes long]");
    w.result_has("t9", "offset 11 is past the end of the file, which has 10 lines");
    assert!(w.is_error("t4"));
    assert!(!w.is_error("t1"));
}

#[test]
fn read_file_limits() {
    // The output size limit stops a long read, and says where to go on.
    let mut w = World::new(true);
    let line = "y".repeat(1000);
    let big: String = (0..200).map(|_| format!("{line}\n")).collect();
    w.put("big.txt", &big);
    w.put("cut.txt", &format!("{}é{}\n", "a".repeat(1999), "b".repeat(10)));
    w.reply_tools(&[
        tool("t1", "read_file", r#"{"path":"big.txt"}"#),
        tool("t2", "read_file", r#"{"path":"cut.txt"}"#),
        tool("t3", "read_file", r#"{"path":"big.txt","limit":999999,"offset":-4}"#),
    ]);
    w.reply_text("ok");
    w.input("go");
    let r = w.result("t1");
    assert!(r.contains("; stopped at the output size limit; call read_file again with offset "), "{r}");
    assert!(r.len() < 102_000);
    // A line cut does not split a character: the two bytes of the é go together or not at all.
    let r2 = w.result("t2");
    assert!(r2.starts_with(&format!("     1\t{} ... [line cut: it is 2011 bytes long]", "a".repeat(1999))), "{r2}");
    assert_eq!(w.result("t3"), w.result("t1"));
}

#[test]
fn input_errors() {
    let mut w = World::new(true);
    w.reply_tools(&[
        tool("t1", "read_file", "[]"),
        tool("t2", "read_file", "{}"),
        tool("t3", "read_file", r#"{"path":5}"#),
        tool("t4", "read_file", r#"{"path":"a\u0000b"}"#),
        tool("t5", "frobnicate", "{}"),
        tool("t6", "read_file", r#"{"path":""}"#),
        tool("t7", "read_file", r#"{"path":"x","offset":"two"}"#),
        tool("t8", "bash", r#"{"command":"  "}"#),
        tool("t9", "bash", r#"{"command":"a\u0000b"}"#),
        tool("t10", "edit_file", r#"{"path":"x","old_string":"a","new_string":"b","replace_all":"yes"}"#),
    ]);
    w.reply_text("ok");
    w.input("go");
    assert_eq!(w.bodies.len(), 2);
    w.result_has("t1", "must be a JSON object");
    w.result_has("t2", "Missing required argument 'path' (a string)");
    w.result_has("t3", "Argument 'path' must be a string");
    w.result_has("t4", "without NUL characters");
    w.result_has("t5", "There is no tool named 'frobnicate'");
    w.result_has("t6", "non-empty file path");
    w.result_has("t7", "Argument 'offset' must be an integer");
    w.result_has("t8", "The command is empty");
    w.result_has("t9", "Argument 'command' must be text without NUL characters.");
    w.result_has("t10", "Argument 'replace_all' must be true or false.");
    for t in ["t1", "t5", "t8", "t10"] {
        assert!(w.is_error(t));
    }
}

#[test]
fn edit() {
    let mut w = World::new(true);
    w.put("src/a.c", "int a;\nint b;\nint a2;\nreturn 0;\n");
    w.put("dup.c", "x = 1;\ny = 2;\nx = 1;\n");
    w.put("crlf.c", "p\r\nq\r\n");
    w.reply_tools(&[
        tool("r", "read_file", r#"{"path":"src//a.c"}"#),
        tool("r2", "read_file", r#"{"path":"dup.c"}"#),
        tool("r3", "read_file", r#"{"path":"crlf.c"}"#),
    ]);
    w.reply_tools(&[
        tool("e1", "edit_file", r#"{"path":"./src/a.c","old_string":"int b;","new_string":"long b;\nlong c;"}"#),
        tool("e2", "edit_file", r#"{"path":"dup.c","old_string":"x = 1;","new_string":"x = 9;"}"#),
        tool("e3", "edit_file", r#"{"path":"dup.c","old_string":"x = 1;","new_string":"x = 9;","replace_all":true}"#),
        tool("e4", "edit_file", r#"{"path":"src/a.c","old_string":"nothing like this","new_string":"z"}"#),
        tool("e5", "edit_file", r#"{"path":"src/a.c","old_string":"  return 0;","new_string":"z"}"#),
        tool("e6", "edit_file", r#"{"path":"crlf.c","old_string":"p\nq","new_string":"z"}"#),
        tool("e7", "edit_file", r#"{"path":"src/a.c","old_string":"int a;","new_string":"int a;"}"#),
        tool("e8", "edit_file", r#"{"path":"src/a.c","old_string":"","new_string":"z"}"#),
        tool("e9", "edit_file", r#"{"path":"missing.c","old_string":"a","new_string":"b"}"#),
        tool("e10", "edit_file", r#"{"path":"unread.c","old_string":"a","new_string":"b"}"#),
    ]);
    w.put("unread.c", "a\n");
    w.reply_text("done");
    w.input("edit things");
    assert_eq!(w.bodies.len(), 3);
    assert_eq!(w.file("src/a.c").unwrap(), "int a;\nlong b;\nlong c;\nint a2;\nreturn 0;\n");
    w.result_has("e1", "Edited src/a.c: replaced 1 occurrence.");
    w.result_has("e1", "     2\tlong b;\n     3\tlong c;");
    w.result_has("e2", "old_string appears 2 times in dup.c (at lines 1, 3)");
    w.result_has("e2", "set replace_all to true");
    w.result_has("e3", "replaced 2 occurrences");
    assert_eq!(w.file("dup.c").unwrap(), "x = 9;\ny = 2;\nx = 9;\n");
    w.result_has("e4", "old_string was not found in src/a.c");
    w.result_has("e4", "Re-read the lines");
    w.result_has("e5", "whitespace around it differs");
    w.result_has("e6", "CRLF");
    w.result_has("e7", "identical");
    w.result_has("e8", "must not be empty");
    w.result_has("e9", "No such file: missing.c");
    w.result_has("e10", "You have not read unread.c");
    assert_eq!(w.file("unread.c").unwrap(), "a\n"); // untouched
}

#[test]
fn edit_snippet() {
    // The result shows the lines around the change, numbered as they now are.
    let mut w = World::new(true);
    let text: String = (1..=30).map(|i| format!("line {i}\n")).collect();
    w.put("f.txt", &text);
    let many: String = (1..=8).map(|_| "z\n").collect();
    w.reply_tools(&[tool("r", "read_file", r#"{"path":"f.txt"}"#)]);
    w.reply_tools(&[tool(
        "e",
        "edit_file",
        &format!(r#"{{"path":"f.txt","old_string":"line 10\n","new_string":"ten\n"}}"#),
    )]);
    w.reply_tools(&[tool("e2", "edit_file", r#"{"path":"f.txt","old_string":"line 1\n","new_string":"ONE\n"}"#)]);
    w.reply_tools(&[tool(
        "e3",
        "edit_file",
        &format!(r#"{{"path":"f.txt","old_string":"line 20\n","new_string":{}}}"#, json_str(&many)),
    )]);
    w.reply_text("ok");
    w.input("go");
    assert_eq!(w.bodies.len(), 5);
    // Earlier results stay in the conversation: e's still shows the lines as they were then.
    w.result_has("e", "     9\tline 9\n    10\tten\n    11\tline 11\n");
    let r3 = w.result("e3");
    assert!(r3.ends_with("    18\tline 18\n    19\tline 19\n    20\tz\n    21\tz\n    22\tz\n    23\tz\n    24\tz\n    25\tz\n    26\tz\n    27\tz\n    28\tline 21\n    29\tline 22\n"), "{r3}");
    assert!(w.file("f.txt").unwrap().starts_with("ONE\nline 2\n"));
}

fn json_str(t: &str) -> String {
    let mut w = json::Writer::new();
    w.str(t);
    s(&w.out)
}

#[test]
fn write() {
    let mut w = World::new(true);
    w.put("old.txt", "keep\n");
    w.put("seen.txt", "was\n");
    w.reply_tools(&[tool("r", "read_file", r#"{"path":"seen.txt"}"#)]);
    w.reply_tools(&[
        tool("w1", "write_file", r#"{"path":"new/dir/n.txt","content":"hello\n"}"#),
        tool("w2", "write_file", r#"{"path":"old.txt","content":"clobber"}"#),
        tool("w3", "write_file", r#"{"path":"seen.txt","content":"now\n"}"#),
        tool("w4", "write_file", r#"{"path":"e.txt","content":""}"#),
        tool("w5", "write_file", r#"{"path":"e.txt","content":"again"}"#),
    ]);
    w.reply_text("ok");
    w.input("write things");
    assert_eq!(w.result("w1"), "Created new/dir/n.txt (6 bytes).");
    assert_eq!(w.file("new/dir/n.txt").unwrap(), "hello\n");
    w.result_has("w2", "already exists and you have not read it");
    assert_eq!(w.file("old.txt").unwrap(), "keep\n");
    assert_eq!(w.result("w3"), "Replaced seen.txt (4 bytes).");
    assert_eq!(w.file("seen.txt").unwrap(), "now\n");
    assert_eq!(w.result("w4"), "Created e.txt (0 bytes).");
    // What the model wrote counts as seen: it may replace it.
    assert_eq!(w.result("w5"), "Replaced e.txt (5 bytes).");
}

#[test]
fn permissions() {
    // Asking: yes, no, and yes-always.
    let mut w = World::new(false);
    w.ask_answer = Some(Answer::Yes);
    w.put("f.txt", "a\nb\n");
    w.reply_tools(&[tool("r", "read_file", r#"{"path":"f.txt"}"#)]);
    w.reply_tools(&[tool("e1", "edit_file", r#"{"path":"f.txt","old_string":"a","new_string":"A"}"#)]);
    w.reply_text("ok");
    w.input("go");
    assert_eq!(w.asks.len(), 1); // reading needs no permission; the edit did
    assert!(w.asks[0].contains("edit f.txt (1 place)"));
    assert!(w.asks[0].contains("- a"));
    assert!(w.asks[0].contains("+ A"));
    assert_eq!(w.file("f.txt").unwrap(), "A\nb\n");

    let mut w = World::new(false);
    w.ask_answer = Some(Answer::No);
    w.put("f.txt", "a\n");
    w.reply_tools(&[tool("r", "read_file", r#"{"path":"f.txt"}"#)]);
    w.reply_tools(&[
        tool("e1", "edit_file", r#"{"path":"f.txt","old_string":"a","new_string":"A"}"#),
        tool("b1", "bash", r#"{"command":"rm -rf /"}"#),
        tool("w1", "write_file", r#"{"path":"g.txt","content":"g"}"#),
    ]);
    w.reply_text("ok");
    w.input("go");
    w.result_has("e1", "The user declined this action");
    w.result_has("b1", "The user declined this action");
    w.result_has("w1", "The user declined this action");
    assert_eq!(w.file("f.txt").unwrap(), "a\n");
    assert!(w.file("g.txt").is_none());
    assert_eq!(w.last_op(Kind::Spawn), 0); // the command never ran
    assert!(w.asks[2].starts_with("create g.txt (1 bytes)\n+ g\n"));

    let mut w = World::new(false);
    w.ask_answer = Some(Answer::Always);
    w.procs.push(("echo", b"x\n".to_vec(), 0, 0));
    w.reply_tools(&[tool("b1", "bash", r#"{"command":"echo one"}"#)]);
    w.reply_tools(&[tool("b2", "bash", r#"{"command":"echo two"}"#)]);
    w.reply_text("ok");
    w.input("go");
    assert_eq!(w.asks.len(), 1); // "always" was remembered: the second ran unasked
    assert_eq!(w.result("b2"), "x\n[exit code: 0]");

    // A question left open is answered later.
    let mut w = World::new(false);
    w.procs.push(("ls", b"f\n".to_vec(), 0, 0));
    w.reply_tools(&[tool("b1", "bash", r#"{"command":"ls","description":"list files"}"#)]);
    w.reply_text("ok");
    w.input("go");
    assert_eq!(w.asks.len(), 1);
    assert!(w.asks[0].contains("run:\nls\n(list files)"));
    assert_eq!(w.bodies.len(), 1); // waiting for the user, not the model
    // An answer of the wrong kind for the step (a file event with the question's id) is ignored.
    let q = w.last_op(Kind::Ask);
    w.file_ev(q, Fs::Ok, "x");
    assert_eq!(w.bodies.len(), 1);
    w.answer(q, Answer::Yes);
    assert_eq!(w.bodies.len(), 2);
    assert_eq!(w.result("b1"), "f\n[exit code: 0]");
}

#[test]
fn bash() {
    let mut w = World::new(true);
    let big: Vec<u8> = (0..60000).map(|i| if i % 80 == 79 { b'\n' } else { b'a' + (i % 26) as u8 }).collect();
    w.procs = vec![
        ("echo", b"hi\n".to_vec(), 0, 0),
        ("false", vec![], 1, 0),
        ("sleep 999", b"partial".to_vec(), 0, PROC_TIMEDOUT),
        ("nonesuch", vec![], 0, PROC_FAILED),
        ("bigout", big, 0, 0),
        ("sleep 15", vec![], 0, PROC_TIMEDOUT),
        ("sleep 16", vec![], 0, PROC_TIMEDOUT),
        ("kill", b"bye".to_vec(), 0, PROC_SIGNALED),
    ];
    w.reply_tools(&[
        tool("b1", "bash", r#"{"command":"echo hi"}"#),
        tool("b2", "bash", r#"{"command":"false"}"#),
        tool("b3", "bash", r#"{"command":"sleep 999","timeout_ms":2000}"#),
        tool("b4", "bash", r#"{"command":"nonesuch"}"#),
        tool("b5", "bash", r#"{"command":"bigout"}"#),
        tool("b6", "bash", r#"{"command":"sleep 15","timeout_ms":1500}"#),
        tool("b7", "bash", r#"{"command":"sleep 16","timeout_ms":10}"#),
        tool("b8", "bash", r#"{"command":"kill me","timeout_ms":99999999}"#),
    ]);
    w.reply_text("ok");
    w.input("go");
    assert_eq!(w.result("b1"), "hi\n[exit code: 0]");
    assert_eq!(w.result("b2"), "(no output)\n[exit code: 1]");
    w.result_has("b3", "partial\n[killed: it ran longer than its limit of 2 seconds.");
    w.result_has("b6", "[killed: it ran longer than its limit of 1.5 seconds."); // not "1 seconds"
    w.result_has("b7", "[killed: it ran longer than its limit of 1 second."); // clamped up to 1000
    w.result_has("b4", "The command could not be started: no such shell");
    w.result_has("b5", "bytes omitted from the middle");
    assert!(w.result("b5").len() < 31000);
    assert_eq!(w.result("b8"), "bye\n[killed by a signal]");
    assert!(!w.is_error("b2") && w.is_error("b3") && w.is_error("b4") && !w.is_error("b8"));
    // The timeout asked for reached the platform, clamped.
    let n = |cmd: &str| w.ops.iter().find(|o| o.kind == Kind::Spawn && o.a.contains(cmd)).unwrap().n;
    assert_eq!(n("sleep 999"), 2000);
    assert_eq!(n("sleep 16"), 1000);
    assert_eq!(n("kill me"), 600000);
    assert_eq!(n("echo hi"), 120000);
    assert!(w.said("bash echo hi"));
    assert!(w.said("  exit 1"));
    assert!(w.said("  error: partial")); // the first line of the result, which is the output's
    assert!(w.said("  error: The command could not be started"));
}

#[test]
fn cap_output() {
    use barb_core::tools::cap_output;
    assert_eq!(cap_output(b"short", 10), b"short");
    let c = cap_output(b"0123456789abcdefghij", 10);
    assert_eq!(s(&c), "01234\n\n... [10 bytes omitted from the middle] ...\n\nfghij");
    // Neither end cuts a character in two.
    let t = "ééééééééé".as_bytes(); // 18 bytes
    let c = cap_output(t, 7);
    assert!(std::str::from_utf8(&c).is_ok(), "{c:?}");
    assert_eq!(s(&c), "é\n\n... [14 bytes omitted from the middle] ...\n\né");
}

#[test]
fn norm_path() {
    use barb_core::tools::norm_path;
    for (a, b) in [
        ("src//a.c", "src/a.c"),
        ("./src/a.c", "src/a.c"),
        ("src/./a.c/", "src/a.c"),
        ("/abs//x", "/abs/x"),
        ("/", "/"),
        ("./", "."),
        ("..//x", "../x"),
    ] {
        assert_eq!(s(&norm_path(a.as_bytes())), b, "{a}");
    }
}

#[test]
fn background() {
    let mut w = World::new(true);
    w.reply_tools(&[tool("b1", "bash", r#"{"command":"make -j8","run_in_background":true}"#)]);
    w.reply_text("Building; I will wait.");
    w.input("build it");
    assert_eq!(w.bodies.len(), 2);
    w.result_has("b1", "Started background task");
    w.result_has("b1", ".barb/tasks/");
    w.result_has("b1", "You will be woken");
    let task = w.last_op(Kind::Spawn);
    let o = w.op_of(task);
    assert!(o.bg);
    assert!(o.a.contains("make -j8"));
    assert!(o.c.contains(".barb/tasks/") && o.c.contains(".log"));
    assert!(w.said("(waiting for task")); // it tells the user it is asleep
    assert!(!w.done);
    let before = w.bodies.len();
    // Nothing happens, and no model call is made, until the task ends.
    w.timer(9999); // an unknown timer: ignored
    w.proc_ev(9999, 0, 0, "x"); // an unknown process: ignored
    assert_eq!(w.bodies.len(), before);

    w.reply_text("The build passed.");
    w.eof(); // no more input; but a task is still running
    assert!(!w.done);
    w.proc_ev(task, 0, 0, "");
    assert_eq!(w.bodies.len(), before + 1); // woken: one model call
    let t = w.last_user_text();
    assert!(t.contains("[barb] Background task"));
    assert!(t.contains("finished with exit code 0 after 5s"));
    assert!(t.contains("make -j8"));
    assert!(w.said("The build passed."));
    assert!(w.done && w.exit_code == 0); // and now everything is finished

    // A task that fails, and one stopped with task_stop.
    let mut w = World::new(true);
    w.reply_tools(&[tool("b1", "bash", r#"{"command":"sleep 3600","run_in_background":true}"#)]);
    w.reply_tools(&[
        tool("s1", "task_stop", r#"{"task_id":999}"#),
        tool("s2", "task_stop", r#"{"task_id":2}"#),
    ]);
    w.reply_text("stopped");
    w.input("go");
    let id = w.last_op(Kind::Spawn);
    assert_eq!(id, 2);
    w.result_has("s1", "No running background task has the number 999. Running now: 2.");
    assert_eq!(w.result("s2"), "Asked task 2 to stop. You will get a message when it has exited.");
    assert!(w.op_of(id).cancelled);
    w.reply_text("noted");
    w.proc_ev(id, 0, PROC_SIGNALED, "");
    assert!(w.last_user_text().contains("[barb] Background task 2 was killed by a signal after 5s. Command: sleep 3600"));
    w.reply_tools(&[tool("s3", "task_stop", r#"{"task_id":2}"#)]);
    w.reply_text("ok");
    w.input("again");
    w.result_has("s3", "No running background task has the number 2. None are running.");

    // Several running: listed newest first.
    let mut w = World::new(true);
    w.reply_tools(&[
        tool("b1", "bash", r#"{"command":"one","run_in_background":true}"#),
        tool("b2", "bash", r#"{"command":"two","run_in_background":true}"#),
        tool("s", "task_stop", r#"{"task_id":7}"#),
    ]);
    w.reply_text("ok");
    w.input("go");
    w.result_has("s", "No running background task has the number 7. Running now: 3 2.");
}

#[test]
fn wakeup() {
    let mut w = World::new(true);
    w.reply_tools(&[
        tool("w1", "schedule_wakeup", r#"{"seconds":60,"note":"check the build"}"#),
        tool("w2", "schedule_wakeup", r#"{"seconds":0}"#),
        tool("w3", "schedule_wakeup", r#"{"seconds":99999}"#),
    ]);
    w.reply_text("Waiting.");
    w.input("later");
    w.result_has("w1", "you will be woken in 60 seconds");
    w.result_has("w2", "between 1 and 86400");
    w.result_has("w3", "between 1 and 86400");
    assert!(w.said("(waiting for a timer; no model calls until something happens)"));
    let timer = w.last_op(Kind::Timer);
    assert!(timer != 0);
    assert_eq!(w.op_of(timer).n, 60000);
    let before = w.bodies.len();
    w.reply_text("Checked.");
    w.timer(timer);
    assert_eq!(w.bodies.len(), before + 1);
    assert!(w.last_user_text().contains("The timer you set has fired. Your note: check the build"));
    w.timer(timer); // a second ring of the same timer does nothing
    assert_eq!(w.bodies.len(), before + 1);

    // A loop of wake-ups cannot spend without limit: after max_turns model calls it stops.
    let mut w = World::new_cfg(true, 4, false);
    for _ in 0..4 {
        w.reply_tools(&[tool("w", "schedule_wakeup", r#"{"seconds":1}"#)]);
    }
    w.reply_text("back"); // for after the user speaks again
    w.input("start");
    for _ in 0..10 {
        if w.bodies.len() >= 10 {
            break;
        }
        let t2 = w.last_op(Kind::Timer);
        if t2 == 0 {
            break;
        }
        w.timer(t2);
    }
    assert_eq!(w.bodies.len(), 4);
    assert!(w.said("Stopped after 4 model calls"));
    // The user speaking resets the count.
    w.input("carry on");
    assert_eq!(w.bodies.len(), 5);
}

#[test]
fn interrupts() {
    // During a model request: cancelled, and what the user types next joins their first message.
    let mut w = World::new(true);
    w.hold_http = true;
    w.input("first");
    let req = w.last_op(Kind::Http);
    w.interrupt();
    assert!(w.op_of(req).cancelled);
    assert!(w.said("Interrupted."));
    let said = w.says.len();
    w.http(req, 200, "{}"); // a late reply to a cancelled request
    assert_eq!(w.says.len(), said);
    w.hold_http = false;
    w.reply_text("ok");
    w.input("second");
    // ...with a line saying it was interrupted, so the model does not take up the old request as
    // if nothing had happened.
    assert_eq!(w.last_user_text(), "first|[barb] The user interrupted you here, before you replied.|second");

    // During a retry's wait: the timer is cancelled.
    let mut w = World::new(true);
    w.reply(OVERLOADED, 529);
    w.input("hi");
    let t = w.last_op(Kind::Timer);
    w.interrupt();
    assert!(w.op_of(t).cancelled);
    w.timer(t); // too late
    assert_eq!(w.bodies.len(), 1);

    // During tool calls: every call is answered, and the model is not called.
    let mut w = World::new(true);
    w.hold_proc = true;
    w.reply_tools(&[
        tool("b1", "bash", r#"{"command":"sleep 100"}"#),
        tool("b2", "bash", r#"{"command":"echo never"}"#),
    ]);
    w.input("go");
    assert_eq!(w.bodies.len(), 1);
    let spawn = w.last_op(Kind::Spawn);
    w.interrupt();
    assert!(w.op_of(spawn).cancelled);
    assert_eq!(w.bodies.len(), 1);
    w.proc_ev(spawn, 0, 0, "late"); // the killed process reports in: ignored
    assert_eq!(w.bodies.len(), 1);
    w.reply_text("ok");
    w.input("never mind, do this instead");
    assert_eq!(w.bodies.len(), 2);
    w.result_has("b1", "Interrupted by the user");
    w.result_has("b2", "Not run: the user interrupted");
    assert_eq!(w.last_user_text(), "never mind, do this instead");

    // During a question to the user.
    let mut w = World::new(false);
    w.reply_tools(&[tool("b1", "bash", r#"{"command":"ls"}"#)]);
    w.input("go");
    assert_eq!(w.asks.len(), 1);
    let q = w.last_op(Kind::Ask);
    w.interrupt();
    assert!(w.op_of(q).cancelled);
    w.answer(q, Answer::Yes); // too late: ignored, the command does not run
    assert_eq!(w.last_op(Kind::Spawn), 0);

    // Twice at an idle prompt quits.
    let mut w = World::new(true);
    w.interrupt();
    assert!(!w.done);
    assert!(w.said("Press Ctrl-C again"));
    w.interrupt();
    assert!(w.done && w.exit_code == 130);
    // ...but not if something else happened in between.
    let mut w = World::new(true);
    w.reply_text("hi");
    w.interrupt();
    w.input("hello");
    w.interrupt();
    assert!(!w.done);
    // After it is done, the agent does nothing more.
    w.interrupt();
    assert!(w.done);
    let n = w.says.len();
    w.input("anything");
    assert_eq!(w.says.len(), n);
}

#[test]
fn instructions() {
    // AGENTS.md, with an import, goes into the system prompt before the first request.
    let mut w = World::new_cfg(true, 0, true);
    w.put("AGENTS.md", "Be terse.\n@docs/STYLE.md\nThe end.\n");
    w.put("docs/STYLE.md", "Use tabs.");
    assert!(!w.g.busy());
    w.reply_text("ok");
    w.input("hi");
    assert_eq!(w.count_reads("AGENTS.md"), 1);
    assert_eq!(w.count_reads("docs/STYLE.md"), 1);
    assert_eq!(w.count_reads("CLAUDE.md"), 0);
    assert_eq!(w.bodies.len(), 1);
    let b = w.last_body();
    assert!(b.contains("Project instructions"));
    assert!(b.contains("from AGENTS.md in the working directory (with the files it imports), in full below"));
    assert!(b.contains("Be terse.\\nUse tabs.\\nThe end.")); // the import line replaced, in order
    assert!(!b.contains("@docs/STYLE.md"));
    assert!(w.said("(following the project's instructions in AGENTS.md and docs/STYLE.md)"));
    assert_eq!(w.last_user_text(), "hi");
    assert!(!w.g.busy());
    // Read once per session, not per message; /clear keeps them.
    w.reply_text("ok");
    w.input("/clear");
    w.input("again");
    assert_eq!(w.count_reads("AGENTS.md"), 1);
    assert_eq!(w.bodies.len(), 2);
    assert!(w.last_body().contains("Be terse."));

    // No AGENTS.md: CLAUDE.md, whose import of the missing AGENTS.md is marked, not dropped silently.
    let mut w = World::new_cfg(true, 0, true);
    w.put("CLAUDE.md", "@AGENTS.md\nAlso: no emojis.\n");
    w.reply_text("ok");
    w.input("hi");
    assert_eq!(w.count_reads("AGENTS.md"), 2);
    assert_eq!(w.count_reads("CLAUDE.md"), 1);
    let b = w.last_body();
    assert!(b.contains("from CLAUDE.md in the working directory"));
    assert!(b.contains("@AGENTS.md  [barb: this file could not be read]"));
    assert!(b.contains("Also: no emojis."));

    // Neither file: nothing added, and the request goes out as before.
    let mut w = World::new_cfg(true, 0, true);
    w.reply_text("ok");
    w.input("hi");
    assert_eq!(w.bodies.len(), 1);
    assert!(!w.last_body().contains("Project instructions"));
    assert!(!w.said("following the project"));

    // An empty AGENTS.md counts as none; a directory called AGENTS.md too.
    for agents in [Some(&b"  \n\n"[..]), None] {
        let mut w = World::new_cfg(true, 0, true);
        w.put_file("AGENTS.md", agents);
        w.put("CLAUDE.md", "From Claude.");
        w.reply_text("ok");
        w.input("hi");
        assert!(w.last_body().contains("From Claude."));
    }

    // Turned off: no reads at all.
    let mut w = World::new_cfg(true, 0, false);
    w.put("AGENTS.md", "Be terse.");
    w.reply_text("ok");
    w.input("hi");
    assert_eq!(w.count_reads("AGENTS.md"), 0);
    assert!(!w.last_body().contains("Be terse."));

    // Imports: at most eight, each once, and a file does not import itself.
    let mut w = World::new_cfg(true, 0, true);
    w.put("AGENTS.md", "@AGENTS.md\n@a\n@b\n@a\n@c\n@d\n@e\n@f\n@g\n@h\n@i\n@ not an import\nx @a\n");
    for n in ["a", "b", "c", "d", "e", "f", "g", "h", "i"] {
        w.put(n, n);
    }
    w.reply_text("ok");
    w.input("hi");
    assert_eq!(w.count_reads("AGENTS.md"), 1);
    assert_eq!(w.count_reads("a"), 1);
    assert_eq!(w.count_reads("h"), 1);
    assert_eq!(w.count_reads("i"), 0);
    let b = w.last_body();
    assert!(!b.contains("@i  [barb") && b.contains("\\n@i\\n")); // the ninth stays as written
    assert!(b.contains("@ not an import") && b.contains("x @a"));
    // An import's text gets a newline if it lacks one; the self-import stays as written.
    assert!(b.contains("@AGENTS.md\\na\\nb\\na\\nc\\n"));
    assert!(w.said("AGENTS.md and a, b, c, d, e, f, g, h)"));

    // Too long: cut at 64 KB, and said so.
    let mut w = World::new_cfg(true, 0, true);
    w.put("AGENTS.md", &"k".repeat(70000));
    w.reply_text("ok");
    w.input("hi");
    let b = w.last_body();
    assert!(b.contains(&format!("{}\\n[barb: the instructions were cut here", "k".repeat(1000))));
    assert!(b.contains(&format!("{}\\n[barb", "k".repeat(65536))));
    assert!(!b.contains(&"k".repeat(65537)));

    // While they load, the agent is busy. The first message goes out once they are read, and one
    // typed meanwhile waits for the reply, as it would behind any request: the turns come out the
    // same as without instructions.
    let mut w = World::new_cfg(true, 0, true);
    w.put("AGENTS.md", "Be terse.");
    w.hold_fs = true;
    w.input("alpha-msg");
    assert!(w.g.busy());
    w.input("beta-msg");
    assert!(w.said("(queued"));
    assert_eq!(w.bodies.len(), 0);
    w.hold_fs = false;
    w.reply_text("one");
    w.reply_text("two");
    w.file_ev(w.last_op(Kind::Read), Fs::Ok, "Be terse.");
    assert_eq!(w.bodies.len(), 2);
    assert_eq!(w.last_user_text(), "beta-msg");
    assert!(w.bodies[0].contains("\"alpha-msg\"") && !w.bodies[0].contains("beta-msg"));

    // Ctrl-C while they load: drop what was typed, carry on without them, and do not try again.
    let mut w = World::new_cfg(true, 0, true);
    w.hold_fs = true;
    w.input("first");
    w.input("also-typed");
    let rd = w.last_op(Kind::Read);
    w.interrupt();
    assert!(w.op_of(rd).cancelled);
    assert!(!w.g.busy());
    assert_eq!(w.bodies.len(), 0);
    w.file_ev(rd, Fs::Ok, "LATE-READ-TEXT"); // the cancelled read answers anyway: ignored
    assert_eq!(w.bodies.len(), 0);
    w.hold_fs = false;
    w.reply_text("ok");
    w.input("second");
    assert_eq!(w.bodies.len(), 1);
    assert_eq!(w.last_user_text(), "second");
    assert!(!w.last_body().contains("LATE-READ-TEXT"));
    assert_eq!(w.ops.len(), 2); // the one read, then the request: no second attempt

    // -p: the prompt then end of input; the instructions still come first.
    let mut w = World::new_cfg(true, 0, true);
    w.put("AGENTS.md", "Be terse.");
    w.reply_text("done");
    w.input("task");
    w.eof();
    assert!(w.done);
    assert_eq!(w.exit_code, 0);
    assert!(w.last_body().contains("Be terse."));
}

#[test]
fn busy() {
    let mut w = World::new(false);
    assert!(!w.g.busy());
    w.hold_http = true;
    w.input("hi");
    assert!(w.g.busy()); // asking the model
    w.hold_http = false;
    w.reply_tools(&[tool("t1", "bash", r#"{"command":"echo hi"}"#)]);
    let (body, _) = w.replies[w.irep].clone();
    w.irep += 1;
    w.http(w.last_op(Kind::Http), 200, &body);
    assert!(w.g.busy()); // asking the user about a tool call
    w.reply_text("ok");
    w.answer(w.last_op(Kind::Ask), Answer::Yes);
    assert!(!w.g.busy()); // done: the prompt may show
    w.reply_tools(&[tool("t2", "bash", r#"{"command":"sleep 9","run_in_background":true}"#)]);
    w.reply_text("waiting");
    w.ask_answer = Some(Answer::Yes);
    w.input("start it");
    assert!(!w.g.busy()); // a background task is not the agent being busy
}

#[test]
fn api_errors() {
    // A transient error is retried with a delay, the same request is sent again, and then it works.
    let mut w = World::new(true);
    w.reply(OVERLOADED, 529);
    w.reply_text("finally");
    w.input("hi");
    assert_eq!(w.bodies.len(), 1);
    assert!(w.said("API returned HTTP 529; retrying in 1s (attempt 1 of 5)"));
    let timer = w.last_op(Kind::Timer);
    assert_eq!(w.op_of(timer).n, 1000);
    w.timer(timer);
    assert_eq!(w.bodies.len(), 2);
    assert_eq!(w.bodies[1], w.bodies[0]); // exactly the same request
    assert!(w.said("finally"));

    // Retries run out: the delay doubles each time, and the last error is shown.
    let mut w = World::new(true);
    for _ in 0..6 {
        w.reply(OVERLOADED, 529);
    }
    w.input("hi");
    for i in 0..5 {
        let t = w.last_op(Kind::Timer);
        assert_eq!(w.op_of(t).n, 1000 << i);
        w.timer(t);
    }
    assert_eq!(w.bodies.len(), 6);
    assert!(w.said("API error (HTTP 529, overloaded_error): Overloaded"));
    // The conversation is still sound: the next message joins the unanswered one.
    w.reply_text("ok");
    w.input("again");
    assert_eq!(w.last_user_text(), "hi|again");

    // The request itself failing.
    let mut w = World::new(true);
    w.reply("", 0);
    w.reply_text("back");
    w.input("hi");
    assert!(w.said("request failed: could not connect; retrying in 1s"));
    w.timer(w.last_op(Kind::Timer));
    assert!(w.said("back"));

    // ...and failing every time.
    let mut w = World::new(true);
    for _ in 0..6 {
        w.reply("", 0);
    }
    w.input("hi");
    for _ in 0..5 {
        w.timer(w.last_op(Kind::Timer));
    }
    assert!(w.said("The request failed and retrying did not help: could not connect"));
    w.eof();
    assert!(w.done && w.exit_code == 1);

    // Errors that are not worth retrying.
    let mut w = World::new(true);
    w.reply(
        "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}",
        401,
    );
    w.input("hi");
    assert_eq!(w.bodies.len(), 1);
    assert_eq!(w.last_op(Kind::Timer), 0);
    assert!(w.said("API error (HTTP 401, authentication_error): invalid x-api-key"));
    assert!(w.said("Check ANTHROPIC_API_KEY"));
    w.eof();
    assert!(w.done && w.exit_code == 1); // a failed run says so in its exit status

    let mut w = World::new(true);
    w.reply(
        "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"prompt is too long: 250000 tokens > 200000\"}}",
        400,
    );
    w.input("hi");
    assert!(w.said("Use /clear to start over"));

    // A 200 that is not a message, and a gateway's HTML error page.
    let mut w = World::new(true);
    w.reply("{\"hello\":1}", 200);
    w.reply("<html>bad gateway</html>", 502);
    w.input("hi");
    assert!(w.said("API error (HTTP 200): unreadable reply: {\"hello\":1}"));

    // A refusal, and a successful reply after a failure clears the exit status.
    let mut w = World::new(true);
    w.reply("<html>no</html>", 400);
    w.reply(&mk_reply("refusal", "[]"), 200);
    w.input("hi");
    w.input("again");
    assert!(w.said("[the model declined to continue]"));
    w.eof();
    assert!(w.done && w.exit_code == 0);
}

#[test]
fn queued_and_turns() {
    // Typing while the model is busy: queued, and sent as the next message when the reply is in.
    let mut w = World::new(true);
    w.hold_http = true;
    w.input("A");
    let r1 = w.last_op(Kind::Http);
    w.input("B");
    assert!(w.said("(queued"));
    assert_eq!(w.bodies.len(), 1);
    let ra = mk_reply("end_turn", "[{\"type\":\"text\",\"text\":\"answer to A\"}]");
    w.http(r1, 200, &ra);
    assert_eq!(w.bodies.len(), 2);
    assert_eq!(w.last_user_text(), "B");

    // Typing during tool calls: it follows the results, in the same message.
    let mut w = World::new(true);
    w.hold_proc = true;
    w.reply_tools(&[tool("b1", "bash", r#"{"command":"ls"}"#)]);
    w.input("go");
    w.input("also, be careful");
    let sp = w.last_op(Kind::Spawn);
    w.reply_text("ok");
    w.proc_ev(sp, 0, 0, "x\n");
    assert_eq!(w.bodies.len(), 2);
    assert_eq!(w.result("b1"), "x\n[exit code: 0]");
    assert_eq!(w.last_user_text(), "also, be careful");

    // A reply cut off at max_tokens with a tool call in it: the call is not run, but it is answered.
    let mut w = World::new(true);
    let cut = mk_reply("max_tokens", &format!("[{}]", tool("t1", "write_file", r#"{"path":"big.txt","content":"abc"}"#)));
    w.reply(&cut, 200);
    w.reply_text("I will do it in parts.");
    w.input("write a big file");
    assert_eq!(w.bodies.len(), 2);
    w.result_has("t1", "Not run: your reply was cut off");
    assert!(w.file("big.txt").is_none());
    assert!(w.said("[the reply was cut off at the token limit; tool calls not run]"));

    // Cut off with only text.
    let mut w = World::new(true);
    w.reply(&mk_reply("max_tokens", "[{\"type\":\"text\",\"text\":\"half\"}]"), 200);
    w.input("go");
    assert!(w.said("[the reply was cut off at the token limit]"));

    // The turn limit with tool calls pending: results are kept, and the user can carry on.
    let mut w = World::new_cfg(true, 2, false);
    w.put("a", "x\n");
    w.reply_tools(&[tool("t1", "read_file", r#"{"path":"a"}"#)]);
    w.reply_tools(&[tool("t2", "read_file", r#"{"path":"a"}"#)]);
    w.reply_text("continuing");
    w.input("loop");
    assert_eq!(w.bodies.len(), 2);
    assert!(w.said("Stopped after 2 model calls"));
    w.input("go on");
    assert_eq!(w.bodies.len(), 3);
    assert_eq!(w.result("t2"), "     1\tx\n");
    assert_eq!(w.last_user_text(), "go on");
}

#[test]
fn commands() {
    let mut w = World::new(true);
    w.reply_text("one");
    w.input("hello");
    w.input("/clear");
    assert!(w.said("Conversation cleared"));
    w.reply_text("two");
    w.input("fresh start");
    assert_eq!(w.last_user_text(), "fresh start");
    assert!(!w.bodies[1].contains("hello"));
    w.input("/help");
    assert!(w.said("/clear"));
    w.input("/bogus");
    assert!(w.said("Unknown command /bogus. Try /help."));
    w.reply_text("path question answered");
    let n = w.bodies.len();
    w.input("/etc/hosts is broken"); // not a command: it is a question
    assert_eq!(w.bodies.len(), n + 1);
    w.input("   "); // nothing
    assert_eq!(w.bodies.len(), n + 1);
    // /clear while busy is refused.
    w.hold_http = true;
    w.input("slow");
    w.input("/clear");
    assert!(w.said("Busy: wait for the current step"));
    w.input("/exit");
    assert!(w.done && w.exit_code == 0);

    let mut w = World::new(true);
    w.input("/quit now"); // a command's prefix counts even with more after it
    assert!(w.said("Unknown command /quit now"));
    w.input("/quit");
    assert!(w.done && w.exit_code == 0);
}

#[test]
fn stale_events() {
    // A reply that is not for the request in flight is ignored; the real one still works.
    let mut w = World::new(true);
    w.hold_http = true;
    w.input("hi");
    let real = w.last_op(Kind::Http);
    let wrong = mk_reply("end_turn", "[{\"type\":\"text\",\"text\":\"from nowhere\"}]");
    w.http(real + 1000, 200, &wrong);
    assert!(!w.said("from nowhere"));
    w.http(real, 200, &wrong);
    assert!(w.said("from nowhere"));

    let mut w = World::new(true);
    w.reply_text("ok");
    w.http(12345, 200, "{}");
    w.file_ev(12345, Fs::Ok, "x");
    w.answer(12345, Answer::Yes);
    w.proc_ev(12345, 0, 0, "x");
    w.timer(12345);
    assert_eq!(w.bodies.len(), 0);
    assert_eq!(w.says.len(), 0);
    w.input("hi");
    assert_eq!(w.bodies.len(), 1);
}

#[test]
fn file_errors() {
    // File-system failures, in words the model can act on.
    let mut w = World::new(true);
    w.hold_fs = true;
    w.reply_tools(&[
        tool("r1", "read_file", r#"{"path":"big"}"#),
        tool("r2", "read_file", r#"{"path":"locked"}"#),
        tool("r3", "read_file", r#"{"path":"odd"}"#),
    ]);
    w.reply_text("ok");
    w.input("go");
    w.file_ev(w.last_op(Kind::Read), Fs::TooBig, "");
    w.event(Ev::File { id: w.last_op(Kind::Read), status: Fs::Denied, data: vec![] });
    w.file_ev(w.last_op(Kind::Read), Fs::Error, "");
    w.result_has("r1", "big is too large to read whole. Look at parts of it with bash");
    assert_eq!(w.result("r2"), "Permission denied read locked.");
    assert_eq!(w.result("r3"), "Could not read odd");
}

// ---- chaos ----

// Random events in random order, with every request checked against the API's rules. The agent
// may do whatever it likes with nonsense, except break the conversation or loop forever.
struct Rnd(u32);
impl Rnd {
    fn n(&mut self, n: u32) -> u32 {
        self.0 = self.0.wrapping_mul(1103515245).wrapping_add(12345);
        (self.0 >> 8) % n
    }
}

fn random_reply(w: &mut World, r: &mut Rnd) {
    match r.n(12) {
        0..=2 => w.reply_text("Some text."),
        3 => w.reply_tools(&[tool("a", "read_file", r#"{"path":"f.txt"}"#)]),
        4 => w.reply_tools(&[
            tool("a", "edit_file", r#"{"path":"f.txt","old_string":"x","new_string":"y"}"#),
            tool("b", "bash", r#"{"command":"echo hi"}"#),
        ]),
        5 => w.reply_tools(&[tool("a", "write_file", r#"{"path":"f.txt","content":"x"}"#)]),
        6 => w.reply_tools(&[
            tool("a", "bash", r#"{"command":"make","run_in_background":true}"#),
            tool("b", "schedule_wakeup", r#"{"seconds":30,"note":"look"}"#),
        ]),
        7 => w.reply_tools(&[tool("a", "task_stop", r#"{"task_id":3}"#), tool("b", "nonesuch", "{}")]),
        8 => {
            let c = format!("[{{\"type\":\"text\",\"text\":\"cut\"}},{}]", tool("a", "bash", r#"{"command":"x"}"#));
            w.reply(&mk_reply("max_tokens", &c), 200);
        }
        9 => w.reply("{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"busy\"}}", 529),
        10 => w.reply("{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"bad\"}}", 400),
        _ => {
            let c = "[{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"s\"},{\"type\":\"text\",\"text\":\"after thinking\"}]";
            w.reply(&mk_reply("end_turn", c), 200);
        }
    }
}

const FS_ALL: [Fs; 6] = [Fs::Ok, Fs::NotFound, Fs::IsDir, Fs::TooBig, Fs::Denied, Fs::Error];
const ANS_ALL: [Answer; 3] = [Answer::No, Answer::Yes, Answer::Always];

#[test]
fn random_events() {
    let mut total_requests = 0;
    let mut resumed = 0;
    for seed in 1..=400u32 {
        let mut r = Rnd(seed.wrapping_mul(7919));
        let auto = r.n(2) == 1;
        let turns = 1 + r.n(8);
        let instr = r.n(2) == 1;
        let mut w = World::new_cfg(auto, turns, instr);
        let saving = r.n(2) == 1;
        if saving {
            let mut cfg = test_cfg();
            cfg.auto_approve = auto;
            cfg.max_turns = turns;
            cfg.instructions = instr;
            cfg.session = b"s.json".to_vec();
            cfg.verbose = r.n(2) == 1;
            w.g = Agent::new(cfg);
        }
        w.ctx = format!("seed {seed}");
        w.put("f.txt", "x\nx\n");
        if r.n(2) == 1 {
            w.put("AGENTS.md", "Rules.\n@f.txt\n@gone\n");
        }
        w.hold_http = r.n(3) == 0;
        w.hold_proc = r.n(3) == 0;
        w.ask_answer = [None, Some(Answer::No), Some(Answer::Yes), Some(Answer::Always)][r.n(4) as usize];
        w.procs.push(("echo", b"hi\n".to_vec(), 0, 0));
        for _ in 0..20 {
            random_reply(&mut w, &mut r);
        }
        for _ in 0..60 {
            if w.done {
                break;
            }
            let pick = |w: &World, r: &mut Rnd| w.ops[r.n(w.ops.len() as u32) as usize].id;
            match r.n(10) {
                0 | 1 => w.input("a message"),
                2 => w.interrupt(),
                3 if !w.ops.is_empty() => {
                    let id = pick(&w, &mut r);
                    let (code, flags) = (r.n(3) as i32, r.n(8) as i32);
                    w.proc_ev(id, code, flags, "out");
                }
                4 if !w.ops.is_empty() => {
                    let id = pick(&w, &mut r);
                    w.timer(id);
                }
                5 if !w.ops.is_empty() => {
                    let id = pick(&w, &mut r);
                    let a = ANS_ALL[r.n(3) as usize];
                    w.answer(id, a);
                }
                6 if !w.ops.is_empty() => {
                    let id = pick(&w, &mut r);
                    let st = FS_ALL[r.n(6) as usize];
                    w.file_ev(id, st, "x\n");
                }
                7 => {
                    // a reply, to a real request or to a stale one
                    let id = if !w.ops.is_empty() && r.n(2) == 1 { pick(&w, &mut r) } else { w.last_op(Kind::Http) };
                    if w.irep < w.replies.len() {
                        let (body, status) = w.replies[w.irep].clone();
                        w.irep += 1;
                        w.http(id, status, &body);
                    }
                }
                8 => {
                    w.hold_http = false;
                    w.hold_proc = r.n(2) == 1;
                    w.input("/clear");
                }
                3..=6 => {}
                _ => w.input("go on"),
            }
        }
        total_requests += w.bodies.len();
        // Whatever state it was saved in, a saved conversation can be taken up again, and the
        // request that follows keeps the API's rules (check_body runs on it).
        let saved: Vec<String> = w.ops.iter().filter(|o| o.kind == Kind::Write && o.a == "s.json").map(|o| o.c.clone()).collect();
        if let Some(last) = saved.last() {
            let mut v = World::new(true);
            v.ctx = format!("seed {seed}, resumed");
            let acts = v.g.resume(last.as_bytes(), true).expect("a saved conversation loads");
            for a in acts {
                v.act(a);
            }
            v.reply_text("back");
            v.input("go on");
            assert_eq!(v.bodies.len(), 1, "{}", v.ctx);
            resumed += 1;
        }
    }
    assert!(resumed > 50, "only {resumed} saved conversations resumed");
    // the chaos did get as far as asking the model, a lot
    assert!(total_requests > 400, "only {total_requests} requests");
}

// ---- saved conversations ----

fn writes_to<'a>(w: &'a World, path: &str) -> Vec<&'a Op> {
    w.ops.iter().filter(|o| o.kind == Kind::Write && o.a == path).collect()
}

#[test]
fn saves_as_it_goes() {
    let mut w = World::saving(".barb/sessions/t.json");
    w.hold_http = true;
    w.input("hello");
    // the user's message is saved before the model answers
    let f = w.file(".barb/sessions/t.json").expect("saved");
    assert!(f.starts_with("{\"barb_session\":1,\"model\":\"claude-test\",\"cwd\":\"/work\",\"messages\":["), "{f}");
    assert!(f.contains("hello") && !f.contains("cache_control"), "{f}");
    let id = w.last_op(Kind::Http);
    w.hold_http = false;
    let r = mk_reply("tool_use", &format!("[{{\"type\":\"text\",\"text\":\"looking\"}},{}]", tool("t1", "bash", r#"{"command":"echo hi"}"#)));
    w.reply_text("done");
    w.procs.push(("echo", b"hi\n".to_vec(), 0, 0));
    w.http(id, 200, &r);
    let f = w.file(".barb/sessions/t.json").unwrap();
    assert!(f.contains("done") && f.contains("tool_result") && f.contains("looking"), "{f}");
    // one save per request and one per reply: request, reply (tool_use), request, reply
    assert_eq!(writes_to(&w, ".barb/sessions/t.json").len(), 4);
    // a retry does not save again
    let n = writes_to(&w, ".barb/sessions/t.json").len();
    w.reply(OVERLOADED, 529);
    w.reply_text("ok");
    w.input("more");
    let t = w.last_op(Kind::Timer);
    w.timer(t);
    assert_eq!(writes_to(&w, ".barb/sessions/t.json").len(), n + 2, "request and reply, not the retry");
    // an interrupt is saved as soon as it happens: while the model is asked...
    let mut w = World::saving("i.json");
    w.hold_http = true;
    w.input("first");
    w.interrupt();
    assert!(w.file("i.json").unwrap().contains("The user interrupted you here"));
    // ...and while tools run, with every call answered in the file
    let mut w = World::saving("j.json");
    w.hold_proc = true;
    w.reply_tools(&[tool("b1", "bash", r#"{"command":"sleep 100"}"#), tool("b2", "bash", r#"{"command":"echo never"}"#)]);
    w.input("go");
    w.interrupt();
    let f = w.file("j.json").unwrap();
    assert!(f.contains("Interrupted by the user") && f.contains("Not run: the user interrupted."), "{f}");

    // nothing is saved for a conversation that never started
    let mut w = World::saving("x.json");
    w.input("/help");
    w.eof();
    assert!(writes_to(&w, "x.json").is_empty());
    // and nothing at all without a file to save to
    let mut w = World::new(true);
    w.reply_text("hi");
    w.input("hello");
    assert!(w.ops.iter().all(|o| o.kind != Kind::Write));
}

#[test]
fn a_failed_save_is_said_once() {
    let mut w = World::saving("ro/t.json");
    w.fail_writes = true;
    w.reply_text("one");
    w.reply_text("two");
    w.input("a");
    w.input("b");
    let errs: Vec<_> = w.says.iter().filter(|x| x.0 == Style::Error).collect();
    assert_eq!(errs.len(), 1, "{:?}", w.says);
    assert!(errs[0].1.starts_with("Could not save the conversation to ro/t.json"), "{}", errs[0].1);
    assert_eq!(w.bodies.len(), 2, "the conversation goes on");
}

#[test]
fn clear_starts_a_new_file() {
    let mut w = World::saving("d/t.json");
    w.reply_text("one");
    w.input("first");
    assert_eq!(w.g.session_path(), b"d/t.json");
    w.input("/clear");
    w.input("/clear"); // nothing said since: still the same next file
    assert_eq!(w.g.session_path(), b"d/t-2.json");
    w.reply_text("two");
    w.input("second");
    assert!(w.file("d/t.json").unwrap().contains("first"));
    let f = w.file("d/t-2.json").unwrap();
    assert!(f.contains("second") && !f.contains("first"), "{f}");
    w.input("/clear");
    assert_eq!(w.g.session_path(), b"d/t-3.json");
}

/// A session file with these messages (JSON, comma-separated).
fn session_file(msgs: &str) -> String {
    format!("{{\"barb_session\":1,\"model\":\"m\",\"cwd\":\"/work\",\"messages\":[{msgs}]}}")
}

#[test]
fn resume() {
    let file = session_file(concat!(
        r#"{"role":"user","content":[{"type":"text","text":"fix the build"}]},"#,
        r#"{"role":"assistant","content":[{"type":"thinking","thinking":"plan it","signature":"x"},{"type":"text","text":"Looking."},"#,
        r#"{"type":"tool_use","id":"t1","name":"read_file","input":{"path":"Makefile"}}]},"#,
        r#"{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":"all:"},{"type":"text","text":"[barb] Background task 1 finished"}]},"#,
        r#"{"role":"assistant","content":[{"type":"text","text":"Fixed."}]}"#
    ));
    let mut w = World::new(true);
    for a in w.g.resume(file.as_bytes(), true).expect("loads") {
        w.act(a);
    }
    let shown: Vec<(Style, &str)> = w.says.iter().map(|x| (x.0, x.1.as_str())).collect();
    assert_eq!(
        shown,
        [(Style::User, "fix the build"), (Style::Assistant, "Looking."), (Style::Tool, "read_file Makefile"), (Style::Assistant, "Fixed.")],
        "the [barb] note and the tool result are not shown; thinking only with verbose"
    );
    assert!(w.ops.is_empty(), "resuming does nothing else");
    w.reply_text("ok");
    w.input("and the tests");
    let b = w.last_body().to_string();
    assert!(b.contains("fix the build") && b.contains("plan it") && b.contains("Fixed."), "{b}");
    assert!(b.contains("\"text\":\"[barb] This conversation was saved and is being resumed now."), "{b}");
    let note = b.find("being resumed").unwrap();
    assert!(note < b.find("and the tests").unwrap(), "the note comes before what the user typed");
    assert!(!b.contains("It was saved in"), "same directory");
    // the note is said once
    w.reply_text("ok");
    w.input("again");
    assert_eq!(w.last_body().matches("being resumed").count(), 1);

    // /clear forgets the note with the conversation
    let mut w = World::new(true);
    w.g.resume(file.as_bytes(), false).unwrap();
    w.input("/clear");
    w.reply_text("ok");
    w.input("new topic");
    assert!(!w.last_body().contains("being resumed"), "{}", w.last_body());

    // Not shown again: nothing said
    let mut w = World::new(true);
    assert!(w.g.resume(file.as_bytes(), false).unwrap().is_empty());
    // Not a session file: refused
    assert!(w.g.resume(b"{\"messages\":[]}", true).is_none());
    assert!(w.g.resume(b"not json", true).is_none());
}

#[test]
fn resume_shows_thinking_when_verbose_and_only_the_last_messages() {
    let mut cfg = test_cfg();
    cfg.verbose = true;
    let mut w = World::with(cfg);
    let mut msgs = Vec::new();
    for i in 0..30 {
        msgs.push(format!(r#"{{"role":"user","content":[{{"type":"text","text":"q{i}"}}]}}"#));
        msgs.push(format!(r#"{{"role":"assistant","content":[{{"type":"thinking","thinking":"t{i}","signature":"x"}},{{"type":"redacted_thinking","data":"zz"}},{{"type":"text","text":"a{i}"}}]}}"#));
    }
    for a in w.g.resume(session_file(&msgs.join(",")).as_bytes(), true).unwrap() {
        w.act(a);
    }
    assert_eq!(w.says[0], (Style::Info, "(20 earlier messages not shown)".to_string()));
    assert_eq!(w.says[1], (Style::User, "q10".to_string()));
    assert_eq!(w.says[2], (Style::Thinking, "t10".to_string()));
    assert_eq!(w.says[3], (Style::Thinking, "[redacted thinking]".to_string()));
    assert_eq!(w.says[4], (Style::Assistant, "a10".to_string()));
    assert!(!w.said("q9"));
}

#[test]
fn resume_after_a_crash_mid_tools() {
    // Closed while two tools ran: the reply with the calls was saved, the results were not.
    let file = session_file(concat!(
        r#"{"role":"user","content":[{"type":"text","text":"go"}]},"#,
        r#"{"role":"assistant","content":[{"type":"tool_use","id":"a","name":"bash","input":{"command":"make","description":"Build"}},"#,
        r#"{"type":"tool_use","id":"b","name":"bash","input":{"command":"sleep 9\nmore","run_in_background":true}}]}"#
    ));
    let mut w = World::new(true);
    for a in w.g.resume(file.as_bytes(), true).unwrap() {
        w.act(a);
    }
    assert!(w.says.contains(&(Style::Tool, "bash Build".to_string())));
    assert!(w.says.contains(&(Style::Tool, "bash (background) sleep 9".to_string())), "{:?}", w.says);
    w.reply_text("ok");
    w.input("what happened?");
    assert_eq!(w.bodies.len(), 1); // check_body: every tool_use has its result
    let (r, err) = w.result_of("a").unwrap();
    assert!(err && r.contains("Barb was closed"), "{r}");
    assert!(w.result_of("b").unwrap().1);
}

#[test]
fn resume_in_another_directory() {
    let file = session_file(r#"{"role":"user","content":[{"type":"text","text":"hi"}]}"#).replace("/work", "/old");
    let mut w = World::new(true);
    for a in w.g.resume(file.as_bytes(), false).unwrap() {
        w.act(a);
    }
    assert!(w.said("(this conversation was saved in /old)"));
    w.reply_text("ok");
    w.input("still there?");
    let b = w.last_body();
    assert!(b.contains("It was saved in /old; the working directory is now /work."), "{b}");
    // the unanswered message and the new one go together, as one user message
    assert!(b.contains("\"text\":\"hi\"},{\"type\":\"text\",\"text\":\"[barb] This conversation"), "{b}");
}

#[test]
fn verbose_shows_thinking() {
    let c = "[{\"type\":\"thinking\",\"thinking\":\"  let me see\\n\",\"signature\":\"s\"},{\"type\":\"text\",\"text\":\"Answer.\"}]";
    let mut w = World::new(true);
    w.reply(&mk_reply("end_turn", c), 200);
    w.input("q");
    assert!(!w.says.iter().any(|x| x.0 == Style::Thinking), "hidden by default");
    w.input("/verbose");
    assert!(w.said("Verbose: the model's thinking is shown."));
    w.reply(&mk_reply("end_turn", c), 200);
    w.input("q2");
    let t: Vec<_> = w.says.iter().filter(|x| x.0 == Style::Thinking).collect();
    assert_eq!(t.len(), 1);
    assert_eq!(t[0].1, "let me see", "trimmed");
    let i = w.says.iter().position(|x| x.0 == Style::Thinking).unwrap();
    assert_eq!(w.says[i + 1], (Style::Assistant, "Answer.".to_string()), "before the reply it led to");
    w.input("/verbose");
    assert!(w.said("Verbose off"));
    w.reply(&mk_reply("end_turn", c), 200);
    w.input("q3");
    assert_eq!(w.says.iter().filter(|x| x.0 == Style::Thinking).count(), 1);
    // empty thinking (some APIs send it) shows nothing
    let mut cfg = test_cfg();
    cfg.verbose = true;
    let mut w = World::with(cfg);
    w.reply(&mk_reply("end_turn", "[{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"s\"},{\"type\":\"text\",\"text\":\"A\"}]"), 200);
    w.input("q");
    assert!(!w.says.iter().any(|x| x.0 == Style::Thinking));
}
