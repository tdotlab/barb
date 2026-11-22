// Tests for the terminal's screen (src/ui/screen.c): the record, the block at the bottom with its
// activity line, rule, prompt and footer, the one-line status, and the plain output of PIPE mode.
//
// The screen writes bytes for a terminal, so the tests play them on the small terminal of term.h
// (with output processing, as a terminal in raw mode still has: "\n" starts a new row) and look
// at the rows. A random run at the end mixes everything and checks the screen after each step.
#define TR 60
#define TC 100
#include "check.h"
#include "../src/ui/screen.h"
#include "term.h"

static term_t t;
static sc_t sc;
static char out[1 << 20];      // everything written, in order
static size_t outn;
static char fd1[1 << 18], fd2[1 << 18];
static size_t fd1n, fd2n;

static void keep(char* b, size_t* bn, size_t cap, const char* p, size_t n) {
    if (*bn + n + 1 > cap) { printf("test output overflow\n"); exit(2); }
    memcpy(b + *bn, p, n);
    *bn += n;
    b[*bn] = 0;
}

static void wfn(void* ctx, int fd, const char* p, size_t n) {
    (void)ctx;
    keep(out, &outn, sizeof out, p, n);
    if (fd == 1) keep(fd1, &fd1n, sizeof fd1, p, n);
    else keep(fd2, &fd2n, sizeof fd2, p, n);
    for (size_t i = 0; i < n;) {   // a terminal's output processing: "\n" is "\r\n"
        size_t e = i;
        while (e < n && p[e] != '\n') e++;
        t_feed(&t, p + i, (int)(e - i));
        if (e < n) t_feed(&t, "\r\n", 2);
        i = e + 1;
    }
}

static void clockfn(void* ctx, int64_t at, char* hm) {
    (void)ctx;
    int m = (int)(at / 60000 % 1440);
    hm[0] = (char)('0' + m / 600);
    hm[1] = (char)('0' + m / 60 % 10);
    hm[2] = ':';
    hm[3] = (char)('0' + m % 60 / 10);
    hm[4] = (char)('0' + m % 10);
    hm[5] = 0;
}

static void reset_out(void) { outn = fd1n = fd2n = 0; out[0] = fd1[0] = fd2[0] = 0; }

static void setup_full(int mode, int vt, int width, int deferred, int ascii, const char* model, const char* cwd, const char* home) {
    reset_out();
    t_init(&t, width, deferred);
    t.scroll = 1;
    sc_cfg_t c;
    memset(&c, 0, sizeof c);
    c.mode = mode;
    c.vt_out = c.vt_err = vt;
    c.fancy = !ascii;
    c.ascii = ascii;
    c.width = width;
    c.model = model;
    c.cwd = cwd;
    c.home = home;
    c.write = wfn;
    c.clock = clockfn;
    sc_init(&sc, &c);
}

static void setup(int mode, int vt, int width) { setup_full(mode, vt, width, 1, 0, "m1", "/w", 0); }

static const char* row(int r) {
    static char b[8][TC * 4 + 1];
    static int k;
    k = (k + 1) % 8;
    t_row(&t, r, b[k]);
    return b[k];
}

static int find_row(const char* text) {
    for (int r = 0; r < TR; r++) if (strstr(row(r), text)) return r;
    return -1;
}

static void dump(const char* why) {
    printf("--- %s (cursor %d,%d bad %d)\n", why, t.r, t.c, t.bad);
    int last = 0;
    for (int r = 0; r < TR; r++) if (*row(r)) last = r;
    for (int r = 0; r <= last + 1 && r < TR; r++) printf("%2d|%s|\n", r, row(r));
}

static int has(const char* text) { return strstr(out, text) != 0; }

static void say(int style, const char* p) { sc_say(&sc, style, p, strlen(p)); }

static act_t act(actk_t kind, int64_t id, int flag, int64_t n, const char* a) {
    act_t x;
    memset(&x, 0, sizeof x);
    x.kind = kind;
    x.id = id;
    x.flag = flag;
    x.n = n;
    x.a = str_z(a ? a : "");
    return x;
}

static void do_act(actk_t kind, int64_t id, int flag, int64_t n, const char* a, int64_t now) {
    act_t x = act(kind, id, flag, n, a);
    sc_action(&sc, &x, now);
}

static void do_ev(evk_t kind, int64_t id, int status) {
    ev_t e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.id = id;
    e.status = status;
    sc_event(&sc, &e);
}

static void type(const char* s) {
    for (; *s; s++) sc_key(&sc, EK_CHAR, (uint32_t)(unsigned char)*s);
}

// Every escape is a CSI the terminal knows: colours, erasing, moving the cursor.
static int escapes_ok(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != 0x1B) continue;
        if (i + 1 >= n || s[i + 1] != '[') return 0;
        size_t j = i + 2;
        while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) j++;
        if (j >= n || !strchr("mKJABC", s[j])) return 0;
        i = j;
    }
    return 1;
}

