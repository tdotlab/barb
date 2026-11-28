// Tests for the transcript, the request builder and the reply parser.
#include "check.h"
#include "../src/core/api.h"

static str_t build(arena_t* a, const api_cfg_t* cfg, const transcript_t* t) {
    sb_t b;
    sb_init(&b, a);
    CHECK(api_request(&b, cfg, t));
    return sb_str(&b);
}

static void test_request(void) {
    arena_t a;
    arena_init(&a, &test_mem);
    transcript_t t = { 0, 0 };
    api_cfg_t cfg = { str_z("claude-test"), 1000, str_z("You are Barb."), str_z("[{\"name\":\"bash\"}]"), 0 };

    // An empty conversation still builds (the agent never sends one, but it must not crash).
    str_t body = build(&a, &cfg, &t);
    CHECK(json_valid(body));

    msg_t* u = tr_user_msg(&a, &t);
    CHECK(tr_add_text(&a, u, str_z("hello \"world\"\n")));
    msg_t* m = tr_new_msg(&a, &t, 1);
    CHECK(tr_add_text(&a, m, str_z("let me look")));
    block_t* tu = block_new(&a, BK_TOOL_USE);
    tu->id = str_z("toolu_1"); tu->name = str_z("bash"); tu->input = str_z("{\"command\":\"ls\"}");
    msg_add(m, tu);
    block_t* raw = block_new(&a, BK_RAW);
    raw->text = str_z("{\"type\":\"thinking\",\"thinking\":\"hmm\",\"signature\":\"abc\"}");
    msg_add(m, raw);
    msg_t* r = tr_user_msg(&a, &t);
    CHECK(r != m);                                   // the last message was the assistant's: a new user one
    block_t* tr = block_new(&a, BK_TOOL_RESULT);
    tr->id = str_z("toolu_1"); tr->text = str_z("file.txt\n"); tr->is_error = 1;
    msg_add(r, tr);
    CHECK(tr_user_msg(&a, &t) == r);                 // now the last is a user message: reuse it

    body = build(&a, &cfg, &t);
    CHECK(json_valid(body));
    str_t v;
    CHECK(json_get(body, "model", &v)); CHECK_STR(v, "\"claude-test\"");
    CHECK(json_get(body, "max_tokens", &v)); CHECK_STR(v, "1000");
    CHECK(json_get(body, "tools", &v)); CHECK_STR(v, "[{\"name\":\"bash\"}]");
    CHECK(json_get(body, "system", &v));
    CHECK(str_find(v, str_z("\"cache_control\":{\"type\":\"ephemeral\"}")) != STR_NONE);
    CHECK(json_get(body, "messages", &v));
    CHECK_STR(v,
        "[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hello \\\"world\\\"\\n\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"let me look\"},"
        "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"bash\",\"input\":{\"command\":\"ls\"}},"
        "{\"type\":\"thinking\",\"thinking\":\"hmm\",\"signature\":\"abc\"}]},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_1\","
        "\"content\":\"file.txt\\n\",\"is_error\":true,\"cache_control\":{\"type\":\"ephemeral\"}}]}]");

    // Empty text blocks and empty messages are dropped, never sent.
    transcript_t t2 = { 0, 0 };
    msg_t* e = tr_user_msg(&a, &t2);
    CHECK(tr_add_text(&a, e, str_z("")));            // skipped at insertion
    block_t* eb = block_new(&a, BK_TEXT);            // forced in by hand: skipped at build
    msg_add(e, eb);
    msg_t* u2 = tr_new_msg(&a, &t2, 1);
    CHECK(tr_add_text(&a, u2, str_z("only this")));
    body = build(&a, &cfg, &t2);
    CHECK(json_get(body, "messages", &v));
    CHECK_STR(v, "[{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"only this\","
                 "\"cache_control\":{\"type\":\"ephemeral\"}}]}]");

    // A raw block that is not valid JSON is not sent (it would make the whole request invalid).
    transcript_t t3 = { 0, 0 };
    msg_t* g = tr_new_msg(&a, &t3, 1);
    block_t* bad = block_new(&a, BK_RAW);
    bad->text = str_z("{not json");
    msg_add(g, bad);
    CHECK(tr_add_text(&a, g, str_z("fine")));
    body = build(&a, &cfg, &t3);
    CHECK(json_valid(body));
    CHECK(str_find(body, str_z("not json")) == STR_NONE);

    // Bytes that are not UTF-8, as a tool might read from a binary file, cannot spoil the request.
    transcript_t t4 = { 0, 0 };
    msg_t* b4 = tr_user_msg(&a, &t4);
    tr_add_text(&a, b4, str_of("\xff\xfe binary \x80\x00 end", 17));
    body = build(&a, &cfg, &t4);
    CHECK(json_valid(body));

    // No system prompt and no tools: those keys are left out.
    api_cfg_t bare = { str_z("m"), 5, str_z(""), str_z(""), 0 };
    body = build(&a, &bare, &t4);
    CHECK(!json_get(body, "system", &v));
    CHECK(!json_get(body, "tools", &v));
    arena_free_all(&a);
    CHECK_INT(mem_live, 0);
}

