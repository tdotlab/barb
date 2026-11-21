// Tests for the core's foundation: the arena, strings, and JSON.
// Built with the address and undefined-behaviour sanitizers.
#include "check.h"
#include "../src/core/json.h"

static void test_arena(void) {
    arena_t a;
    arena_init(&a, &test_mem);
    char* p = arena_alloc(&a, 10);
    CHECK(p != 0);
    CHECK(((uintptr_t)p & 15) == 0);
    int zero = 1;
    for (int i = 0; i < 10; i++) if (p[i]) zero = 0;
    CHECK(zero);
    // Many small allocations cross a chunk boundary and stay distinct.
    char* prev = p;
    int distinct = 1;
    for (int i = 0; i < 20000; i++) {
        char* q = arena_alloc(&a, 24);
        if (!q || q == prev) distinct = 0;
        prev = q;
    }
    CHECK(distinct);
    // A big request gets its own chunk and the head still serves small ones.
    char* big = arena_alloc(&a, 1 << 20);
    CHECK(big != 0);
    memset(big, 7, 1 << 20);
    CHECK(arena_alloc(&a, 8) != 0);
    CHECK(!a.failed);
    arena_free_all(&a);
    CHECK_INT(mem_live, 0);

    // Failure is sticky and reported, and the arena recovers after free_all.
    mem_count = 0;
    mem_fail_after = 0;
    arena_init(&a, &test_mem);
    CHECK(arena_alloc(&a, 8) == 0);
    CHECK(a.failed);
    mem_fail_after = -1;
    arena_free_all(&a);
    CHECK(!a.failed);
    CHECK(arena_alloc(&a, 8) != 0);
    arena_free_all(&a);
    CHECK(arena_alloc(&a, (size_t)-1) == 0);  // absurd sizes fail, not wrap
    CHECK(a.failed);
    arena_free_all(&a);
    CHECK_INT(mem_live, 0);
}

static void test_str(void) {
    arena_t a;
    arena_init(&a, &test_mem);
    CHECK(str_eqz(str_z("abc"), "abc"));
    CHECK(!str_eqz(str_z("abc"), "abd"));
    CHECK(!str_eqz(str_z("abc"), "ab"));
    CHECK(str_eq(str_of(0, 0), str_z("")));
    CHECK_INT(str_find(str_z("hello world"), str_z("o w")), 4);
    CHECK_INT(str_find(str_z("hello"), str_z("")), 0);
    CHECK(str_find(str_z("hello"), str_z("lo!")) == STR_NONE);
    CHECK(str_find(str_z("aab"), str_z("ab")) == 1);
    CHECK(str_find(str_z("ab"), str_z("abc")) == STR_NONE);
    CHECK_STR(str_trim(str_z("  \t x y \r\n")), "x y");
    CHECK(str_starts(str_z("/clear now"), str_z("/clear")));
    str_t d = str_dup(&a, str_z("copy"));
    CHECK_STR(d, "copy");
    CHECK(d.p[4] == 0);

    sb_t b;
    sb_init(&b, &a);
    CHECK_STR(sb_str(&b), "");
    sb_putz(&b, "n=");
    sb_puti(&b, -9223372036854775807LL - 1);
    sb_putc(&b, ' ');
    sb_putu(&b, 18446744073709551615ULL);
    sb_putc(&b, ' ');
    sb_pad_u(&b, 42, 6);
    CHECK_STR(sb_str(&b), "n=-9223372036854775808 18446744073709551615     42");
    for (int i = 0; i < 100000; i++) sb_putc(&b, 'x');
    CHECK_INT(sb_str(&b).n, 50 + 100000);  // 50 characters before the loop
    CHECK(!b.failed);
    arena_free_all(&a);

    // Out of memory makes a buffer fail cleanly.
    mem_count = 0;
    mem_fail_after = 1;
    arena_init(&a, &test_mem);
    sb_init(&b, &a);
    for (int i = 0; i < 200000; i++) sb_putc(&b, 'y');
    CHECK(b.failed);
    CHECK(sb_str(&b).p == 0);
    mem_fail_after = -1;
    arena_free_all(&a);
    CHECK_INT(mem_live, 0);
}

static int tokens(const char* s, char* kinds, size_t cap) {
    json_t j;
    json_open(&j, str_z(s));
    size_t n = 0;
    for (;;) {
        jtok_t t = json_next(&j);
        if (n + 1 < cap) kinds[n++] = (char)('0' + t);
        if (t == JT_END) { kinds[n] = 0; return 1; }
        if (t == JT_ERR) { kinds[n] = 0; return 0; }
    }
}

