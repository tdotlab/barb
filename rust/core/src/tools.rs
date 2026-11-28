//! The tools the model can call, and their definitions.
//!
//! These are written for a model to read. A tool's result says what happened in plain words, and
//! an error says what to do next ("read the file first", "add more context so old_string is
//! unique"), not just that something went wrong, so the model can correct itself in one step.
//! Output that could be huge is cut in the middle, where the least is usually lost, and says so.
//!
//! A tool is a small state machine because the core does no I/O: it asks the platform to read a
//! file and returns; the answer arrives as an event and the tool continues. `Tc::phase` is where
//! it is; [`begin`] starts it and [`event`] continues it. An event of a kind the phase does not
//! expect (an answer while waiting for a file) is ignored.
//!
//! The tool functions return `Err(message)` to fail the call: the message goes to the model as
//! an error result.

use crate::agent::{Action, Agent, Answer, Event, Exit, Fs, Style, Task, Wake, BIT_BASH, BIT_EDIT, BIT_WRITE};
use crate::json;
use crate::text::{self, Bytes};
use alloc::vec::Vec;

pub const TOOLS_JSON: &str = concat!(
    r#"[{"name":"read_file","#,
    r#""description":"Read a text file. Returns the lines, each prefixed with its line number and a tab. Paths are relative to the working directory, or absolute. By default up to 2000 lines are returned; use offset and limit to read further into a large file.","#,
    r#""input_schema":{"type":"object","properties":{"#,
    r#""path":{"type":"string","description":"The file to read."},"#,
    r#""offset":{"type":"integer","description":"The line to start at, counting from 1."},"#,
    r#""limit":{"type":"integer","description":"The most lines to return."}},"#,
    r#""required":["path"]}},"#,
    r#"{"name":"write_file","#,
    r#""description":"Create a file, or replace one completely. Parent directories are created. If the file already exists you must have read it first in this session. To change part of an existing file, use edit_file instead.","#,
    r#""input_schema":{"type":"object","properties":{"#,
    r#""path":{"type":"string","description":"The file to write."},"#,
    r#""content":{"type":"string","description":"The whole new contents of the file."}},"#,
    r#""required":["path","content"]}},"#,
    r#"{"name":"edit_file","#,
    r#""description":"Replace an exact piece of text in an existing file. old_string must match the file exactly, including whitespace, indentation and line endings, and must occur once unless replace_all is true: include a few neighbouring lines to make it unique. You must have read the file first in this session.","#,
    r#""input_schema":{"type":"object","properties":{"#,
    r#""path":{"type":"string","description":"The file to edit."},"#,
    r#""old_string":{"type":"string","description":"The exact text to replace."},"#,
    r#""new_string":{"type":"string","description":"The text to put in its place. May be empty to delete."},"#,
    r#""replace_all":{"type":"boolean","description":"Replace every occurrence instead of requiring exactly one."}},"#,
    r#""required":["path","old_string","new_string"]}},"#,
    r#"{"name":"bash","#,
    r#""description":"Run a shell command in the working directory. Standard output and standard error come back together, followed by the exit code. A command that fails (a non-zero exit code) is not an error in itself: read the output. Output longer than about 30000 characters is cut in the middle. For anything that may take more than a minute or two, set run_in_background: you get a task number at once, the output goes to a log file, and you are woken with a message when it exits.","#,
    r#""input_schema":{"type":"object","properties":{"#,
    r#""command":{"type":"string","description":"The command line, run with sh -c."},"#,
    r#""description":{"type":"string","description":"A few words on what the command does, shown to the user."},"#,
    r#""timeout_ms":{"type":"integer","description":"How long to let it run before it is killed. Default 120000, at most 600000. Ignored for background tasks."},"#,
    r#""run_in_background":{"type":"boolean","description":"Start it and return immediately; you will be woken when it exits."}},"#,
    r#""required":["command"]}},"#,
    r#"{"name":"task_stop","#,
    r#""description":"Stop a background task started with bash and run_in_background.","#,
    r#""input_schema":{"type":"object","properties":{"#,
    r#""task_id":{"type":"integer","description":"The task number you were given when it started."}},"#,
    r#""required":["task_id"]}},"#,
    r#"{"name":"schedule_wakeup","#,
    r#""description":"Wake yourself after a delay. Barb sends you a message at that time. Nothing runs and no tokens are spent while you wait, so end your turn after calling this. Use it to check back on something later.","#,
    r#""input_schema":{"type":"object","properties":{"#,
    r#""seconds":{"type":"integer","description":"How long to wait, from 1 to 86400."},"#,
    r#""note":{"type":"string","description":"What you want to remember when you wake."}},"#,
    r#""required":["seconds"]}}]"#,
);