static void test_reply(void) {
    arena_t a;
    arena_init(&a, &test_mem);
    api_reply_t r;

    str_t ok = str_z(
        "{\"id\":\"msg_01\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\","
        "\"content\":[{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"sig\"},"
        "{\"type\":\"text\",\"text\":\"I will list it.\\n\"},"
        "{\"type\":\"tool_use\",\"id\":\"toolu_9\",\"name\":\"bash\",\"input\":{\"command\":\"ls -la\",\"timeout_ms\":5000}}],"
        "\"stop_reason\":\"tool_use\",\"stop_sequence\":null,"
        "\"usage\":{\"input_tokens\":120,\"cache_creation_input_tokens\":7,\"cache_read_input_tokens\":100,\"output_tokens\":33}}");
    CHECK(api_parse(&a, ok, &r));
    CHECK(r.is_message);
    CHECK_STR(r.stop_reason, "tool_use");
    CHECK_INT(r.in_tok, 120); CHECK_INT(r.out_tok, 33); CHECK_INT(r.cache_read, 100); CHECK_INT(r.cache_write, 7);
    CHECK(r.msg && r.msg->assistant);
    block_t* b = r.msg->first;
    CHECK(b && b->kind == BK_RAW);
    CHECK_STR(b->text, "{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"sig\"}");
    b = b->next;
    CHECK(b && b->kind == BK_TEXT);
    CHECK_STR(b->text, "I will list it.\n");
    b = b->next;
    CHECK(b && b->kind == BK_TOOL_USE);
    CHECK_STR(b->id, "toolu_9"); CHECK_STR(b->name, "bash");
    CHECK_STR(b->input, "{\"command\":\"ls -la\",\"timeout_ms\":5000}");
    CHECK(b->next == 0);

    // The round trip: a parsed reply goes back into a request unchanged.
    transcript_t t = { 0, 0 };
    t.first = t.last = r.msg;
    sb_t sb;
    sb_init(&sb, &a);
    api_cfg_t cfg = { str_z("m"), 10, str_z(""), str_z(""), 0 };
    CHECK(api_request(&sb, &cfg, &t));
    str_t body = sb_str(&sb), v;
    CHECK(json_get(body, "messages", &v));
    CHECK_STR(v,
        "[{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"sig\"},"
        "{\"type\":\"text\",\"text\":\"I will list it.\\n\"},"
        "{\"type\":\"tool_use\",\"id\":\"toolu_9\",\"name\":\"bash\",\"input\":{\"command\":\"ls -la\",\"timeout_ms\":5000}}]}]");

    // An API error.
    CHECK(api_parse(&a, str_z("{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}"), &r));
    CHECK(!r.is_message);
    CHECK_STR(r.err_type, "overloaded_error"); CHECK_STR(r.err_msg, "Overloaded");

    // A reply with empty text blocks and no stop reason: accepted, the empty text is dropped.
    CHECK(api_parse(&a, str_z("{\"type\":\"message\",\"content\":[{\"type\":\"text\",\"text\":\"\"}],\"stop_reason\":null}"), &r));
    CHECK(r.is_message && r.msg->first == 0);
    CHECK_INT(r.stop_reason.n, 0);

    // Things that are not replies.
    static const char* junk[] = {
        "", "not json", "[]", "{}", "{\"type\":\"ping\"}", "{\"type\":\"message\"}",
        "{\"type\":\"message\",\"content\":{}}", "{\"type\":\"message\",\"content\":[1]}",
        "{\"type\":\"message\",\"content\":[{\"type\":\"text\"}]}",
        "{\"type\":\"message\",\"content\":[{\"type\":\"tool_use\",\"id\":\"x\"}]}",
        "{\"type\":\"error\"}", "<html>502 Bad Gateway</html>",
    };
    for (size_t i = 0; i < sizeof junk / sizeof *junk; i++) {
        if (api_parse(&a, str_z(junk[i]), &r)) printf("should be refused: %s\n", junk[i]);
        CHECK(!api_parse(&a, str_z(junk[i]), &r));
    }
    arena_free_all(&a);
    CHECK_INT(mem_live, 0);
}