// ---- PIPE: as barb always wrote it ----

static void test_pipe(void) {
    setup(SC_PIPE, 0, 80);
    sc_banner(&sc, "0.2.0");
    say(SAY_TOOL, "bash ls");
    say(SAY_TOOL, "  exit 0");
    say(SAY_INFO, "a note");
    say(SAY_ERROR, "  error: nope");
    sc_question(&sc, "write_file", 10, "create a.txt", 12);
    sc_answer(&sc, ANS_YES);
    do_act(ACT_SAY, 0, SAY_USAGE, 11, "[tokens: 5 in, 6 out]", 0);
    say(SAY_ASSISTANT, "**hi**\n");
    CHECK(!strcmp(fd2,
        "barb 0.2.0, model m1, in /w\n/help for commands; Ctrl-D to exit.\n"
        "> bash ls\n  exit 0\na note\n  error: nope\n"
        "\nwrite_file wants to:\ncreate a.txt\nAllow? [y]es / [n]o / [a]lways this session: yes\n"
        "[tokens: 5 in, 6 out]\n"));
    CHECK(!strcmp(fd1, "**hi**\n\n"));
    CHECK(!strchr(out, 0x1B));
    size_t before = outn;
    do_act(ACT_HTTP, 1, 0, 0, "/v1/messages", 0);
    sc_draw(&sc, 5000, 1, 0, 1);
    CHECK(outn == before);     // no status line
    CHECK_INT(sc_next_ms(&sc, 5000), -1);
    sc_hide(&sc);
    CHECK(outn == before);

    // A terminal on standard output alone: replies are drawn as markdown there, the rest is plain.
    setup(SC_PIPE, 0, 80);
    sc.cfg.vt_out = 1;
    say(SAY_ASSISTANT, "some **bold** words");
    CHECK(strstr(fd1, "bold") && !strstr(fd1, "**"));
    CHECK(fd1[0] != '\n');      // no blank line before the first thing
}

// ---- LITE: styled, with a status line ----

static void test_lite(void) {
    setup(SC_LITE, 1, 80);
    sc_banner(&sc, "0.2.0");
    CHECK(!strcmp(row(0), "barb 0.2.0 \xC2\xB7 m1 \xC2\xB7 /w"));
    CHECK(!strcmp(row(1), "/help for commands \xC2\xB7 Ctrl-D to exit"));
    say(SAY_TOOL, "bash ls -la");
    CHECK(!strcmp(row(2), ""));
    CHECK(!strcmp(row(3), "\xE2\x97\x8F bash ls -la"));
    CHECK(has("\033[1mbash\033[0m ls -la"));
    say(SAY_TOOL, "  exit 0, 3 lines");
    CHECK(!strcmp(row(4), "  \xE2\x94\x94 exit 0, 3 lines"));
    say(SAY_ERROR, "  error: no such file\nsecond line");
    CHECK(!strcmp(row(5), "  \xE2\x94\x94 error: no such file"));
    CHECK(!strcmp(row(6), "    second line"));
    say(SAY_ASSISTANT, "Some **bold** text\n");
    CHECK(!strcmp(row(7), ""));
    CHECK(!strcmp(row(8), "Some bold text"));
    CHECK(!strcmp(row(9), ""));
    say(SAY_INFO, "first note");
    say(SAY_INFO, "second note");
    say(SAY_ERROR, "an error");
    CHECK(!strcmp(row(9), ""));
    CHECK(!strcmp(row(10), "first note"));
    CHECK(!strcmp(row(11), "second note"));
    CHECK(!strcmp(row(12), "an error"));
    CHECK(has("\033[31man error\033[0m"));
    CHECK(t.r == 13 && t.c == 0);

    // The status line, while a request is out.
    do_act(ACT_HTTP, 1, 0, 0, "/v1/messages", 1000);
    sc_draw(&sc, 1000, 1, 0, 1);
    CHECK(strstr(row(13), "waiting for the model") != 0);
    CHECK(!strstr(row(13), "\xC2\xB7"));        // no time until a second has passed
    size_t before = outn;
    sc_draw(&sc, 1010, 1, 0, 1);
    CHECK(outn == before);                      // nothing changed, nothing written
    CHECK_INT(sc_next_ms(&sc, 1010), 120 - 1010 % 120);
    sc_draw(&sc, 13000, 1, 0, 1);
    CHECK(strstr(row(13), "waiting for the model \xC2\xB7 12s") != 0);
    CHECK(t.r == 13);
    sc_draw(&sc, 13000, 1, 0, 0);               // a question is coming: it goes
    CHECK(!strcmp(row(13), ""));
    CHECK_INT(sc_next_ms(&sc, 13000), -1);
    sc_draw(&sc, 14000, 1, 0, 1);
    say(SAY_INFO, "a third");                   // output takes its place
    CHECK(!strcmp(row(13), "a third"));
    CHECK(find_row("waiting") < 0);
    do_act(ACT_SAY, 0, SAY_USAGE, 11, "[tokens: 5 in, 6 out]", 0);
    CHECK(!strcmp(row(14), "[tokens: 5 in, 6 out]"));
    do_ev(EV_HTTP, 1, 200);
    sc_draw(&sc, 15000, 0, 0, 1);
    CHECK(!strcmp(row(15), ""));
    CHECK_INT(sc_next_ms(&sc, 15000), -1);
    // A narrow terminal: the status line is cut to fit, and never wraps.
    setup(SC_LITE, 1, 20);
    do_act(ACT_SPAWN, 2, 0, 1000, "a-very-long-command-line --with arguments", 0);
    sc_draw(&sc, 0, 1, 0, 1);
    CHECK(ed_text_cols(row(0), strlen(row(0))) <= 19);
    CHECK(!strcmp(row(1), ""));
    CHECK(escapes_ok(out, outn));
}