const READ_DEFAULT_LINES: i64 = 2000;
const READ_MAX_LINES: i64 = 10000;
const READ_MAX_LINE_BYTES: usize = 2000;
const READ_MAX_BYTES: usize = 100000;
pub const OUTPUT_MAX: usize = 30000;
const BASH_DEFAULT_MS: i64 = 120000;
const BASH_MAX_MS: i64 = 600000;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub(crate) enum Phase {
    /// Not started.
    #[default]
    Idle,
    /// The call is finished.
    Done,
    Read,
    WriteCheck,
    WriteAsk,
    WriteWrote,
    EditRead,
    EditAsk,
    EditWrote,
    BashAsk,
    BashRun,
}

/// The running tool call's state; emptied when the call ends.
#[derive(Default)]
pub(crate) struct Tc {
    pub phase: Phase,
    /// The operation it waits for.
    pub op: i64,
    path: Bytes,
    s1: Bytes,
    s2: Bytes,
    data: Bytes,
    flag: bool,
    n: i64,
    n2: i64,
}

impl Tc {
    pub fn done() -> Tc {
        Tc { phase: Phase::Done, ..Tc::default() }
    }
    pub fn finished(&self) -> bool {
        self.phase == Phase::Done
    }
}

type R = Result<(), Bytes>;

// ---- helpers ----

/// `s` cut to about `max` bytes by dropping the middle, without splitting a UTF-8 character.
pub fn cap_output(s: &[u8], max: usize) -> Bytes {
    if s.len() <= max {
        return s.to_vec();
    }
    let mut head = max / 2;
    let mut tail_from = s.len() - max / 2;
    while head > 0 && text::is_continuation(s[head]) {
        head -= 1;
    }
    while tail_from < s.len() && text::is_continuation(s[tail_from]) {
        tail_from += 1;
    }
    cat!(
        s[..head],
        "\n\n... [",
        tail_from - head,
        " bytes omitted from the middle] ...\n\n",
        s[tail_from..]
    )
}

fn missing(key: &str, ty: &str) -> Bytes {
    cat!("Missing required argument '", key, "' (", ty, ").")
}

fn wrong(key: &str, ty: &str) -> Bytes {
    cat!("Argument '", key, "' must be ", ty, ".")
}

/// The input must be a JSON object; then each argument is looked up by name.
fn input_ok(input: &[u8]) -> R {
    let t = text::trim(input);
    if !json::valid(t) || t.first() != Some(&b'{') {
        return Err(b"The tool input must be a JSON object with the arguments in the tool's schema.".to_vec());
    }
    Ok(())
}

fn arg_str(input: &[u8], key: &str, required: bool) -> Result<Option<Bytes>, Bytes> {
    match json::get(input, key) {
        None if required => Err(missing(key, "a string")),
        None => Ok(None),
        Some(raw) => json::as_str(raw).map(Some).ok_or_else(|| wrong(key, "a string")),
    }
}

fn arg_int(input: &[u8], key: &str, required: bool) -> Result<Option<i64>, Bytes> {
    match json::get(input, key) {
        None if required => Err(missing(key, "an integer")),
        None => Ok(None),
        Some(raw) => json::as_int(raw).map(Some).ok_or_else(|| wrong(key, "an integer")),
    }
}

fn arg_bool(input: &[u8], key: &str) -> Result<bool, Bytes> {
    match json::get(input, key) {
        None => Ok(false),
        Some(raw) => json::as_bool(raw).ok_or_else(|| wrong(key, "true or false")),
    }
}