// Out of memory at every possible point: never a crash, and a failure is reported, not hidden.
static void test_oom(void) {
    str_t ok = str_z(
        "{\"type\":\"message\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"},"
        "{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"bash\",\"input\":{\"command\":\"ls\"}}],"
        "\"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}");
    int failures = 0, successes = 0;
    for (long k = 0; k < 40; k++) {
        mem_count = 0;
        mem_fail_after = k;
        arena_t a;
        arena_init(&a, &test_mem);
        api_reply_t r;
        int got = api_parse(&a, ok, &r);
        if (got) successes++; else failures++;
        arena_free_all(&a);
    }
    mem_fail_after = -1;
    CHECK(failures > 0);
    CHECK(successes > 0);
    CHECK_INT(mem_live, 0);
}

// ---- saved sessions ----

static str_t save_str(arena_t* a, const transcript_t* t) {
    sb_t b;
    sb_init(&b, a);
    CHECK(api_session(&b, str_z("claude-x"), str_z("/home/t/p"), t));
    return sb_str(&b);
}

// The messages of a loaded session, written out again as a request would send them, without marks.
static str_t msgs_of(arena_t* a, const transcript_t* t) {
    str_t s = save_str(a, t), v;
    CHECK(json_get(s, "messages", &v));
    return v;
}