static void test_json_reader(void) {
    static const char* good[] = {
        "null", "true", "false", "0", "-0", "12", "-1.5e+10", "0.25E-3", "\"\"", "\"a\\n\\u00e9\\ud83d\\ude00\"",
        "[]", "{}", "[1,2,3]", "{\"a\":1}", " \n\t{ \"a\" : [ true , null , { \"b\" : \"c\" } ] } \r\n",
        "[[[[[[[[[[1]]]]]]]]]]", "{\"\":\"\"}", "[\"\\/\\\\\\\"\\b\\f\\r\\t\"]",
    };
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
        if (!json_valid(str_z(good[i]))) printf("should be valid: %s\n", good[i]);
        CHECK(json_valid(str_z(good[i])));
    }
    static const char* bad[] = {
        "", " ", "nul", "nulll", "truex", "tru", "True", "01", "-", "1.", ".5", "1e", "1e+", "+1", "0x10", "--1",
        "\"abc", "\"a\nb\"", "\"\\x\"", "\"\\u12\"", "\"\\u12g4\"", "\"\\", "[", "]", "{", "}", "[1,]", "[,1]", "[1 2]",
        "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{a:1}", "{\"a\" 1}", "{1:2}", "[1]]", "{} {}", "1 2", "[1}", "{\"a\":1]",
        "NaN", "Infinity", "'a'", "[\"a\"\"b\"]", "{\"a\":1 \"b\":2}", "\"\x01\"",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        if (json_valid(str_z(bad[i]))) printf("should be invalid: %s\n", bad[i]);
        CHECK(!json_valid(str_z(bad[i])));
    }
    // Depth: 64 levels are accepted, 65 refused, with no stack use that grows.
    char deep[200];
    memset(deep, '[', 64); memset(deep + 64, ']', 64); deep[128] = 0;
    CHECK(json_valid(str_z(deep)));
    memset(deep, '[', 65); memset(deep + 65, ']', 65); deep[130] = 0;
    CHECK(!json_valid(str_z(deep)));
    {
        static char big[100001];
        memset(big, '[', 100000);
        big[100000] = 0;
        CHECK(!json_valid(str_z(big)));  // refused at depth 65, quickly
    }

    char k[64];
    CHECK(tokens("{\"a\":[1,\"x\",true]}", k, sizeof k));
    // OBJ KEY ARR NUM STR TRUE ARR_END OBJ_END END, as the enum's numbers
    CHECK_STR(str_z(k), "264879531");
    CHECK(!tokens("[1,", k, sizeof k));

    // Values come out raw, and capture gives the exact text.
    json_t j;
    json_open(&j, str_z("  {\"k\": [1, {\"x\": \"y\"}] , \"s\":\"a\\nb\"}"));
    CHECK(json_next(&j) == JT_OBJ);
    CHECK(json_next(&j) == JT_KEY);
    CHECK_STR(j.tok, "k");
    jtok_t t = json_next(&j);
    str_t cap;
    CHECK(t == JT_ARR);
    CHECK(json_capture(&j, t, &cap));
    CHECK_STR(cap, "[1, {\"x\": \"y\"}]");
    CHECK(json_next(&j) == JT_KEY);
    t = json_next(&j);
    CHECK(t == JT_STR);
    CHECK(json_capture(&j, t, &cap));
    CHECK_STR(cap, "\"a\\nb\"");

    // Lookup by key.
    str_t v;
    str_t obj = str_z("{\"a\":1,\"b\":{\"a\":2},\"c\":\"s\",\"a\":3,\"t\":true}");
    CHECK(json_get(obj, "a", &v)); CHECK_STR(v, "1");
    CHECK(json_get(obj, "b", &v)); CHECK_STR(v, "{\"a\":2}");
    CHECK(json_get(obj, "c", &v)); CHECK_STR(v, "\"s\"");
    CHECK(!json_get(obj, "zz", &v));
    CHECK(!json_get(str_z("[1]"), "a", &v));
    CHECK(!json_get(str_z("{\"a\":"), "a", &v));
    CHECK(!json_get(str_z("{\"a\":1,"), "b", &v));  // truncated: not found, no crash
    int64_t n; int bl;
    CHECK(json_get(obj, "a", &v) && json_as_int(v, &n) && n == 1);
    CHECK(json_get(obj, "t", &v) && json_as_bool(v, &bl) && bl == 1);
    CHECK(!json_as_bool(str_z("1"), &bl));
}