/// Drop "./", doubled slashes and a trailing slash, so "src//a.c" and "./src/a.c" are one file.
pub fn norm_path(p: &[u8]) -> Bytes {
    let mut b = Bytes::new();
    let abs = p.first() == Some(&b'/');
    if abs {
        b.push(b'/');
    }
    let mut first = true;
    for seg in p.split(|&c| c == b'/') {
        if seg.is_empty() || seg == b"." {
            continue;
        }
        if !first {
            b.push(b'/');
        }
        b.extend_from_slice(seg);
        first = false;
    }
    if first && !abs {
        b.push(b'.');
    }
    b
}

fn arg_path(input: &[u8]) -> Result<Bytes, Bytes> {
    let raw = arg_str(input, "path", true)?.unwrap_or_default();
    if raw.is_empty() {
        return Err(wrong("path", "a non-empty file path"));
    }
    if raw.contains(&0) {
        return Err(wrong("path", "a file path without NUL characters"));
    }
    Ok(norm_path(&raw))
}

fn needs_ask(g: &Agent, bit: u32) -> bool {
    !g.cfg.auto_approve && g.always & bit == 0
}

fn ask(g: &mut Agent, what: Bytes, next: Phase) {
    let id = g.new_id();
    g.tc.op = id;
    g.tc.phase = next;
    let tool = g.calls[g.cur].name.clone();
    g.emit(Action::Ask { id, tool, what });
}

/// The answer to a question: Ok to go on, Err if the user said no.
fn answered(g: &mut Agent, a: Answer, bit: u32) -> R {
    match a {
        Answer::No => Err(b"The user declined this action. Do not try the same thing again; ask them what they would like instead."
            .to_vec()),
        Answer::Always => {
            g.always |= bit;
            Ok(())
        }
        Answer::Yes => Ok(()),
    }
}

/// The line a tool call is shown as ("read_file src/main.rs"), from its name and input: for a
/// call that is shown again, when a saved conversation is resumed.
pub(crate) fn label(name: &[u8], input: &[u8]) -> Bytes {
    let s = |k: &str| json::get(input, k).and_then(json::as_str).unwrap_or_default();
    let mut b = name.to_vec();
    match name {
        b"read_file" | b"edit_file" | b"write_file" => push!(b; " ", s("path")),
        b"bash" => {
            let bg = json::get(input, "run_in_background").and_then(json::as_bool) == Some(true);
            let desc = s("description");
            let cmd = s("command");
            push!(b; " ", if bg { "(background) " } else { "" });
            push!(b; if desc.is_empty() { text::first_line(&cmd, 100) } else { &desc[..] });
        }
        _ => {}
    }
    b
}

fn say_tool(g: &mut Agent, name: &str, what: &[u8]) {
    let t = cat!(name, " ", what);
    g.say(Style::Tool, t);
}

/// A file-system failure in words the model can act on.
fn fs_fail(verb: &str, path: &[u8], status: Fs, note: &[u8]) -> Bytes {
    match status {
        Fs::NotFound => cat!(
            "No such file: ",
            path,
            ". Paths are relative to the working directory; check with bash (ls, find)."
        ),
        Fs::IsDir => cat!(path, " is a directory, not a file. List it with bash (ls)."),
        Fs::TooBig => {
            let mut b = cat!(path, " is too large to read whole");
            if !note.is_empty() {
                push!(b; " (", note, ")");
            }
            push!(b; ". Look at parts of it with bash (head, tail, grep -n, sed -n).");
            b
        }
        Fs::Denied => cat!("Permission denied ", verb, " ", path, "."),
        _ => {
            let mut b = cat!("Could not ", verb, " ", path);
            if !note.is_empty() {
                push!(b; ": ", note);
            }
            b
        }
    }
}