static void test_session(void) {
    arena_t a;
    arena_init(&a, &test_mem);

    // Thinking asked for: adaptive, with its summarized text.
    transcript_t t0 = { 0, 0 };
    tr_add_text(&a, tr_user_msg(&a, &t0), str_z("hi"));
    api_cfg_t think = { str_z("m"), 5, str_z(""), str_z(""), 1 };
    str_t body = build(&a, &think, &t0), v;
    CHECK(json_get(body, "thinking", &v));
    CHECK_STR(v, "{\"type\":\"adaptive\",\"display\":\"summarized\"}");
    api_cfg_t plain = { str_z("m"), 5, str_z(""), str_z(""), 0 };
    CHECK(!json_get(build(&a, &plain, &t0), "thinking", &v));

    // The round trip: everything a conversation holds comes back as it was, and no cache marks.
    transcript_t t = { 0, 0 };
    tr_add_text(&a, tr_user_msg(&a, &t), str_z("fix \"it\"\nplease"));
    msg_t* m = tr_new_msg(&a, &t, 1);
    block_t* raw = block_new(&a, BK_RAW);
    raw->text = str_z("{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"s\"}");
    msg_add(m, raw);
    tr_add_text(&a, m, str_z("looking"));
    block_t* tu = block_new(&a, BK_TOOL_USE);
    tu->id = str_z("t1"); tu->name = str_z("bash"); tu->input = str_z("{\"command\":\"ls\"}");
    msg_add(m, tu);
    msg_t* u = tr_user_msg(&a, &t);
    block_t* res = block_new(&a, BK_TOOL_RESULT);
    res->id = str_z("t1"); res->text = str_z("a\nb\n"); res->is_error = 1;
    msg_add(u, res);
    tr_add_text(&a, u, str_z("and then?"));
    str_t file = save_str(&a, &t);
    CHECK(json_valid(file));
    CHECK(str_find(file, str_z("cache_control")) == STR_NONE);
    CHECK(json_get(file, "barb_session", &v)); CHECK_STR(v, "1");
    CHECK(json_get(file, "model", &v)); CHECK_STR(v, "\"claude-x\"");
    CHECK(json_get(file, "cwd", &v)); CHECK_STR(v, "\"/home/t/p\"");
    session_t s;
    CHECK(api_load_session(&a, file, &s));
    CHECK_STR(s.model, "claude-x");
    CHECK_STR(s.cwd, "/home/t/p");
    CHECK_INT(tr_count(&s.tr), 3);
    CHECK(s.tr.first->next->first->kind == BK_RAW);
    CHECK(s.tr.last->first->kind == BK_TOOL_RESULT && s.tr.last->first->is_error);
    CHECK_STR(s.tr.last->first->text, "a\nb\n");
    CHECK(str_eq(save_str(&a, &s.tr), file));            // read and written again: the same bytes

    // No model or cwd: still a session, with those empty.
    CHECK(api_load_session(&a, str_z("{\"barb_session\":1,\"messages\":[]}"), &s));
    CHECK_INT(s.model.n, 0); CHECK_INT(s.cwd.n, 0); CHECK_INT(tr_count(&s.tr), 0);

    // Not sessions.
    static const char* junk[] = {
        "", "not json", "[]", "{}", "{\"messages\":[]}", "{\"barb_session\":2,\"messages\":[]}",
        "{\"barb_session\":0,\"messages\":[]}", "{\"barb_session\":\"1\",\"messages\":[]}",
        "{\"barb_session\":1}", "{\"barb_session\":1,\"messages\":{}}", "{\"barb_session\":1,\"messages\":[1]}",
        "{\"barb_session\":1,\"messages\":[{\"role\":\"system\",\"content\":[]}]}",
        "{\"barb_session\":1,\"messages\":[{\"content\":[]}]}",
        "{\"barb_session\":1,\"messages\":[{\"role\":\"user\"}]}",
        "{\"barb_session\":1,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
        "{\"barb_session\":1,\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\"}]}]}",
    };
    for (size_t i = 0; i < sizeof junk / sizeof *junk; i++) {
        if (api_load_session(&a, str_z(junk[i]), &s)) printf("should be refused: %s\n", junk[i]);
        CHECK(!api_load_session(&a, str_z(junk[i]), &s));
    }

    // Repair: an empty message goes and the two user messages around it become one; a tool call
    // left unanswered (Barb was closed while it ran) gets its result, before what the user said.
    CHECK(api_load_session(&a, str_z(
        "{\"barb_session\":1,\"messages\":["
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"one\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"\"}]},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"two\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"a\",\"name\":\"bash\",\"input\":{}},"
        "{\"type\":\"tool_use\",\"id\":\"b\",\"name\":\"bash\",\"input\":{}},{\"type\":\"tool_use\",\"id\":\"c\",\"name\":\"bash\",\"input\":{}}]},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"b\",\"content\":\"done\"},"
        "{\"type\":\"text\",\"text\":\"three\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"d\",\"name\":\"bash\"}]}]}"), &s));
    CHECK_STR(msgs_of(&a, &s.tr),
        "[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"one\"},{\"type\":\"text\",\"text\":\"two\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"a\",\"name\":\"bash\",\"input\":{}},"
        "{\"type\":\"tool_use\",\"id\":\"b\",\"name\":\"bash\",\"input\":{}},{\"type\":\"tool_use\",\"id\":\"c\",\"name\":\"bash\",\"input\":{}}]},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"a\",\"content\":\"Not run: Barb was closed before this tool call finished.\",\"is_error\":true},"
        "{\"type\":\"tool_result\",\"tool_use_id\":\"c\",\"content\":\"Not run: Barb was closed before this tool call finished.\",\"is_error\":true},"
        "{\"type\":\"tool_result\",\"tool_use_id\":\"b\",\"content\":\"done\"},{\"type\":\"text\",\"text\":\"three\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"d\",\"name\":\"bash\",\"input\":{}}]},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"d\",\"content\":\"Not run: Barb was closed before this tool call finished.\",\"is_error\":true}]}]");
    CHECK(s.tr.last && !s.tr.last->assistant);           // the added message is the last one now
    // Two assistant messages in a row become one, and its calls are still answered.
    CHECK(api_load_session(&a, str_z(
        "{\"barb_session\":1,\"messages\":["
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"go\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"x\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"e\",\"name\":\"bash\",\"input\":{}}]},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"e\",\"content\":[{\"type\":\"text\",\"text\":\"r\"}]}]}]}"), &s));
    CHECK_INT(tr_count(&s.tr), 3);
    CHECK(s.tr.first->next->first->next && s.tr.first->next->first->next->kind == BK_TOOL_USE);
    CHECK(s.tr.last->first->kind == BK_RAW);              // a result that is not a string is kept as it came
    CHECK(s.tr.last->first->next == 0);                   // and it answers e: no second result is added
    // But one for another call does not.
    CHECK(api_load_session(&a, str_z(
        "{\"barb_session\":1,\"messages\":["
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"go\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"e\",\"name\":\"bash\",\"input\":{}}]},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"ee\",\"content\":[]},"
        "{\"type\":\"tool_reslt\",\"tool_use_id\":\"e\",\"content\":[]}]}]}"), &s));
    CHECK(s.tr.last->first->kind == BK_TOOL_RESULT);
    CHECK_STR(s.tr.last->first->id, "e");
    arena_free_all(&a);
    CHECK_INT(mem_live, 0);
}

