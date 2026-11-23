// The transcript, the request builder and the reply parser.
use barb_core::api::{self, Block, Cfg, Msg, Reply, Transcript};
use barb_core::json;

fn s(x: &[u8]) -> &str {
    core::str::from_utf8(x).unwrap()
}

#[test]
fn request() {
    let cfg = Cfg {
        model: b"claude-test",
        max_tokens: 1000,
        system: b"You are Barb.",
        tools: b"[{\"name\":\"bash\"}]",
    };
    let mut t = Transcript::default();

    // An empty conversation still builds (the agent never sends one).
    assert!(json::valid(&api::request(&cfg, &t)));

    t.user_msg().add_text(b"hello \"world\"\n");
    let mut m = Msg { assistant: true, blocks: vec![] };
    m.add_text(b"let me look");
    m.blocks.push(Block::ToolUse {
        id: b"toolu_1".to_vec(),
        name: b"bash".to_vec(),
        input: b"{\"command\":\"ls\"}".to_vec(),
    });
    m.blocks.push(Block::Raw(b"{\"type\":\"thinking\",\"thinking\":\"hmm\",\"signature\":\"abc\"}".to_vec()));
    t.msgs.push(m);
    // The last message is the assistant's: a new user one.
    t.user_msg().blocks.push(Block::ToolResult {
        id: b"toolu_1".to_vec(),
        content: b"file.txt\n".to_vec(),
        is_error: true,
    });
    assert_eq!(t.msgs.len(), 3);
    t.user_msg(); // now the last is a user message: reused
    assert_eq!(t.msgs.len(), 3);

    let body = api::request(&cfg, &t);
    assert!(json::valid(&body));
    assert_eq!(s(json::get(&body, "model").unwrap()), "\"claude-test\"");
    assert_eq!(s(json::get(&body, "max_tokens").unwrap()), "1000");
    assert_eq!(s(json::get(&body, "tools").unwrap()), "[{\"name\":\"bash\"}]");
    let sys = json::get(&body, "system").unwrap();
    assert_eq!(
        s(sys),
        "[{\"type\":\"text\",\"text\":\"You are Barb.\",\"cache_control\":{\"type\":\"ephemeral\"}}]"
    );
    assert_eq!(
        s(json::get(&body, "messages").unwrap()),
        concat!(
            "[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hello \\\"world\\\"\\n\"}]},",
            "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"let me look\"},",
            "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"bash\",\"input\":{\"command\":\"ls\"}},",
            "{\"type\":\"thinking\",\"thinking\":\"hmm\",\"signature\":\"abc\"}]},",
            "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_1\",",
            "\"content\":\"file.txt\\n\",\"is_error\":true,\"cache_control\":{\"type\":\"ephemeral\"}}]}]"
        )
    );
    // The keys come in the order the C wrote them.
    assert!(s(&body).starts_with("{\"model\":\"claude-test\",\"max_tokens\":1000,\"system\":"));

    // Empty text blocks and empty messages are dropped, never sent.
    let mut t2 = Transcript::default();
    let e = t2.user_msg();
    e.add_text(b""); // skipped at insertion
    assert!(e.blocks.is_empty());
    e.blocks.push(Block::Text(vec![])); // forced in by hand: skipped at build
    let mut a = Msg { assistant: true, blocks: vec![] };
    a.add_text(b"only this");
    t2.msgs.push(a);
    assert_eq!(
        s(json::get(&api::request(&cfg, &t2), "messages").unwrap()),
        "[{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"only this\",\"cache_control\":{\"type\":\"ephemeral\"}}]}]"
    );

    // A raw block that is not valid JSON is not sent (it would make the whole request invalid).
    // The cache mark then goes on the last block that is sent. A tool_use never carries one.
    let mut t3 = Transcript::default();
    t3.msgs.push(Msg {
        assistant: true,
        blocks: vec![Block::Text(b"fine".to_vec()), Block::Raw(b"{not json".to_vec())],
    });
    let body = api::request(&cfg, &t3);
    assert!(json::valid(&body));
    assert!(!s(&body).contains("not json"));
    assert!(s(&body).contains("\"text\":\"fine\",\"cache_control\""));
    t3.msgs[0].blocks.push(Block::ToolUse { id: b"t".to_vec(), name: b"x".to_vec(), input: vec![] });
    let body = api::request(&cfg, &t3);
    assert!(s(&body).contains("\"input\":{}"));
    assert_eq!(s(&body).matches("cache_control").count(), 1); // the system prompt's only

    // Bytes that are not UTF-8, as a tool might read from a binary file, cannot spoil the request.
    let mut t4 = Transcript::default();
    t4.user_msg().add_text(b"\xff\xfe binary \x80\x00 end");
    assert!(json::valid(&api::request(&cfg, &t4)));

    // No system prompt and no tools: those keys are left out.
    let bare = Cfg { model: b"m", max_tokens: 5, system: b"", tools: b"" };
    let body = api::request(&bare, &t4);
    assert!(json::get(&body, "system").is_none());
    assert!(json::get(&body, "tools").is_none());
}