/// Up to `max_lines` lines of `s`, each after `prefix`, cut at 120 bytes; for the user's question.
fn preview(b: &mut Bytes, prefix: &str, s: &[u8], max_lines: usize) {
    let mut lines = 0;
    let mut i = 0;
    while i < s.len() && lines < max_lines {
        let j = s[i..].iter().position(|&c| c == b'\n').map_or(s.len(), |k| i + k);
        let len = j - i;
        push!(*b; prefix, s[i..i + len.min(120)]);
        if len > 120 {
            push!(*b; " ...");
        }
        b.push(b'\n');
        lines += 1;
        i = j + 1;
    }
    if i < s.len() {
        push!(*b; "  ... (more)\n");
    }
}

fn count_lines(s: &[u8]) -> u64 {
    let n = s.iter().filter(|&&c| c == b'\n').count() as u64;
    n + (s.last().is_some_and(|&c| c != b'\n') as u64)
}

fn line_at(s: &[u8], off: usize) -> u64 {
    1 + s[..off.min(s.len())].iter().filter(|&&c| c == b'\n').count() as u64
}

/// A few numbered lines around an edit, so the model can see the result without another read.
fn snippet(b: &mut Bytes, s: &[u8], from: u64, to: u64) {
    let (mut line, mut shown, mut i) = (1u64, 0, 0usize);
    while i <= s.len() && line <= to && shown < 24 {
        let j = s[i..].iter().position(|&c| c == b'\n').map_or(s.len(), |k| i + k);
        if line >= from && i != s.len() {
            text::pad(b, line, 6);
            b.push(b'\t');
            push!(*b; s[i..i + (j - i).min(200)]);
            b.push(b'\n');
            shown += 1;
        }
        if j >= s.len() {
            break;
        }
        i = j + 1;
        line += 1;
    }
}

// ---- dispatch ----

/// Start the current call.
pub(crate) fn begin(g: &mut Agent) {
    let name = g.calls[g.cur].name.clone();
    let input = g.calls[g.cur].input.clone();
    let r = match &name[..] {
        b"read_file" => read_begin(g, &input),
        b"write_file" => write_begin(g, &input),
        b"edit_file" => edit_begin(g, &input),
        b"bash" => bash_begin(g, &input),
        b"task_stop" => task_stop(g, &input),
        b"schedule_wakeup" => wakeup(g, &input),
        _ => Err(cat!(
            "There is no tool named '",
            name,
            "'. The tools are read_file, write_file, edit_file, bash, task_stop and schedule_wakeup."
        )),
    };
    if let Err(m) = r {
        g.finish_call(m, true, None);
    }
}

/// Continue the current call with the event that answers its operation.
pub(crate) fn event(g: &mut Agent, ev: &Event) {
    let r = match (g.tc.phase, *ev) {
        (Phase::Read, Event::File { status, data, note, .. }) => read_done(g, status, data, note),
        (Phase::WriteCheck, Event::File { status, data: _, note, .. }) => write_checked(g, status, note),
        (Phase::WriteAsk, Event::Answer { answer, .. }) => answered(g, answer, BIT_WRITE).map(|_| write_go(g)),
        (Phase::WriteWrote, Event::File { status, note, .. }) => write_wrote(g, status, note),
        (Phase::EditRead, Event::File { status, data, note, .. }) => edit_read(g, status, data, note),
        (Phase::EditAsk, Event::Answer { answer, .. }) => answered(g, answer, BIT_EDIT).map(|_| edit_go(g)),
        (Phase::EditWrote, Event::File { status, note, .. }) => edit_wrote(g, status, note),
        (Phase::BashAsk, Event::Answer { answer, .. }) => answered(g, answer, BIT_BASH).and_then(|_| bash_go(g)),
        (Phase::BashRun, Event::Proc { exit, out, note, .. }) => bash_done(g, exit, out, note),
        _ => Ok(()), // not what this step waits for
    };
    if let Err(m) = r {
        g.finish_call(m, true, None);
    }
}

// ---- read_file ----

