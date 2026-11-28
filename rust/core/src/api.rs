//! The conversation and the Messages API: the transcript, the request body, the reply.
//!
//! The transcript is a list of messages, each a list of blocks, kept in the shape the API wants
//! them in. The rules the API enforces, and that this file and the agent keep so a request is
//! never refused for its shape:
//!
//! - after an assistant message with tool_use blocks, the next user message starts with a
//!   tool_result for every one of them, in any order;
//! - no empty text blocks and no empty messages (the builder drops them);
//! - consecutive messages of one role are fine (the API merges them), but the agent adds to the
//!   last user message instead of starting another;
//! - blocks the core does not know (thinking, and whatever comes next) are kept as raw JSON and
//!   sent back exactly as they came, because the API requires thinking blocks to come back
//!   unchanged.

use crate::json::{self, Reader, Tok, Writer};
use alloc::vec::Vec;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Block {
    Text(Vec<u8>),
    ToolUse { id: Vec<u8>, name: Vec<u8>, input: Vec<u8> },
    ToolResult { id: Vec<u8>, content: Vec<u8>, is_error: bool },
    /// A block the core does not know, as its JSON.
    Raw(Vec<u8>),
}

impl Block {
    fn emittable(&self) -> bool {
        match self {
            Block::Text(t) => !t.is_empty(),
            Block::ToolUse { .. } | Block::ToolResult { .. } => true,
            Block::Raw(j) => json::valid(j),
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Msg {
    pub assistant: bool,
    pub blocks: Vec<Block>,
}

impl Msg {
    /// Add text; empty text is skipped.
    pub fn add_text(&mut self, text: &[u8]) {
        if !text.is_empty() {
            self.blocks.push(Block::Text(text.to_vec()));
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct Transcript {
    pub msgs: Vec<Msg>,
}

impl Transcript {
    /// The user message to add blocks to: the last message if that is a user's, else a new one.
    pub fn user_msg(&mut self) -> &mut Msg {
        if !matches!(self.msgs.last(), Some(m) if !m.assistant) {
            self.msgs.push(Msg { assistant: false, blocks: Vec::new() });
        }
        self.msgs.last_mut().unwrap()
    }
}

pub struct Cfg<'a> {
    pub model: &'a [u8],
    pub max_tokens: i64,
    pub system: &'a [u8],
    /// A JSON array of tool definitions, as text; empty for none.
    pub tools: &'a [u8],
}

fn cache_mark(w: &mut Writer) {
    w.key("cache_control");
    w.obj();
    w.key("type");
    w.str("ephemeral");
    w.obj_end();
}

fn emit_block(w: &mut Writer, b: &Block, cache: bool) {
    match b {
        Block::Raw(j) => w.raw(j),
        Block::Text(t) => {
            w.obj();
            w.key("type");
            w.str("text");
            w.key("text");
            w.str(t);
            if cache {
                cache_mark(w);
            }
            w.obj_end();
        }
        Block::ToolUse { id, name, input } => {
            w.obj();
            w.key("type");
            w.str("tool_use");
            w.key("id");
            w.str(id);
            w.key("name");
            w.str(name);
            w.key("input");
            w.raw(if input.is_empty() { b"{}" } else { input });
            w.obj_end();
        }
        Block::ToolResult { id, content, is_error } => {
            w.obj();
            w.key("type");
            w.str("tool_result");
            w.key("tool_use_id");
            w.str(id);
            w.key("content");
            w.str(content);
            if *is_error {
                w.key("is_error");
                w.bool(true);
            }
            if cache {
                cache_mark(w);
            }
            w.obj_end();
        }
    }
}

/// The request body for POST /v1/messages. Two prompt-cache breakpoints are set, on the system
/// prompt (which caches the tools before it) and on the last block of the conversation, so a
/// turn pays full price only for what is new.
pub fn request(cfg: &Cfg, t: &Transcript) -> Vec<u8> {
    // The last block of the last message that has any gets the cache breakpoint.
    let mut last = None;
    for (mi, m) in t.msgs.iter().enumerate() {
        for (bi, b) in m.blocks.iter().enumerate() {
            if b.emittable() {
                last = Some((mi, bi));
            }
        }
    }

    let mut w = Writer::new();
    w.obj();
    w.key("model");
    w.str(cfg.model);
    w.key("max_tokens");
    w.int(cfg.max_tokens);
    if !cfg.system.is_empty() {
        w.key("system");
        w.arr();
        w.obj();
        w.key("type");
        w.str("text");
        w.key("text");
        w.str(cfg.system);
        cache_mark(&mut w);
        w.obj_end();
        w.arr_end();
    }
    if !cfg.tools.is_empty() {
        w.key("tools");
        w.raw(cfg.tools);
    }
    w.key("messages");
    w.arr();
    for (mi, m) in t.msgs.iter().enumerate() {
        if !m.blocks.iter().any(Block::emittable) {
            continue;
        }
        w.obj();
        w.key("role");
        w.str(if m.assistant { "assistant" } else { "user" });
        w.key("content");
        w.arr();
        for (bi, b) in m.blocks.iter().enumerate() {
            if !b.emittable() {
                continue;
            }
            let cache = last == Some((mi, bi))
                && matches!(b, Block::Text(_) | Block::ToolResult { .. });
            emit_block(&mut w, b, cache);
        }
        w.arr_end();
        w.obj_end();
    }
    w.arr_end();
    w.obj_end();
    debug_assert!(!w.bad);
    w.out
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Reply {
    /// An error object from the API.
    Error { kind: Vec<u8>, message: Vec<u8> },
    Message(Message),
}

#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub struct Message {
    /// The assistant message (not added to any transcript).
    pub msg: Msg,
    /// end_turn, tool_use, max_tokens, refusal, pause_turn, ... Empty if it was null.
    pub stop_reason: Vec<u8>,
    pub in_tok: i64,
    pub out_tok: i64,
    pub cache_read: i64,
    pub cache_write: i64,
}

fn get_str(obj: &[u8], key: &str) -> Option<Vec<u8>> {
    json::as_str(json::get(obj, key)?)
}

fn get_int(obj: &[u8], key: &str) -> i64 {
    json::get(obj, key).and_then(json::as_int).unwrap_or(0)
}

fn parse_content(content: &[u8], m: &mut Msg) -> Option<()> {
    let mut j = Reader::new(content);
    if j.next() != Tok::Arr {
        return None;
    }
    loop {
        let t = j.next();
        if t == Tok::ArrEnd {
            return Some(());
        }
        if t != Tok::Obj {
            return None;
        }
        let el = j.capture(t)?;
        let kind = get_str(el, "type").unwrap_or_default();
        match &kind[..] {
            b"text" => m.add_text(&get_str(el, "text")?),
            b"tool_use" => {
                let id = get_str(el, "id")?;
                let name = get_str(el, "name")?;
                let input = json::get(el, "input").unwrap_or(b"{}").to_vec();
                m.blocks.push(Block::ToolUse { id, name, input });
            }
            _ => m.blocks.push(Block::Raw(el.to_vec())),
        }
    }
}

/// Read a response body. None if it is neither a message nor an API error (not JSON, the wrong
/// shape).
pub fn parse(body: &[u8]) -> Option<Reply> {
    if !json::valid(body) {
        return None;
    }
    let kind = get_str(body, "type")?;
    if kind == b"error" {
        let e = json::get(body, "error")?;
        return Some(Reply::Error {
            kind: get_str(e, "type").unwrap_or_default(),
            message: get_str(e, "message").unwrap_or_default(),
        });
    }
    if kind != b"message" {
        return None;
    }
    let content = json::get(body, "content")?;
    let mut r = Message { msg: Msg { assistant: true, blocks: Vec::new() }, ..Default::default() };
    parse_content(content, &mut r.msg)?;
    // null (an unfinished stream) leaves it empty
    r.stop_reason = get_str(body, "stop_reason").unwrap_or_default();
    if let Some(u) = json::get(body, "usage") {
        r.in_tok = get_int(u, "input_tokens");
        r.out_tok = get_int(u, "output_tokens");
        r.cache_read = get_int(u, "cache_read_input_tokens");
        r.cache_write = get_int(u, "cache_creation_input_tokens");
    }
    Some(Reply::Message(r))
}