static void test_json_numbers_strings(void) {
    int64_t n;
    CHECK(json_as_int(str_z("0"), &n) && n == 0);
    CHECK(json_as_int(str_z("-0"), &n) && n == 0);
    CHECK(json_as_int(str_z("9223372036854775807"), &n) && n == INT64_MAX);
    CHECK(json_as_int(str_z("-9223372036854775808"), &n) && n == INT64_MIN);
    CHECK(!json_as_int(str_z("9223372036854775808"), &n));
    CHECK(!json_as_int(str_z("-9223372036854775809"), &n));
    CHECK(!json_as_int(str_z("99999999999999999999999"), &n));
    CHECK(!json_as_int(str_z("1.5"), &n));
    CHECK(!json_as_int(str_z("1e3"), &n));
    CHECK(!json_as_int(str_z("-"), &n));
    CHECK(!json_as_int(str_z(""), &n));

    arena_t a;
    arena_init(&a, &test_mem);
    str_t s;
    CHECK(json_as_str(&a, str_z("\"a\\\"b\\\\c\\/d\\n\\t\\u0041\\u00e9\\u20ac\""), &s));
    CHECK_STR(s, "a\"b\\c/d\n\tA\xC3\xA9\xE2\x82\xAC");
    CHECK(json_as_str(&a, str_z("\"\\ud83d\\ude00\""), &s));
    CHECK_STR(s, "\xF0\x9F\x98\x80");                       // a surrogate pair is one character
    CHECK(json_as_str(&a, str_z("\"\\ud83dx\""), &s));
    CHECK_STR(s, "\xEF\xBF\xBD" "x");                        // a lone high surrogate
    CHECK(json_as_str(&a, str_z("\"\\ude00\""), &s));
    CHECK_STR(s, "\xEF\xBF\xBD");                            // a lone low surrogate
    CHECK(json_as_str(&a, str_z("\"\\ud83d\\u0041\""), &s));
    CHECK_STR(s, "\xEF\xBF\xBD" "A");                        // a high surrogate then a plain character
    CHECK(json_as_str(&a, str_z("\"\\u0000\""), &s));
    CHECK_INT(s.n, 1);                                       // an embedded NUL survives, with its length
    CHECK(!json_as_str(&a, str_z("abc"), &s));
    CHECK(!json_as_str(&a, str_z("\""), &s));
    CHECK(json_as_str(&a, str_z("\"\""), &s));
    CHECK_INT(s.n, 0);
    arena_free_all(&a);
}

// The writer: structure, escaping, and what it does with bytes that are not UTF-8.
static void test_json_writer(void) {
    arena_t a;
    arena_init(&a, &test_mem);
    sb_t b;
    jw_t w;
    sb_init(&b, &a);
    jw_init(&w, &b);
    jw_obj(&w);
    jw_key(&w, "s"); jw_strz(&w, "a\"b\\c\n\r\t\x01\x1f/");
    jw_key(&w, "n"); jw_int(&w, -42);
    jw_key(&w, "a"); jw_arr(&w); jw_int(&w, 1); jw_bool(&w, 0); jw_null(&w); jw_obj(&w); jw_obj_end(&w); jw_arr_end(&w);
    jw_key(&w, "r"); jw_raw(&w, str_z("{\"x\":[1,2]}"));
    jw_obj_end(&w);
    CHECK(!w.bad);
    CHECK_STR(sb_str(&b),
        "{\"s\":\"a\\\"b\\\\c\\n\\r\\t\\u0001\\u001f/\",\"n\":-42,\"a\":[1,false,null,{}],\"r\":{\"x\":[1,2]}}");
    CHECK(json_valid(sb_str(&b)));

    // Valid UTF-8 passes through; every kind of bad sequence becomes U+FFFD.
    struct { const char* in; size_t n; const char* want; } cases[] = {
        { "\xC3\xA9", 2, "\"\xC3\xA9\"" },                         // e acute
        { "\xE2\x82\xAC", 3, "\"\xE2\x82\xAC\"" },                 // euro
        { "\xF0\x9F\x98\x80", 4, "\"\xF0\x9F\x98\x80\"" },         // emoji
        { "\xF4\x8F\xBF\xBF", 4, "\"\xF4\x8F\xBF\xBF\"" },         // U+10FFFF, the last one
        { "\x80", 1, "\"\\ufffd\"" },                               // a stray continuation byte
        { "\xC0\x80", 2, "\"\\ufffd\\ufffd\"" },                    // overlong NUL
        { "\xC1\xBF", 2, "\"\\ufffd\\ufffd\"" },                    // overlong
        { "\xE0\x80\x80", 3, "\"\\ufffd\\ufffd\\ufffd\"" },         // overlong
        { "\xED\xA0\x80", 3, "\"\\ufffd\\ufffd\\ufffd\"" },         // a surrogate
        { "\xF4\x90\x80\x80", 4, "\"\\ufffd\\ufffd\\ufffd\\ufffd\"" }, // past U+10FFFF
        { "\xF8\x88\x80\x80\x80", 5, "\"\\ufffd\\ufffd\\ufffd\\ufffd\\ufffd\"" },
        { "\xE2\x82", 2, "\"\\ufffd\\ufffd\"" },                    // cut off
        { "ab\xFF" "cd", 5, "\"ab\\ufffdcd\"" },
        { "\xC3", 1, "\"\\ufffd\"" },
        { "\x7f", 1, "\"\x7f\"" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        arena_t t;
        arena_init(&t, &test_mem);
        sb_t tb;
        jw_t tw;
        sb_init(&tb, &t);
        jw_init(&tw, &tb);
        jw_str(&tw, str_of(cases[i].in, cases[i].n));
        str_t got = sb_str(&tb);
        if (!str_eqz(got, cases[i].want)) printf("utf-8 case %zu\n", i);
        CHECK_STR(got, cases[i].want);
        CHECK(json_valid(got));
        arena_free_all(&t);
    }

    // Round trip: any bytes at all, written then read back, give valid UTF-8 JSON that parses.
    unsigned seed = 12345;
    for (int round = 0; round < 300; round++) {
        char buf[300];
        size_t n = (size_t)(round % 290);
        for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; buf[i] = (char)(seed >> 16); }
        arena_t t;
        arena_init(&t, &test_mem);
        sb_t tb;
        jw_t tw;
        sb_init(&tb, &t);
        jw_init(&tw, &tb);
        jw_str(&tw, str_of(buf, n));
        str_t got = sb_str(&tb);
        str_t back;
        CHECK(json_valid(got));
        CHECK(json_as_str(&t, got, &back));
        // The decoded text is itself valid UTF-8: writing it again and reading it back
        // gives the same bytes (no second round of replacement).
        sb_t tb2;
        jw_t tw2;
        str_t back2;
        sb_init(&tb2, &t);
        jw_init(&tw2, &tb2);
        jw_str(&tw2, back);
        CHECK(json_as_str(&t, sb_str(&tb2), &back2));
        CHECK(str_eq(back, back2));
        arena_free_all(&t);
    }

    // Misuse is noticed, not crashed on.
    sb_init(&b, &a);
    jw_init(&w, &b);
    jw_obj_end(&w);
    CHECK(w.bad);
    arena_free_all(&a);
    CHECK_INT(mem_live, 0);
}