fn read_begin(g: &mut Agent, input: &[u8]) -> R {
    input_ok(input)?;
    let path = arg_path(input)?;
    let offset = arg_int(input, "offset", false)?;
    let limit = arg_int(input, "limit", false)?;
    let offset = offset.filter(|&o| o >= 1).unwrap_or(1);
    let limit = limit.filter(|&l| l >= 1).unwrap_or(READ_DEFAULT_LINES).min(READ_MAX_LINES);
    say_tool(g, "read_file", &path);
    let id = g.new_id();
    g.emit(Action::Read { id, path: path.clone() });
    g.tc = Tc { phase: Phase::Read, op: id, path, n: offset, n2: limit, ..Tc::default() };
    Ok(())
}

fn read_done(g: &mut Agent, status: Fs, d: &[u8], note: &[u8]) -> R {
    let path = core::mem::take(&mut g.tc.path);
    if status != Fs::Ok {
        return Err(fs_fail("read", &path, status, note));
    }
    g.seen_add(&path);
    if d[..d.len().min(8192)].contains(&0) {
        return Err(cat!(
            path,
            " looks like a binary file (it contains NUL bytes), so it was not read. Use bash (file, xxd, strings) if you need to look at it."
        ));
    }
    if d.is_empty() {
        g.finish_call(b"(the file is empty)".to_vec(), false, Some(b"empty file"));
        return Ok(());
    }

    let total = count_lines(d);
    let (offset, limit) = (g.tc.n, g.tc.n2);
    if offset as u64 > total {
        return Err(cat!("offset ", offset, " is past the end of the file, which has ", total, " lines."));
    }
    let mut out = Bytes::new();
    let (mut lineno, mut shown, mut last_shown) = (0u64, 0i64, 0u64);
    let mut cut_short = false;
    let mut i = 0;
    while i < d.len() {
        let j = d[i..].iter().position(|&c| c == b'\n').map_or(d.len(), |k| i + k);
        lineno += 1;
        if lineno as i64 >= offset {
            if shown >= limit {
                break;
            }
            if out.len() >= READ_MAX_BYTES {
                cut_short = true;
                break;
            }
            let mut len = j - i;
            if len > 0 && d[i + len - 1] == b'\r' {
                len -= 1; // CRLF files read as plain lines
            }
            text::pad(&mut out, lineno, 6);
            out.push(b'\t');
            if len > READ_MAX_LINE_BYTES {
                let mut cut = READ_MAX_LINE_BYTES;
                while cut > 0 && text::is_continuation(d[i + cut]) {
                    cut -= 1;
                }
                push!(out; d[i..i + cut], " ... [line cut: it is ", len, " bytes long]");
            } else {
                push!(out; d[i..i + len]);
            }
            out.push(b'\n');
            shown += 1;
            last_shown = lineno;
        }
        i = j + 1;
    }
    if last_shown < total {
        push!(out;
            "\n(showing lines ", offset, "-", last_shown, " of ", total,
            if cut_short { "; stopped at the output size limit" } else { "" },
            "; call read_file again with offset ", last_shown + 1, " to continue)"
        );
    }
    let sum = cat!(shown, " lines");
    g.finish_call(out, false, Some(&sum));
    Ok(())
}

// ---- write_file ----

fn write_begin(g: &mut Agent, input: &[u8]) -> R {
    input_ok(input)?;
    let path = arg_path(input)?;
    let content = arg_str(input, "content", true)?.unwrap_or_default();
    let w = cat!(path, " (", content.len(), " bytes)");
    say_tool(g, "write_file", &w);
    // Look at what is there first: never silently replace a file the model has not read.
    let id = g.new_id();
    g.emit(Action::Read { id, path: path.clone() });
    g.tc = Tc { phase: Phase::WriteCheck, op: id, path, data: content, ..Tc::default() };
    Ok(())
}

fn write_checked(g: &mut Agent, status: Fs, note: &[u8]) -> R {
    let path = &g.tc.path;
    let seen = g.seen_has(path);
    let exists = match status {
        Fs::Ok if !seen => {
            return Err(cat!(
                path,
                " already exists and you have not read it in this session. Read it first, so you do not overwrite something you have not seen, or use edit_file for a targeted change."
            ))
        }
        Fs::Ok => true,
        Fs::NotFound => false,
        Fs::TooBig if seen => true,
        _ => return Err(fs_fail("write", path, status, note)),
    };
    g.tc.flag = exists;
    if needs_ask(g, BIT_WRITE) {
        let t = &g.tc;
        let mut d = cat!(if exists { "overwrite " } else { "create " }, t.path, " (", t.data.len(), " bytes)\n");
        preview(&mut d, "+ ", &t.data, 6);
        ask(g, d, Phase::WriteAsk);
        return Ok(());
    }
    write_go(g);
    Ok(())
}