// ---- REGION: the block ----

static void check_block(int top, const char* activity, const char* line, const char* footer_left, const char* hint) {
    if (strcmp(row(top), activity)) { dump(activity); }
    CHECK(!strcmp(row(top), activity));
    const char* r = row(top + 1);
    CHECK(ed_text_cols(r, strlen(r)) == (t.width - 1 < 160 ? t.width - 1 : 160));
    CHECK(!strncmp(r, "\xE2\x94\x80\xE2\x94\x80", 6));
    CHECK(!strcmp(row(top + 2), line));
    const char* f = row(top + 3);
    CHECK(!strncmp(f, footer_left, strlen(footer_left)));
    size_t fl = strlen(f), hl = strlen(hint);
    if (*hint) {
        CHECK(fl >= hl && !strcmp(f + fl - hl, hint));
        CHECK(ed_text_cols(f, fl) == t.width - 1);
    } else {
        CHECK(!strcmp(f, footer_left));
    }
    for (int i = top + 4; i < TR; i++) CHECK(!strcmp(row(i), ""));
}

static void test_region(void) {
    setup_full(SC_REGION, 1, 100, 1, 0, "m1", "/home/t/proj", "/home/t");
    sc_banner(&sc, "0.2.0");
    CHECK(!strcmp(row(0), "barb 0.2.0 \xC2\xB7 m1 \xC2\xB7 ~/proj"));
    sc_draw(&sc, 0, 0, 1, 1);
    check_block(2, "", ">", "  m1 \xC2\xB7 ~/proj", "/help for commands");
    CHECK(t.r == 4 && t.c == 2);
    CHECK(has("\033[1;35m> \033[0m"));
    type("hi");
    CHECK(!strcmp(row(4), "> hi"));

    // Working: the activity line, a dim prompt, the hint.
    do_act(ACT_HTTP, 1, 0, 0, "/v1/messages", 0);
    sc_draw(&sc, 5000, 1, 1, 1);
    check_block(2, "\xE2\xA0\x99 waiting for the model \xC2\xB7 5s", "> hi", "  m1", "Enter queues it");
    CHECK(t.r == 4 && t.c == 4);
    sc_key(&sc, EK_CLEAR, 0);
    sc_draw(&sc, 5000, 1, 1, 1);
    check_block(2, "\xE2\xA0\x99 waiting for the model \xC2\xB7 5s", ">", "  m1", "Ctrl-C stops it");
    CHECK(has("\033[2m> \033[0m"));
    CHECK_INT(sc_next_ms(&sc, 5000), 120 - 5000 % 120);

    // Output goes above the block, which comes back under it.
    say(SAY_ASSISTANT, "Hello there");
    CHECK(!sc.ed.shown);
    CHECK(!strcmp(row(2), ""));
    CHECK(!strcmp(row(3), "Hello there"));
    for (int i = 4; i < TR; i++) CHECK(!strcmp(row(i), ""));
    do_act(ACT_SAY, 0, SAY_USAGE, 12400, "[tokens: 12000 in, 400 out]", 0);
    CHECK(find_row("[tokens") < 0);            // not printed: the footer has it
    do_ev(EV_HTTP, 1, 200);
    sc_draw(&sc, 6000, 0, 1, 1);
    check_block(4, "", ">", "  m1 \xC2\xB7 ~/proj \xC2\xB7 12.4k tokens", "/help for commands");
    CHECK_INT(sc_next_ms(&sc, 6000), -1);

    // Enter: the line stays as the record, under a blank row; the next line follows it directly.
    type("do it");
    int n;
    const char* line = sc_take(&sc, &n);
    CHECK(n == 5 && !strcmp(line, "do it"));
    CHECK(!strcmp(row(4), ""));
    CHECK(!strcmp(row(5), "> do it"));
    CHECK(t.r == 6 && t.c == 0);
    for (int i = 6; i < TR; i++) CHECK(!strcmp(row(i), ""));
    sc_draw(&sc, 6000, 1, 1, 1);
    type("more");
    line = sc_take(&sc, &n);
    CHECK(!strcmp(line, "more"));
    CHECK(!strcmp(row(6), "> more"));
    say(SAY_INFO, "(queued: it goes to the model when the current step is done)");
    CHECK(!strcmp(row(7), "(queued: it goes to the model when the current step is done)"));
    sc_draw(&sc, 6000, 1, 1, 1);
    check_block(8, "\xE2\xA0\x8B working", ">", "  m1", "Ctrl-C stops it");
    check_block(8, row(8), ">", "  m1", "Ctrl-C stops it");

    // Hidden for good (the end of input): the block goes and nothing is left of it.
    sc_draw(&sc, 6000, 1, 0, 1);
    CHECK(!sc.ed.shown);
    CHECK(sc.status_shown);                     // the status line takes over
    CHECK(strstr(row(8), "working") != 0);
    for (int i = 9; i < TR; i++) CHECK(!strcmp(row(i), ""));
    sc_hide(&sc);
    CHECK(!strcmp(row(8), ""));
    CHECK(t.r == 8 && t.c == 0);
    CHECK(escapes_ok(out, outn));
}

