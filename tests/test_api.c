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
    api_cfg_t cfg = { str_z("claude-test"), 1000, str_z("You are Barb."), str_z("[{\"name\":\"bash\"}]") };

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
    api_cfg_t bare = { str_z("m"), 5, str_z(""), str_z("") };
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
    api_cfg_t cfg = { str_z("m"), 10, str_z(""), str_z("") };
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

int main(void) {
    test_request();
    test_reply();
    test_oom();
    return checks_report("test_api");
}