fn write_go(g: &mut Agent) {
    let id = g.new_id();
    g.tc.op = id;
    g.tc.phase = Phase::WriteWrote;
    let (path, data) = (g.tc.path.clone(), g.tc.data.clone());
    g.emit(Action::Write { id, path, data });
}

fn write_wrote(g: &mut Agent, status: Fs, note: &[u8]) -> R {
    let t = &g.tc;
    if status != Fs::Ok {
        return Err(fs_fail("write", &t.path, status, note));
    }
    let r = cat!(if t.flag { "Replaced " } else { "Created " }, t.path, " (", t.data.len(), " bytes).");
    let sum: &[u8] = if t.flag { b"replaced" } else { b"created" };
    let path = t.path.clone();
    g.seen_add(&path);
    g.finish_call(r, false, Some(sum));
    Ok(())
}

// ---- edit_file ----

fn edit_begin(g: &mut Agent, input: &[u8]) -> R {
    input_ok(input)?;
    let path = arg_path(input)?;
    let olds = arg_str(input, "old_string", true)?.unwrap_or_default();
    let news = arg_str(input, "new_string", true)?.unwrap_or_default();
    let all = arg_bool(input, "replace_all")?;
    if olds.is_empty() {
        return Err(b"old_string must not be empty. To create a new file use write_file; to change a file give the exact text to replace."
            .to_vec());
    }
    if olds == news {
        return Err(b"old_string and new_string are identical, so there is nothing to change.".to_vec());
    }
    say_tool(g, "edit_file", &path);
    let id = g.new_id();
    g.emit(Action::Read { id, path: path.clone() });
    g.tc = Tc { phase: Phase::EditRead, op: id, path, s1: olds, s2: news, flag: all, ..Tc::default() };
    Ok(())
}

fn edit_read(g: &mut Agent, status: Fs, file: &[u8], note: &[u8]) -> R {
    let t = &g.tc;
    if status == Fs::NotFound {
        return Err(cat!(
            "No such file: ",
            t.path,
            ". edit_file only changes existing files; to create one use write_file."
        ));
    }
    if status != Fs::Ok {
        return Err(fs_fail("edit", &t.path, status, note));
    }
    if !g.seen_has(&t.path) {
        return Err(cat!(
            "You have not read ",
            t.path,
            " in this session. Read it first with read_file (so your old_string comes from the real text), then edit."
        ));
    }
    let (olds, news) = (&t.s1, &t.s2);

    // Find every occurrence.
    let mut count = 0u64;
    let mut at = Bytes::new();
    let mut pos = 0;
    while let Some(k) = text::find(&file[pos..], olds) {
        if count < 6 {
            if count > 0 {
                push!(at; ", ");
            }
            push!(at; line_at(file, pos + k));
        }
        count += 1;
        pos += k + olds.len();
    }
    if count == 0 {
        let mut b = cat!(
            "old_string was not found in ",
            t.path,
            ". It must match the file exactly, including whitespace, indentation and line endings."
        );
        let trimmed = text::trim(olds);
        if !trimmed.is_empty() && trimmed.len() != olds.len() && text::contains(file, trimmed) {
            push!(b; " The text is there, but the whitespace around it differs from what you gave.");
        } else if file.contains(&b'\r') && !olds.contains(&b'\r') && olds.contains(&b'\n') {
            push!(b; " This file uses CRLF line endings; a multi-line old_string has to include the carriage returns.");
        } else {
            push!(b; " Re-read the lines you want to change and copy them exactly.");
        }
        return Err(b);
    }
    if count > 1 && !t.flag {
        return Err(cat!(
            "old_string appears ",
            count,
            " times in ",
            t.path,
            " (at lines ",
            at,
            if count > 6 { ", ...)" } else { ")" },
            ". Add some surrounding lines to old_string so it matches exactly one place, or set replace_all to true to change every occurrence."
        ));
    }

    // Build the new file.
    let mut out = Bytes::with_capacity(file.len());
    let (mut pos, mut done, mut new_first) = (0, 0u64, 0);
    while let Some(k) = text::find(&file[pos..], olds) {
        if done > 0 && !t.flag {
            break;
        }
        out.extend_from_slice(&file[pos..pos + k]);
        if done == 0 {
            new_first = out.len();
        }
        out.extend_from_slice(news);
        pos += k + olds.len();
        done += 1;
    }
    out.extend_from_slice(&file[pos..]);
    let n2 = line_at(&out, new_first) as i64;

    if needs_ask(g, BIT_EDIT) {
        let mut d = cat!("edit ", t.path, " (", done, if done == 1 { " place)\n" } else { " places)\n" });
        preview(&mut d, "- ", olds, 6);
        preview(&mut d, "+ ", news, 6);
        let t = &mut g.tc;
        t.data = out;
        t.n = done as i64;
        t.n2 = n2;
        ask(g, d, Phase::EditAsk);
        return Ok(());
    }
    let t = &mut g.tc;
    t.data = out;
    t.n = done as i64;
    t.n2 = n2;
    edit_go(g);
    Ok(())
}