static void test_activity(void) {
    setup(SC_REGION, 1, 100);
    do_act(ACT_SPAWN, 3, 0, 10000, "ls -la", 0);
    sc_draw(&sc, 4000, 1, 1, 1);
    CHECK(strstr(row(0), " running ls -la \xC2\xB7 4s") != 0);
    sc_draw(&sc, 65000, 1, 1, 1);
    CHECK(strstr(row(0), " running ls -la \xC2\xB7 1m 05s") != 0);
    sc_draw(&sc, 3725000, 1, 1, 1);
    CHECK(strstr(row(0), " running ls -la \xC2\xB7 1h 02m") != 0);
    do_ev(EV_PROC, 3, 0);
    sc_draw(&sc, 3725000, 0, 1, 1);
    CHECK(!strcmp(row(0), ""));

    char longcmd[300];
    memset(longcmd, 'x', 200);
    longcmd[200] = 0;
    do_act(ACT_SPAWN, 4, 0, 10000, longcmd, 0);
    sc_draw(&sc, 2000, 1, 1, 1);
    const char* r = row(0);
    CHECK(ed_text_cols(r, strlen(r)) == 99);
    CHECK(strstr(r, "x\xE2\x80\xA6 \xC2\xB7 2s") != 0);
    do_ev(EV_PROC, 4, 0);
    do_act(ACT_SPAWN, 5, 0, 10000, "\n  echo a\necho b", 0);
    sc_draw(&sc, 0, 1, 1, 1);
    CHECK(strstr(row(0), " running echo a\xE2\x80\xA6") != 0);
    do_act(ACT_CANCEL, 5, 0, 0, 0, 0);
    sc_draw(&sc, 0, 0, 1, 1);
    CHECK(!strcmp(row(0), ""));

    // A failed request, and the wait before it is tried again.
    do_act(ACT_HTTP, 6, 0, 0, "/v1/messages", 0);
    do_ev(EV_HTTP, 6, 503);
    do_act(ACT_SAY, 0, SAY_INFO, 0, "API returned HTTP 503; retrying in 2s (attempt 1 of 5)", 10000);
    do_act(ACT_TIMER, 7, 0, 2000, 0, 10000);
    sc_draw(&sc, 10000, 1, 1, 1);
    int top = t.r - sc.ed.row;
    CHECK(strstr(row(top), " the request failed; retrying in 2s") != 0);
    sc_draw(&sc, 11500, 1, 1, 1);
    CHECK(strstr(row(top), " retrying in 1s") != 0);
    do_ev(EV_TIMER, 7, 0);
    do_act(ACT_HTTP, 8, 0, 0, "/v1/messages", 12000);
    sc_draw(&sc, 12000, 1, 1, 1);
    CHECK(strstr(row(top), " waiting for the model") != 0);
    do_ev(EV_HTTP, 8, 400);                     // not retried: no retry line from the next timer
    do_act(ACT_TIMER, 9, 0, 60000, 0, 0);
    sc_draw(&sc, 12000, 0, 1, 1);
    CHECK(!strcmp(row(top), "\xE2\x97\x8B a wakeup at 00:01 \xC2\xB7 idle until then"));

    // Background tasks and timers, while the agent waits for them.
    do_act(ACT_SPAWN, 10, 1, 0, "make", 0);
    sc_draw(&sc, 12000, 0, 1, 1);
    CHECK(!strcmp(row(top), "\xE2\x97\x8B task 10 running, a wakeup at 00:01 \xC2\xB7 idle until one of them"));
    CHECK_INT(sc_next_ms(&sc, 12000), -1);
    do_act(ACT_TIMER, 11, 0, 120000, 0, 0);
    do_act(ACT_SPAWN, 12, 1, 0, "make", 0);
    sc_draw(&sc, 12000, 0, 1, 1);
    CHECK(!strcmp(row(top), "\xE2\x97\x8B 2 tasks running, wakeups, the next at 00:01 \xC2\xB7 idle until one of them"));
    do_ev(EV_TIMER, 9, 0);
    do_ev(EV_TIMER, 11, 0);
    do_act(ACT_CANCEL, 12, 0, 0, 0, 0);
    sc_draw(&sc, 12000, 0, 1, 1);
    CHECK(!strcmp(row(top), "\xE2\x97\x8B task 10 running \xC2\xB7 idle until it ends"));
    sc_draw(&sc, 12000, 1, 1, 1);               // working beats waiting
    CHECK(strstr(row(top), " working") != 0);
    do_ev(EV_PROC, 10, 0);
    sc_draw(&sc, 12000, 0, 1, 1);
    CHECK(!strcmp(row(top), ""));

    // The tokens in the footer.
    static const struct { int64_t n; const char* s; } tok[] = {
        { 999, "999 tokens" }, { 1000, "1.0k tokens" }, { 12400, "12.4k tokens" }, { 99999, "99.9k tokens" },
        { 123456, "123k tokens" }, { 1500000, "1.5M tokens" }, { 250000000, "250M tokens" } };
    for (size_t i = 0; i < sizeof tok / sizeof tok[0]; i++) {
        do_act(ACT_SAY, 0, SAY_USAGE, tok[i].n, "[tokens]", 0);
        sc_draw(&sc, 0, 0, 1, 1);
        CHECK(strstr(row(t.r + 1), tok[i].s) != 0);
    }
    CHECK(escapes_ok(out, outn));
}