// Feed the reader and the lookups bytes that are almost JSON: it must neither crash
// (the sanitizers watch) nor read past the input, whatever it decides.
static void test_json_fuzz(void) {
    static const char* seeds[] = {
        "{\"id\":\"msg_01\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"hi \\u00e9\"},"
        "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"bash\",\"input\":{\"command\":\"ls -la\",\"timeout_ms\":5000}}],"
        "\"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}",
        "[1,2.5e3,-0,\"x\",[true,false,null],{\"a\":{\"b\":[]}}]",
    };
    unsigned seed = 99;
    long long ran = 0;
    for (size_t s = 0; s < sizeof seeds / sizeof *seeds; s++) {
        size_t len = strlen(seeds[s]);
        for (int round = 0; round < 20000; round++) {
            char* copy = malloc(len + 1);  // exact size, so ASan sees any read past the end
            memcpy(copy, seeds[s], len);
            seed = seed * 1103515245u + 12345u;
            int edits = 1 + (int)((seed >> 16) % 4);
            size_t use = len;
            for (int e = 0; e < edits; e++) {
                seed = seed * 1103515245u + 12345u;
                size_t at = (seed >> 8) % (use ? use : 1);
                seed = seed * 1103515245u + 12345u;
                switch ((seed >> 16) % 4) {
                case 0: copy[at] = (char)(seed >> 4); break;
                case 1: if (use) use = at; break;                        // truncate
                case 2: copy[at] = "{}[]\",:\\ 0e-."[(seed >> 5) % 14]; break;
                default: if (at + 1 < use) { copy[at] = copy[at + 1]; } break;
                }
            }
            str_t in = str_of(copy, use);
            char* exact = malloc(use ? use : 1);
            memcpy(exact, copy, use);
            in = str_of(exact, use);
            json_t j;
            json_open(&j, in);
            for (int steps = 0; steps < 10000; steps++) {
                jtok_t t = json_next(&j);
                if (t == JT_ERR || t == JT_END) break;
            }
            str_t v;
            json_get(in, "content", &v);
            json_get(in, "usage", &v);
            json_valid(in);
            free(exact);
            free(copy);
            ran++;
        }
    }
    CHECK(ran == 40000);
}

int main(void) {
    test_arena();
    test_str();
    test_json_reader();
    test_json_numbers_strings();
    test_json_writer();
    test_json_fuzz();
    return checks_report("test_core");
}