#[test]
fn reply() {
    let ok = concat!(
        "{\"id\":\"msg_01\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",",
        "\"content\":[{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"sig\"},",
        "{\"type\":\"text\",\"text\":\"I will list it.\\n\"},",
        "{\"type\":\"tool_use\",\"id\":\"toolu_9\",\"name\":\"bash\",\"input\":{\"command\":\"ls -la\",\"timeout_ms\":5000}}],",
        "\"stop_reason\":\"tool_use\",\"stop_sequence\":null,",
        "\"usage\":{\"input_tokens\":120,\"cache_creation_input_tokens\":7,\"cache_read_input_tokens\":100,\"output_tokens\":33}}"
    );
    let Some(Reply::Message(r)) = api::parse(ok.as_bytes()) else { panic!("not a message") };
    assert_eq!(r.stop_reason, b"tool_use");
    assert_eq!((r.in_tok, r.out_tok, r.cache_read, r.cache_write), (120, 33, 100, 7));
    assert!(r.msg.assistant);
    assert_eq!(
        r.msg.blocks,
        vec![
            Block::Raw(b"{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"sig\"}".to_vec()),
            Block::Text(b"I will list it.\n".to_vec()),
            Block::ToolUse {
                id: b"toolu_9".to_vec(),
                name: b"bash".to_vec(),
                input: b"{\"command\":\"ls -la\",\"timeout_ms\":5000}".to_vec()
            },
        ]
    );

    // The round trip: a parsed reply goes back into a request unchanged.
    let t = Transcript { msgs: vec![r.msg] };
    let cfg = Cfg { model: b"m", max_tokens: 10, system: b"", tools: b"" };
    assert_eq!(
        s(json::get(&api::request(&cfg, &t), "messages").unwrap()),
        concat!(
            "[{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"sig\"},",
            "{\"type\":\"text\",\"text\":\"I will list it.\\n\"},",
            "{\"type\":\"tool_use\",\"id\":\"toolu_9\",\"name\":\"bash\",\"input\":{\"command\":\"ls -la\",\"timeout_ms\":5000}}]}]"
        )
    );

    // An API error.
    assert_eq!(
        api::parse(b"{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}"),
        Some(Reply::Error { kind: b"overloaded_error".to_vec(), message: b"Overloaded".to_vec() })
    );

    // A reply with an empty text block and no stop reason: accepted, the empty text dropped.
    let Some(Reply::Message(r)) =
        api::parse(b"{\"type\":\"message\",\"content\":[{\"type\":\"text\",\"text\":\"\"}],\"stop_reason\":null}")
    else {
        panic!()
    };
    assert!(r.msg.blocks.is_empty());
    assert!(r.stop_reason.is_empty());
    // A tool_use without input gets {}.
    let Some(Reply::Message(r)) = api::parse(
        b"{\"type\":\"message\",\"content\":[{\"type\":\"tool_use\",\"id\":\"a\",\"name\":\"b\"}]}",
    ) else {
        panic!()
    };
    assert_eq!(r.msg.blocks, vec![Block::ToolUse { id: b"a".to_vec(), name: b"b".to_vec(), input: b"{}".to_vec() }]);

    // Things that are not replies.
    let junk = [
        "", "not json", "[]", "{}", "{\"type\":\"ping\"}", "{\"type\":\"message\"}",
        "{\"type\":\"message\",\"content\":{}}", "{\"type\":\"message\",\"content\":[1]}",
        "{\"type\":\"message\",\"content\":[{\"type\":\"text\"}]}",
        "{\"type\":\"message\",\"content\":[{\"type\":\"tool_use\",\"id\":\"x\"}]}",
        "{\"type\":\"error\"}", "<html>502 Bad Gateway</html>",
    ];
    for j in junk {
        assert_eq!(api::parse(j.as_bytes()), None, "should be refused: {j}");
    }
}