static void test_cwd(void) {
    setup_full(SC_REGION, 1, 100, 1, 0, "m1", "/home/tom", "/home/t");
    sc_draw(&sc, 0, 0, 1, 1);
    CHECK(!strncmp(row(3), "  m1 \xC2\xB7 /home/tom ", 16));
    setup_full(SC_REGION, 1, 100, 1, 0, "m1", "/home/t", "/home/t/");
    sc_draw(&sc, 0, 0, 1, 1);
    CHECK(!strncmp(row(3), "  m1 \xC2\xB7 ~ ", 10));
    setup_full(SC_REGION, 1, 100, 1, 0, "m1", "C:\\Users\\t\\src", "C:\\Users\\t");
    sc_draw(&sc, 0, 0, 1, 1);
    CHECK(!strncmp(row(3), "  m1 \xC2\xB7 ~\\src ", 14));
    setup_full(SC_REGION, 1, 100, 1, 0, "m1", "/srv/a/very/long/path/that/goes/on/and/on/to/the/end", 0);
    sc_draw(&sc, 0, 0, 1, 1);
    CHECK(strstr(row(3), "  m1 \xC2\xB7 \xE2\x80\xA6/that/goes/on/and/on/to/the/end ") != 0);
    // Too narrow for the hint: it goes, and the rest is cut.
    setup_full(SC_REGION, 1, 30, 1, 0, "a-long-model-name", "/srv/proj", 0);
    sc_draw(&sc, 0, 0, 1, 1);
    CHECK(!strstr(row(3), "/help"));
    CHECK(ed_text_cols(row(3), strlen(row(3))) <= 29);
    CHECK(!strcmp(row(4), ""));
}

static void test_question(void) {
    setup(SC_REGION, 1, 100);
    sc_draw(&sc, 0, 0, 1, 1);
    type("typed");
    const char* desc = "create a.txt\n+ new line\n- old line";
    sc_question(&sc, "write_file", 10, desc, strlen(desc));
    CHECK(!sc.ed.shown);
    CHECK(!strcmp(row(0), "\xE2\x97\x8F write_file wants to:"));
    CHECK(!strcmp(row(1), "  create a.txt"));
    CHECK(!strcmp(row(2), "  + new line"));
    CHECK(!strcmp(row(3), "  - old line"));
    CHECK(!strcmp(row(4), "Allow? [y]es / [n]o / [a]lways this session:"));
    CHECK(has("\033[32m+ new line\033[0m") && has("\033[31m- old line\033[0m"));
    size_t before = outn;
    sc_draw(&sc, 0, 1, 0, 0);                   // while it waits, nothing is drawn
    CHECK(outn == before);
    sc_answer(&sc, ANS_ALWAYS);
    CHECK(!strcmp(row(4), "Allow? [y]es / [n]o / [a]lways this session: always"));
    CHECK(has("\033[32malways\033[0m"));
    sc_draw(&sc, 0, 1, 1, 1);
    check_block(5, row(5), "> typed", "  m1", "Enter queues it");
    // A question that goes unanswered (the agent cancelled it): its line is ended.
    sc_question(&sc, "bash", 4, "run: ls", 7);
    CHECK(!strcmp(row(5), ""));
    CHECK(!strcmp(row(6), "\xE2\x97\x8F bash wants to:"));
    sc_hide(&sc);
    CHECK(t.r == 9 && t.c == 0);
    sc_question(&sc, "bash", 4, "run: ls", 7);
    sc_answer(&sc, ANS_NO);
    CHECK(has("\033[31mno\033[0m"));
    CHECK(escapes_ok(out, outn));
}