fn edit_go(g: &mut Agent) {
    let id = g.new_id();
    g.tc.op = id;
    g.tc.phase = Phase::EditWrote;
    let (path, data) = (g.tc.path.clone(), g.tc.data.clone());
    g.emit(Action::Write { id, path, data });
}

fn edit_wrote(g: &mut Agent, status: Fs, note: &[u8]) -> R {
    let t = &g.tc;
    if status != Fs::Ok {
        return Err(fs_fail("write", &t.path, status, note));
    }
    let mut b = cat!(
        "Edited ",
        t.path,
        ": replaced ",
        t.n,
        if t.n == 1 { " occurrence." } else { " occurrences." },
        " The first change now reads:\n"
    );
    let added = count_lines(&t.s2);
    let n2 = t.n2 as u64;
    let from = if n2 > 2 { n2 - 2 } else { 1 };
    snippet(&mut b, &t.data, from, n2 + added.saturating_sub(1) + 2);
    g.finish_call(b, false, Some(b"edited"));
    Ok(())
}

// ---- bash ----

fn bash_begin(g: &mut Agent, input: &[u8]) -> R {
    input_ok(input)?;
    let cmd = arg_str(input, "command", true)?.unwrap_or_default();
    if text::trim(&cmd).is_empty() {
        return Err(b"The command is empty.".to_vec());
    }
    if cmd.contains(&0) {
        return Err(wrong("command", "text without NUL characters"));
    }
    let desc = arg_str(input, "description", false)?.unwrap_or_default();
    let timeout = arg_int(input, "timeout_ms", false)?;
    let bg = arg_bool(input, "run_in_background")?;
    let timeout = match timeout {
        None => BASH_DEFAULT_MS,
        Some(t) => t.clamp(1000, BASH_MAX_MS),
    };
    let mut w = cat!(if bg { "(background) " } else { "" });
    push!(w; if desc.is_empty() { text::first_line(&cmd, 100) } else { &desc[..] });
    say_tool(g, "bash", &w);
    let ask_first = needs_ask(g, BIT_BASH);
    let mut d = Bytes::new();
    if ask_first {
        d = cat!(if bg { "run in the background:\n" } else { "run:\n" }, cmd);
        if !desc.is_empty() {
            push!(d; "\n(", desc, ")");
        }
    }
    g.tc = Tc { phase: Phase::BashAsk, s1: cmd, n: timeout, flag: bg, ..Tc::default() };
    if ask_first {
        ask(g, d, Phase::BashAsk);
        return Ok(());
    }
    bash_go(g)
}