static void test_session_names(void) {
    char out[64];
    const int t[6] = { 2026, 11, 8, 7, 5, 3 };
    CHECK(session_new_name(out, t, 0)); CHECK_STR(str_z(out), "20261108-070503");
    CHECK(session_new_name(out, t, 1)); CHECK_STR(str_z(out), "20261108-070503-b");
    CHECK(session_new_name(out, t, 25)); CHECK_STR(str_z(out), "20261108-070503-z");
    CHECK(!session_new_name(out, t, 26));
    CHECK(!session_new_name(out, t, -1));
    const int leap[6] = { 1999, 12, 31, 23, 59, 60 };     // a leap second is shown as :00
    CHECK(session_new_name(out, leap, 0)); CHECK_STR(str_z(out), "19991231-235900");

    CHECK(session_file(out, sizeof out, "work", 0)); CHECK_STR(str_z(out), ".barb/sessions/work.json");
    CHECK(session_file(out, sizeof out, "work.json", 0)); CHECK_STR(str_z(out), ".barb/sessions/work.json");
    CHECK(session_file(out, sizeof out, "../x/old", 0)); CHECK_STR(str_z(out), "../x/old");
    CHECK(session_file(out, sizeof out, "a\\b", 0)); CHECK_STR(str_z(out), ".barb/sessions/a\\b.json");
    CHECK(session_file(out, sizeof out, "a\\b", 1)); CHECK_STR(str_z(out), "a\\b");
    CHECK(session_file(out, sizeof out, ".json", 0)); CHECK_STR(str_z(out), ".barb/sessions/.json");
    CHECK(session_file(out, 25, "work", 0));             // 24 bytes and the NUL: just fits
    CHECK(!session_file(out, 24, "work", 0));
    CHECK(session_file(out, 4, "a/b", 0) && !session_file(out, 3, "a/b", 0));

    arena_t a;
    arena_init(&a, &test_mem);
    size_t n;
    str_t first;
    CHECK(session_summary(&a, str_z(
        "{\"barb_session\":1,\"messages\":["
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"[barb] a note\"},{\"type\":\"text\",\"text\":\"Fix the build\\nand more\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"later\"}]}]}"), &n, &first));
    CHECK_INT(n, 3);
    CHECK_STR(first, "Fix the build");
    // Long: cut at 60 bytes, but never inside a character (each "é" is two bytes).
    char big[200] = "{\"barb_session\":1,\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"";
    for (int i = 0; i < 59; i++) strcat(big, "a");
    strcat(big, "\xc3\xa9\xc3\xa9\"}]}]}");
    CHECK(session_summary(&a, str_z(big), &n, &first));
    CHECK_INT(first.n, 59);
    big[strlen(big) - 11] = 0;                           // drop one "a": 58 + "é" fits in 60
    strcat(big, "\xc3\xa9\xc3\xa9\"}]}]}");
    CHECK(session_summary(&a, str_z(big), &n, &first));
    CHECK_INT(first.n, 60);
    // Exactly 60 plain bytes and more after: 60.
    char big2[200] = "{\"barb_session\":1,\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"";
    for (int i = 0; i < 70; i++) strcat(big2, "b");
    strcat(big2, "\"}]}]}");
    CHECK(session_summary(&a, str_z(big2), &n, &first));
    CHECK_INT(first.n, 60);
    // Nothing the user said: no first line, but still a session.
    CHECK(session_summary(&a, str_z("{\"barb_session\":1,\"messages\":[]}"), &n, &first));
    CHECK_INT(n, 0); CHECK_INT(first.n, 0);
    CHECK(!session_summary(&a, str_z("{\"barb_session\":9,\"messages\":[]}"), &n, &first));
    CHECK_INT(n, 0); CHECK_INT(first.n, 0);
    arena_free_all(&a);
    CHECK_INT(mem_live, 0);
}

int main(void) {
    test_request();
    test_reply();
    test_oom();
    test_session();
    test_session_names();
    return checks_report("test_api");
}