static void test_safety(void) {
    setup_full(SC_REGION, 1, 100, 1, 0, "m\033[31mx", "/w\033]0;t\007", 0);
    say(SAY_ERROR, "bad \033]0;pwned\007 \033[2J x");
    say(SAY_TOOL, "ba\033[2Jsh run");
    say(SAY_TOOL, "  \033[1Aup");
    say(SAY_INFO, "c1 \xC2\x9B" "2J");
    sc_question(&sc, "t\033]x", 4, "d\033[B", 4);
    sc_answer(&sc, ANS_YES);
    say(SAY_ASSISTANT, "reply \033[2J");
    do_act(ACT_SPAWN, 1, 0, 0, "cmd \033[2J \xC2\x9B" "x \x07", 0);
    sc_draw(&sc, 0, 1, 1, 1);
    CHECK(escapes_ok(out, outn));
    CHECK(has("^["));
    CHECK(t.bad == 0);
    const char* act = row(t.r - sc.ed.row);
    CHECK(strstr(act, "running cmd  [2J ?x") != 0);
    CHECK(strstr(row(t.r + 1), "  m [31mx \xC2\xB7 /w ]0;t ") != 0);
    // In PIPE mode text goes as it came, unless standard error is a terminal after all.
    setup(SC_PIPE, 0, 80);
    say(SAY_INFO, "raw \033[1m");
    CHECK(has("raw \033[1m"));
    setup(SC_PIPE, 1, 80);
    say(SAY_INFO, "raw \033[1m");
    CHECK(!strchr(out, 0x1B));
}

// Corners the walk-throughs above do not reach.
static void test_corners(void) {
    // A 429 is retried as a 5xx is: the timer after it is the retry.
    setup(SC_REGION, 1, 100);
    do_act(ACT_HTTP, 1, 0, 0, "/v1/messages", 0);
    do_ev(EV_HTTP, 1, 429);
    do_act(ACT_TIMER, 2, 0, 3000, 0, 0);
    sc_draw(&sc, 0, 1, 1, 1);
    CHECK(strstr(row(0), "the request failed; retrying in 3s") != 0);
    // A failure counts only for the timer set right after it: anything else in between and the
    // next timer is the model's own.
    setup(SC_REGION, 1, 100);
    do_act(ACT_HTTP, 1, 0, 0, "/v1/messages", 0);
    do_ev(EV_HTTP, 1, 500);
    do_ev(EV_INPUT, 0, 0);
    do_act(ACT_TIMER, 2, 0, 60000, 0, 0);
    sc_draw(&sc, 0, 0, 1, 1);
    CHECK(!strcmp(row(0), "\xE2\x97\x8B a wakeup at 00:01 \xC2\xB7 idle until then"));
    // Tasks that end out of order leave the right one.
    setup(SC_REGION, 1, 100);
    for (int i = 1; i <= 3; i++) do_act(ACT_SPAWN, i, 1, 0, "make", 0);
    do_ev(EV_PROC, 1, 0);
    do_ev(EV_PROC, 2, 0);
    sc_draw(&sc, 0, 0, 1, 1);
    CHECK(strstr(row(0), "task 3 running") != 0);
    // A command cut to fit its store is marked as cut even when what is left fits the row.
    setup(SC_REGION, 1, 100);
    char euro[60 * 3 + 1];
    for (int i = 0; i < 60; i++) memcpy(euro + i * 3, "\xE2\x82\xAC", 3);
    euro[60 * 3] = 0;
    do_act(ACT_SPAWN, 1, 0, 0, euro, 0);
    sc_draw(&sc, 0, 1, 1, 1);
    CHECK(strstr(row(0), "\xE2\x82\xAC\xE2\x80\xA6") != 0);
    // The hint needs three columns of space before it, or it goes.
    setup(SC_REGION, 1, 29);
    sc_draw(&sc, 0, 0, 1, 1);
    CHECK(!strncmp(row(3), "  m1 \xC2\xB7 /w", 10));
    CHECK(!strstr(row(3), "/help"));
    // An overlong UTF-8 form never reaches the terminal.
    setup_full(SC_REGION, 1, 100, 1, 0, "m\xC0\xAF", "/w", 0);
    sc_banner(&sc, "0.2.0");
    sc_draw(&sc, 0, 0, 1, 1);
    CHECK(!memchr(out, 0xC0, outn));
    CHECK(strstr(row(t.r + 1), "  m?? \xC2\xB7 /w") != 0);

    // While a question waits, nothing is drawn over it, whatever the caller says.
    setup(SC_LITE, 1, 80);
    do_act(ACT_HTTP, 1, 0, 0, "/v1/messages", 0);
    sc_draw(&sc, 0, 1, 0, 1);
    CHECK(strstr(row(0), "waiting for the model") != 0);
    sc_question(&sc, "bash", 4, "run: ls", 7);
    int q = find_row("Allow?");
    CHECK(q > 0);
    sc_draw(&sc, 0, 1, 0, 0);
    CHECK(q > 0 && !strcmp(row(q), "Allow? [y]es / [n]o / [a]lways this session:"));
    sc_draw(&sc, 0, 1, 0, 1);
    CHECK(q > 0 && !strcmp(row(q), "Allow? [y]es / [n]o / [a]lways this session:"));
    CHECK_INT(sc_next_ms(&sc, 0), -1);
    // And a prompt drawn under an open question starts on a row of its own.
    setup(SC_REGION, 1, 100);
    sc_question(&sc, "bash", 4, "run: ls", 7);
    sc_draw(&sc, 0, 0, 1, 1);
    q = find_row("Allow?");
    CHECK(q > 0 && !strcmp(row(q), "Allow? [y]es / [n]o / [a]lways this session:"));
    if (q > 0) check_block(q + 1, "", ">", "  m1", "/help for commands");
    CHECK(escapes_ok(out, outn));
}