fn bash_go(g: &mut Agent) -> R {
    let cmd = g.tc.s1.clone();
    if g.tc.flag {
        let id = g.new_id();
        let log = cat!(".barb/tasks/", id, ".log");
        g.tasks.insert(0, Task { id, cmd: cmd.clone(), log: log.clone() });
        let r = cat!(
            "Started background task ",
            id,
            ". Its output is written to ",
            log,
            " (read it with read_file or tail it with bash). You will be woken with a message when it exits; ",
            "you do not need to poll. If there is nothing else to do meanwhile, end your turn: waiting costs nothing."
        );
        g.emit(Action::Spawn { id, cmd, timeout_ms: 0, background: true, log });
        let s = cat!("started task ", id);
        g.finish_call(r, false, Some(&s));
        return Ok(());
    }
    let id = g.new_id();
    g.tc.op = id;
    g.tc.phase = Phase::BashRun;
    let timeout_ms = g.tc.n;
    g.emit(Action::Spawn { id, cmd, timeout_ms, background: false, log: Vec::new() });
    Ok(())
}

fn bash_done(g: &mut Agent, exit: Exit, out: &[u8], note: &[u8]) -> R {
    if exit == Exit::Failed {
        let mut r = cat!("The command could not be started");
        if !note.is_empty() {
            push!(r; ": ", note);
        }
        return Err(r);
    }
    let out = cap_output(out, OUTPUT_MAX);
    let mut r = Bytes::new();
    if out.is_empty() {
        push!(r; "(no output)\n");
    } else {
        push!(r; out);
        if out.last() != Some(&b'\n') {
            r.push(b'\n');
        }
    }
    let n = g.tc.n;
    let (is_error, sum) = match exit {
        Exit::TimedOut => {
            // The limit as the model gave it: 1.5 seconds, not "1 seconds".
            push!(r; "[killed: it ran longer than its limit of ", n / 1000);
            if n % 1000 != 0 {
                push!(r; ".", n % 1000 / 100);
            }
            push!(r;
                if n == 1000 { " second" } else { " seconds" },
                ". Use run_in_background for long commands, or a longer timeout_ms.]"
            );
            (true, cat!("timed out"))
        }
        Exit::Signaled => {
            push!(r; "[killed by a signal]");
            (false, cat!("killed"))
        }
        Exit::Code(c) => {
            push!(r; "[exit code: ", c, "]");
            (false, cat!("exit ", c))
        }
        Exit::Failed => unreachable!(),
    };
    g.finish_call(r, is_error, Some(&sum));
    Ok(())
}

// ---- task_stop and schedule_wakeup ----

fn task_stop(g: &mut Agent, input: &[u8]) -> R {
    input_ok(input)?;
    let id = arg_int(input, "task_id", true)?.unwrap_or(0);
    if g.tasks.iter().any(|t| t.id == id) {
        g.emit(Action::Cancel { id });
        let r = cat!("Asked task ", id, " to stop. You will get a message when it has exited.");
        g.finish_call(r, false, Some(b"stopping task"));
        return Ok(());
    }
    let mut r = cat!("No running background task has the number ", id, ".");
    if g.tasks.is_empty() {
        push!(r; " None are running.");
    } else {
        push!(r; " Running now:");
        for t in &g.tasks {
            push!(r; " ", t.id);
        }
        push!(r; ".");
    }
    Err(r)
}

fn wakeup(g: &mut Agent, input: &[u8]) -> R {
    input_ok(input)?;
    let secs = arg_int(input, "seconds", true)?.unwrap_or(0);
    let note = arg_str(input, "note", false)?.unwrap_or_default();
    if !(1..=86400).contains(&secs) {
        return Err(b"seconds must be between 1 and 86400.".to_vec());
    }
    let id = g.new_id();
    g.wakes.push(Wake { id, note });
    g.emit(Action::Timer { id, ms: secs * 1000 });
    let r = cat!(
        "Timer set: you will be woken in ",
        secs,
        " seconds with a message. End your turn now if there is nothing else to do; waiting costs nothing. ",
        "(The timer only runs while barb is running.)"
    );
    let s = cat!("wake in ", secs, "s");
    g.finish_call(r, false, Some(&s));
    Ok(())
}