static void test_ascii(void) {
    setup_full(SC_REGION, 1, 80, 1, 1, "m1", "/a/very/long/path/that/goes/on/and/on/to/the/end", 0);
    sc_banner(&sc, "0.2.0");
    say(SAY_TOOL, "bash ls");
    say(SAY_TOOL, "  exit 0");
    say(SAY_ASSISTANT, "- item\n\n---");
    sc_question(&sc, "bash", 4, "run: ls", 7);
    sc_answer(&sc, ANS_YES);
    do_act(ACT_HTTP, 1, 0, 0, "/v1/messages", 0);
    do_act(ACT_SPAWN, 2, 1, 0, "make", 0);
    sc_draw(&sc, 3000, 1, 1, 1);
    int ok = 1;
    for (size_t i = 0; i < outn; i++) if ((unsigned char)out[i] >= 0x80) ok = 0;
    CHECK(ok);
    CHECK(strstr(row(t.r - sc.ed.row), "- waiting for the model - 3s") != 0);
    CHECK(!strncmp(row(t.r - sc.ed.row + 1), "-----", 5));
    CHECK(strstr(row(t.r + 1), "...") != 0);
    CHECK_INT(sc_next_ms(&sc, 3000), 250 - 3000 % 250);
    do_ev(EV_HTTP, 1, 200);
    sc_draw(&sc, 3000, 0, 1, 1);
    CHECK(!strcmp(row(t.r - sc.ed.row), "o task 2 running - idle until it ends"));
}

static void test_resize(void) {
    setup(SC_REGION, 1, 60);
    sc_draw(&sc, 0, 0, 1, 1);
    CHECK(ed_text_cols(row(1), strlen(row(1))) == 59);
    sc_resize(&sc, 90);
    t.width = 90;                               // the terminal, rewrapped (the block is gone from it)
    CHECK(!sc.ed.shown);
    sc_draw(&sc, 0, 0, 1, 1);
    int top = t.r - sc.ed.row;
    CHECK(ed_text_cols(row(top + 1), strlen(row(top + 1))) == 89);
    say(SAY_ASSISTANT, "word word word word word word word word word word word word word word word word word word word");
    for (int r = 0; r < TR; r++) CHECK(ed_text_cols(row(r), strlen(row(r))) <= 89);
    CHECK(find_row("word word word word word word word word word word word word word word word word word") >= 0);
}

// ---- random sessions ----

static uint32_t seed = 4242;
static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 8) & 0xFFFFFF; }

static const char* bits[] = {
    "word", " ", "  ", "\n", "x", "**b**", "`c`", "- ", "# ", "| a | b |", "\033[2J", "\033]0;t\007", "\t",
    "\xC3\xA9", "\xE4\xB8\xAD", "\xC2\x9B", "\xFF", "longerwordthatgoeson", "1. ", "> ", "+ ", "\r\n" };

static void random_text(char* b, size_t cap) {
    size_t k = 0;
    int n = (int)(rnd() % 12);
    for (int i = 0; i < n; i++) {
        const char* p = bits[rnd() % (sizeof bits / sizeof bits[0])];
        size_t l = strlen(p);
        if (k + l + 1 > cap) break;
        memcpy(b + k, p, l);
        k += l;
    }
    b[k] = 0;
}

static int dumped;
static void check_screen(int mode, int awaiting) {
    if (mode == SC_PIPE && !sc.cfg.vt_err) return;   // raw bytes, as they came
    if (t.bad && !dumped++) { dump("random"); fwrite(out, 1, outn > 3000 ? 3000 : outn, stdout); printf("\n...\n"); fwrite(outn > 1500 ? out + outn - 1500 : out, 1, outn > 1500 ? 1500 : outn, stdout); }
    CHECK(t.bad == 0);
    if (sc.ed.shown) {
        int top = t.r - sc.ed.row;
        CHECK(top >= 0);
        if (top < 0) return;
        const char* a = row(top);
        CHECK(ed_text_cols(a, strlen(a)) <= t.width - 1);
        const char* r = row(top + 1);
        CHECK(!strncmp(r, sc.cfg.ascii ? "--" : "\xE2\x94\x80", sc.cfg.ascii ? 2 : 3));
        CHECK(!strncmp(row(top + 2), ">", 1));
        int last = -1;
        for (int i = top; i < TR; i++) if (*row(i)) last = i;
        CHECK(last > top + 2 && !strncmp(row(last), "  m1", 4));
    } else if (!awaiting && !sc.status_shown && mode != SC_PIPE) {
        CHECK(t.c == 0);
        for (int i = t.r; i < TR; i++) CHECK(!strcmp(row(i), ""));
    }
}

static void test_random(void) {
    static const int keys[] = { EK_BACK, EK_DEL, EK_LEFT, EK_RIGHT, EK_HOME, EK_END, EK_UP, EK_DOWN, EK_KILL_WORD, EK_CLEAR };
    char text[512];
    for (int round = 0; round < 300; round++) {
        int mode = (int)(rnd() % 3);
        int width = 20 + (int)(rnd() % (TC - 20 + 1));
        // One rnd() per statement: the order of a call's arguments is up to the compiler.
        int vt = mode != SC_PIPE || rnd() % 2;
        int deferred = (int)(rnd() % 2);
        int ascii = rnd() % 4 == 0;
        setup_full(mode, vt, width, deferred, ascii, "m1", "/w", 0);
        int64_t now = 0, id = 1;
        int awaiting = 0, busy = 0;
        int64_t open[8];
        int nopen = 0;
        for (int step = 0; step < 200; step++) {
            now += rnd() % 3000;
            switch (rnd() % 14) {
            case 0: case 1:
                random_text(text, sizeof text);
                say((int)(rnd() % 4), text);
                break;
            case 2: case 3: {
                int prompt_ok = mode == SC_REGION && !awaiting;
                sc_draw(&sc, now, busy, prompt_ok, !awaiting);
                break;
            }
            case 4:
                if (!awaiting) {
                    uint32_t r = rnd() % 4;
                    if (r == 0) sc_key(&sc, keys[rnd() % (sizeof keys / sizeof keys[0])], 0);
                    else sc_key(&sc, EK_CHAR, r == 1 ? 0x4E2D : 'a' + rnd() % 26);
                }
                break;
            case 5:
                if (!awaiting && mode == SC_REGION) { int n; sc_take(&sc, &n); }
                break;
            case 6:
                if (!awaiting) {
                    random_text(text, sizeof text);
                    sc_question(&sc, "bash", 4, text, strlen(text));
                    awaiting = 1;
                } else {
                    sc_answer(&sc, (int)(rnd() % 3));
                    awaiting = 0;
                }
                break;
            case 7: {
                actk_t k = rnd() % 2 ? ACT_HTTP : ACT_SPAWN;
                random_text(text, sizeof text);
                do_act(k, id, (int)(rnd() % 2), 1000, text, now);
                if (nopen < 8) open[nopen++] = id;
                id++;
                busy = 1;
                break;
            }
            case 8:
                do_act(ACT_TIMER, id, 0, rnd() % 100000, 0, now);
                if (nopen < 8) open[nopen++] = id;
                id++;
                break;
            case 9:
                if (nopen) {
                    int i = (int)(rnd() % (uint32_t)nopen);
                    evk_t k = rnd() % 2 ? EV_HTTP : rnd() % 2 ? EV_PROC : EV_TIMER;
                    int st = rnd() % 2 ? 503 : 200;
                    do_ev(k, open[i], st);
                    open[i] = open[--nopen];
                }
                break;
            case 10:
                if (nopen) { int i = (int)(rnd() % (uint32_t)nopen); do_act(ACT_CANCEL, open[i], 0, 0, 0, now); open[i] = open[--nopen]; }
                break;
            case 11:
                do_act(ACT_SAY, 0, SAY_USAGE, rnd() % 3000000, "[tokens: 1 in, 2 out]", now);
                break;
            case 12:
                busy = (int)(rnd() % 2);
                break;
            case 13:
                if (rnd() % 4 == 0) { sc_hide(&sc); awaiting = 0; }
                break;
            }
            check_screen(mode, awaiting);
            if (mode == SC_PIPE && sc.cfg.vt_err) CHECK(!strchr(fd2, 0x1B));   // made safe, and no styling
            CHECK(sc_next_ms(&sc, now) <= 250);
        }
        CHECK(escapes_ok(out, outn) || (mode == SC_PIPE && !sc.cfg.vt_err));
    }
}

int main(void) {
    test_pipe();
    test_lite();
    test_region();
    test_activity();
    test_cwd();
    test_question();
    test_safety();
    test_corners();
    test_ascii();
    test_resize();
    test_random();
    return checks_report("test_screen");
}
