// Tests for the prompt's line editor (src/ui/edit.c).
//
// The editor only produces bytes for a terminal, so the test plays them on a small terminal
// emulator and looks at the screen: what each row shows and where the cursor is. It does this
// for both of the ways terminals treat a row filled to the last column (move to the next row at
// once, or wait for the next character), because the editor must keep track of the cursor on
// either. After the scripted cases, a long random run checks the screen after every key.
#include "check.h"
#include "../src/ui/edit.h"

#include "term.h"

// ---- what the screen should show ----

typedef struct {
    char rows[TR][TC * 4 + 1];
    int n;                     // rows the prompt and line take
    int cr, cc;                // where the cursor should be, from the prompt's row
} layout_t;

// The prompt as it shows (the editor's, without its colours) and its columns.
static char prompt_text[64] = "> ";
static int prompt_cols = 2;

// Worked out independently of the editor: the prompt, then each character on the row it fits
// in, a row filled to the last column putting the cursor at the start of the next.
static void layout(layout_t* L, const char* line, int n, int cur, int width) {
    memset(L, 0, sizeof *L);
    int r = 0, c = prompt_cols, k = (int)strlen(prompt_text);
    strcpy(L->rows[0], prompt_text);
    L->cr = 0; L->cc = prompt_cols;
    for (int i = 0; i < n;) {
        int len;
        uint32_t cp = utf8(line + i, n - i, &len);
        int w = ed_cols(cp);
        if (w && c + w > width) { L->rows[r][k] = 0; r++; c = 0; k = 0; }
        memcpy(L->rows[r] + k, line + i, (size_t)len);
        k += len;
        c += w;
        i += len;
        if (i == cur) { L->cr = r; L->cc = c; }
    }
    L->rows[r][k] = 0;
    L->n = r + 1;
    if (L->cc >= width) { L->cr++; L->cc = 0; }
    for (int i = 0; i < L->n; i++) {     // compared without trailing spaces, as rows are read
        size_t l = strlen(L->rows[i]);
        while (l && L->rows[i][l - 1] == ' ') L->rows[i][--l] = 0;
    }
}

// ---- driving the editor ----

static term_t T;
static ed_t E, E2;
static layout_t L;
static int top;                // the prompt's row on the terminal
static int step_failed;        // set by the checks below; the random run stops at the first

static void fail(const char* what, const char* detail) {
    checks_failed++;
    step_failed = 1;
    printf("FAIL: %s (%s; width %d, %s wrap; line \"%s\", cursor at byte %d)\n", what, detail, T.width,
           T.deferred ? "deferred" : "immediate", E.line, E.cur);
}

static void apply(void) { t_feed(&T, E.out, E.outn); }
static void key(int k, uint32_t ch) { ed_key(&E, k, ch); apply(); }
static void type(const char* s) {
    for (int n = (int)strlen(s), i = 0; i < n;) {
        int len;
        uint32_t cp = utf8(s + i, n - i, &len);
        key(EK_CHAR, cp);
        i += len;
    }
}

static void start(int width, int deferred) {
    t_init(&T, width, deferred);
    t_feed(&T, "above\r\n", 7);
    top = 1;
    ed_init(&E, width);
    strcpy(prompt_text, "> ");
    prompt_cols = 2;
    ed_show(&E);
    apply();
}

// What a decoration row should show: escapes gone, control characters as spaces, cut to leave
// the last column free, without the spaces at its end.
static void decor_text(const char* s, int width, char* b) {
    int k = 0, cols = 0;
    for (int n = (int)strlen(s), i = 0; i < n;) {
        if (s[i] == 0x1B) {
            if (i + 1 < n && s[i + 1] == '[') {
                i += 2;
                while (i < n && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7E)) i++;
                i++;
            } else {
                i += 2;
            }
            continue;
        }
        int len;
        uint32_t cp = utf8(s + i, n - i, &len);
        int ctl = cp < 0x20 || cp == 0x7F;
        int w = ctl ? 1 : ed_cols(cp);
        if (cols + w > width - 1) break;
        if (ctl) b[k++] = ' ';
        else { memcpy(b + k, s + i, (size_t)len); k += len; }
        cols += w;
        i += len;
    }
    while (k && b[k - 1] == ' ') k--;
    b[k] = 0;
}

// The screen shows exactly the block: the decoration rows, the prompt and the line, the footer,
// with the cursor in place; or, hidden, nothing.
static void check_screen(const char* when) {
    char got[TC * CELL + 1], want[TC * CELL + 1];
    checks_run++;
    step_failed = 0;
    if (T.bad) { fail("the terminal was sent something wrong", when); T.bad = 0; return; }
    t_row(&T, 0, got);
    if (strcmp(got, "above")) { fail("the output above the prompt was touched", when); return; }
    if (!E.shown) {
        for (int r = top; r < TR; r++) {
            t_row(&T, r, got);
            if (*got) { fail("a hidden prompt left something on the screen", when); return; }
        }
        if (T.r != top || T.c != 0 || T.pend) fail("hidden, the cursor is not where the prompt was", when);
        return;
    }
    int na = E.nabove;
    for (int i = 0; i < na; i++) {
        t_row(&T, top + i, got);
        decor_text(E.above[i], T.width, want);
        if (strcmp(got, want)) {
            char d[TC * CELL * 2 + 64];
            snprintf(d, sizeof d, "%s: decoration row %d is \"%s\", should be \"%s\"", when, i, got, want);
            fail("the screen does not show the decoration", d);
            return;
        }
    }
    layout(&L, E.line, E.n, E.n, T.width);
    int end_row = L.cr;                // the line's end, on the row after a full one
    layout(&L, E.line, E.n, E.cur, T.width);
    int line0 = top + na;
    for (int i = 0; i < L.n; i++) {
        t_row(&T, line0 + i, got);
        if (strcmp(got, L.rows[i])) {
            char d[TC * CELL * 2 + 64];
            snprintf(d, sizeof d, "%s: row %d is \"%s\", should be \"%s\"", when, i, got, L.rows[i]);
            fail("the screen does not show the line", d);
            return;
        }
    }
    int foot = E.has_below ? line0 + end_row + 1 : -1;
    for (int r = line0 + L.n; r < TR; r++) {
        t_row(&T, r, got);
        if (r == foot) {
            decor_text(E.below, T.width, want);
            if (strcmp(got, want)) {
                char d[TC * CELL * 2 + 64];
                snprintf(d, sizeof d, "%s: the footer is \"%s\", should be \"%s\"", when, got, want);
                fail("the screen does not show the footer", d);
                return;
            }
            continue;
        }
        if (*got) { fail("something is left below the line", when); return; }
    }
    if (T.pend || T.r != line0 + L.cr || T.c != L.cc) {
        char d[200];
        snprintf(d, sizeof d, "%s: at row %d col %d%s, should be row %d col %d", when, T.r - line0, T.c,
                 T.pend ? " waiting to wrap" : "", L.cr, L.cc);
        fail("the cursor is in the wrong place", d);
        return;
    }
    if (E.row != na + L.cr) fail("the editor lost track of the cursor's row", when);
}

static void check_line(const char* want) { CHECK_INT(strcmp(E.line, want), 0); if (strcmp(E.line, want)) printf("  line \"%s\", want \"%s\"\n", E.line, want); }

// Enter: the line stays on the screen and the cursor goes to the start of the row after it.
static const char* enter(void) {
    int was_shown = E.shown;
    // With the cursor at the end it is on the line's last row, or (column 0) already on the
    // row after a full one; either way the next prompt goes on the row after the line.
    layout(&L, E.line, E.n, E.n, T.width);
    int rows = L.cc ? L.cr + 1 : L.cr;
    int n;
    const char* s = ed_take(&E, &n);
    apply();
    CHECK_INT((int)strlen(s), n);
    if (was_shown) {
        top += rows;
        CHECK(T.r == top && T.c == 0 && !T.pend);
    }
    check_screen("after Enter");
    return s;
}

// ---- the cases ----

static void test_typing_and_wrapping(int deferred) {
    start(10, deferred);
    check_screen("a new prompt");
    type("hello");
    check_line("hello");
    check_screen("typing");
    CHECK_INT(T.c, 7);
    start(10, deferred);
    type("abcdefgh");                  // with the prompt, exactly the width
    check_screen("a full row");
    CHECK_INT(E.row, 1);
    CHECK(T.r == top + 1 && T.c == 0);
    type("i");
    check_screen("the next row");
    key(EK_BACK, 0);
    check_screen("back to a full row");
    key(EK_HOME, 0);
    check_screen("home from below");
    CHECK(T.r == top && T.c == 2);
    key(EK_END, 0);
    check_screen("end of a full row");
    type("ijklmnopqrstuvwxyz");
    check_screen("three rows");
    for (int i = 0; i < 12; i++) key(EK_LEFT, 0);
    check_screen("left across rows");
    type("_");
    check_screen("inserting in the middle of wrapped text");
    check_line("abcdefghijklmn_opqrstuvwxyz");
}

static void test_moving_and_killing(void) {
    start(30, 1);
    type("helo");
    key(EK_LEFT, 0);
    type("l");
    check_line("hello");
    CHECK_INT(E.cur, 4);
    check_screen("insert");
    key(EK_DEL, 0);
    check_line("hell");
    key(EK_DEL, 0);                    // at the end: nothing
    CHECK_INT(E.outn, 0);
    key(EK_RIGHT, 0);                  // nowhere to go: nothing drawn
    CHECK_INT(E.outn, 0);

    start(30, 1);
    type("one two  three");
    key(EK_WORD_LEFT, 0); CHECK_INT(E.cur, 9);
    key(EK_WORD_LEFT, 0); CHECK_INT(E.cur, 4);
    key(EK_WORD_LEFT, 0); CHECK_INT(E.cur, 0);
    key(EK_WORD_LEFT, 0); CHECK_INT(E.outn, 0);
    key(EK_WORD_RIGHT, 0); CHECK_INT(E.cur, 3);
    key(EK_WORD_RIGHT, 0); CHECK_INT(E.cur, 7);
    key(EK_WORD_RIGHT, 0); CHECK_INT(E.cur, 14);
    check_screen("word moves");

    key(EK_KILL_WORD, 0); check_line("one two  ");
    key(EK_KILL_WORD, 0); check_line("one ");
    check_screen("kill word");
    type("two three");
    key(EK_HOME, 0);
    key(EK_RIGHT, 0);
    key(EK_KILL_END, 0); check_line("o");
    CHECK_INT(E.cur, 1);
    key(EK_KILL_END, 0); CHECK_INT(E.outn, 0);
    type("ne two");
    key(EK_WORD_LEFT, 0);
    key(EK_KILL_START, 0); check_line("two");
    CHECK_INT(E.cur, 0);
    key(EK_KILL_START, 0); CHECK_INT(E.outn, 0);
    check_screen("kill to the start");
    key(EK_CLEAR, 0); check_line("");
    check_screen("clear");
    key(EK_CLEAR, 0); CHECK_INT(E.outn, 0);
    key(EK_BACK, 0); CHECK_INT(E.outn, 0);
}

static void test_unicode(int deferred) {
    start(12, deferred);
    type("aé");
    CHECK_INT(E.n, 3);
    key(EK_BACK, 0);
    check_line("a");                   // the whole character, not half of it
    type("漢");
    CHECK_INT(T.c, 5);                 // "> a" and two columns
    check_screen("a wide character");
    type("😀");
    CHECK_INT(E.n, 8);
    key(EK_LEFT, 0);
    CHECK_INT(E.cur, 4);
    key(EK_BACK, 0);
    check_line("a😀");
    key(EK_END, 0);
    key(EK_BACK, 0);
    check_line("a");
    type("e\xcc\x81");                 // e and a combining acute: one column
    CHECK_INT(T.c, 4);
    check_screen("a combining mark");
    key(EK_BACK, 0);
    check_line("ae");                  // backspace takes the mark, then the letter

    start(10, deferred);
    type("漢漢漢漢");                   // 2 + 8: exactly full
    check_screen("wide characters filling a row");
    CHECK(T.r == top + 1 && T.c == 0);
    type("\xcc\x81");                  // a mark on the last of them, which is on the row above
    check_screen("a mark after a full row");
    type("漢");
    check_screen("a wide character on the next row");
    start(11, deferred);
    type("漢漢漢漢漢");                 // 2 + 8 leaves one column, too narrow for the fifth
    check_screen("a wide character that does not fit");
    CHECK_INT(E.row, 1);

    start(20, deferred);
    key(EK_CHAR, 1);
    key(EK_CHAR, 0x7F);
    key(EK_CHAR, 0xD800);
    key(EK_CHAR, 0x110000);
    check_line("");
    CHECK_INT(E.outn, 0);
    key(EK_CHAR, '\t');
    check_line(" ");

    CHECK_INT(ed_cols('a'), 1);
    CHECK_INT(ed_cols(0), 0);
    CHECK_INT(ed_cols(0x301), 0);
    CHECK_INT(ed_cols(0x200B), 0);
    CHECK_INT(ed_cols(0x4E00), 2);
    CHECK_INT(ed_cols(0xAC00), 2);
    CHECK_INT(ed_cols(0x1F600), 2);
    CHECK_INT(ed_cols(0xE9), 1);
}

static void test_hidden(int deferred) {
    start(10, deferred);
    type("old");                       // something in the history to move through
    enter();
    ed_show(&E); apply();
    type("abcdefghijk");
    ed_hide(&E); apply();
    check_screen("hidden");
    ed_hide(&E);
    CHECK_INT(E.outn, 0);
    type("lm");                        // while output is going by: no drawing
    CHECK_INT(E.outn, 0);
    key(EK_UP, 0); check_line("old");
    CHECK_INT(E.outn, 0);
    key(EK_DOWN, 0); check_line("abcdefghijklm");
    key(EK_HOME, 0);
    CHECK_INT(E.outn, 0);
    check_screen("edited while hidden");
    ed_show(&E); apply();
    check_line("abcdefghijklm");
    check_screen("shown again");
    CHECK_INT(E.cur, 0);
    ed_show(&E);
    CHECK_INT(E.outn, 0);
}

static void test_enter_and_history(int deferred) {
    start(10, deferred);
    const char* s = enter();
    CHECK_INT(strcmp(s, ""), 0);
    ed_show(&E); apply();
    type("one");
    s = enter();
    CHECK_INT(strcmp(s, "one"), 0);
    check_line("");
    CHECK_INT(E.cur, 0);
    ed_show(&E); apply();
    type("abcdefgh");                  // exactly full: no blank row after it
    key(EK_HOME, 0);
    enter();
    ed_show(&E); apply();
    type("two");
    enter();
    ed_show(&E); apply();
    type("two");                       // the same again is not kept twice
    enter();
    ed_show(&E); apply();
    check_screen("the prompt after a few lines");
    type("dr");
    key(EK_UP, 0); check_line("two");
    check_screen("history");
    key(EK_UP, 0); check_line("abcdefgh");
    key(EK_UP, 0); check_line("one");
    key(EK_UP, 0); check_line("one"); CHECK_INT(E.outn, 0);   // the oldest; the empty line was not kept
    key(EK_DOWN, 0); check_line("abcdefgh");
    check_screen("history down");
    key(EK_DOWN, 0); check_line("two");
    key(EK_DOWN, 0); check_line("dr");    // the draft comes back
    key(EK_DOWN, 0); CHECK_INT(E.outn, 0);
    key(EK_UP, 0);
    type("!");                         // editing a history line, then Enter, keeps both
    s = enter();
    CHECK_INT(strcmp(s, "two!"), 0);
    ed_show(&E); apply();
    key(EK_UP, 0); check_line("two!");
    key(EK_UP, 0); check_line("two");
    key(EK_CLEAR, 0);                  // Escape leaves the history: a fresh, empty line
    key(EK_DOWN, 0); check_line("");
    key(EK_UP, 0); check_line("two!");

    // When the history is full, the oldest whole lines go. A short line first and then nine
    // long ones: 101 + 8 x 8,001 bytes fit in 65,536; the tenth pushes out the short one and
    // then the first long one, so no line is ever cut in two.
    static char big[8001];
    ed_init(&E2, 80);
    for (int i = 0; i < 10; i++) {
        int len = i ? 8000 : 100;
        memset(big, 'a' + i, (size_t)len);
        big[len] = 0;
        for (int j = 0; j < len; j++) ed_key(&E2, EK_CHAR, (uint32_t)(unsigned char)big[j]);
        int n;
        ed_take(&E2, &n);
        CHECK_INT(n, len);
    }
    int ups = 0;
    for (;;) {
        char before = E2.line[0];
        ed_key(&E2, EK_UP, 0);
        if (E2.line[0] == before && ups) break;
        ups++;
        if (ups > 20) break;
    }
    CHECK_INT(ups, 8);                 // eight lines are left
    CHECK_INT(E2.line[0], 'c');        // the oldest of them is the third typed
    CHECK_INT((int)strlen(E2.line), 8000);
}

static void test_limits(void) {
    ed_init(&E2, 5);
    CHECK_INT(E2.width, 10);
    ed_set_width(&E2, 300);
    CHECK_INT(E2.width, 300);
    for (int i = 0; i < ED_MAX + 50; i++) ed_key(&E2, EK_CHAR, 'x');
    CHECK_INT(E2.n, ED_MAX - 1);       // and the NUL
    ed_key(&E2, EK_CHAR, 0xE9);
    CHECK_INT(E2.n, ED_MAX - 1);
    ed_key(&E2, EK_BACK, 0);
    ed_key(&E2, EK_BACK, 0);
    ed_key(&E2, EK_CHAR, 0x4E00);      // three bytes, two free: refused whole, not cut in half
    CHECK_INT(E2.n, ED_MAX - 3);
    ed_key(&E2, EK_CHAR, 0xE9);        // two bytes fit
    CHECK_INT(E2.n, ED_MAX - 1);
    CHECK_INT(E2.n, (int)strlen(E2.line));
    ed_show(&E2);                      // a whole line's drawing fits in the output buffer
    CHECK(E2.outn > ED_MAX && E2.outn < (int)sizeof E2.out);
    ed_key(&E2, EK_HOME, 0);
    CHECK(E2.outn < (int)sizeof E2.out);
}

static void refresh(void) { ed_refresh(&E); apply(); }
static void set_prompt(const char* s, const char* shows, int cols) {
    ed_set_prompt(&E, s, cols);
    strcpy(prompt_text, shows);
    prompt_cols = cols;
}

static void test_decoration(int deferred) {
    start(20, deferred);
    ed_frame(&E, 2, 1); apply();
    check_screen("an empty frame");
    ed_set_above(&E, 0, "\033[2m\xe2\xa0\x8b waiting \xc2\xb7 3s\033[0m");
    ed_set_above(&E, 1, "\033[2m\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\033[0m");
    ed_set_below(&E, "\033[2mmodel \xc2\xb7 ~/x\033[0m");
    refresh();
    check_screen("decorated");
    CHECK_INT(E.above_cols[1], 19);    // a rule wider than the row is cut to leave the last column
    refresh();
    CHECK_INT(E.outn, 0);              // nothing changed, nothing drawn
    ed_set_above(&E, 0, E.above[0]);
    refresh();
    CHECK_INT(E.outn, 0);              // the same text again changes nothing
    type("hello");
    check_screen("typing above a footer");
    ed_set_above(&E, 0, "\033[2m\xe2\xa0\x99 waiting \xc2\xb7 4s\033[0m");
    refresh();
    check_screen("the activity row changed");
    CHECK(!memchr(E.out, 'J', (size_t)E.outn));   // rewritten in place: the line was not erased
    key(EK_HOME, 0);
    ed_set_above(&E, 1, "short");      // shorter than before: the rest of the row is cleared
    refresh();
    check_screen("a shorter decoration row, cursor at home");
    key(EK_END, 0);
    type("abcdefghijklm");             // 2 + 18: exactly full, the footer two rows down
    check_screen("a full row above a footer");
    CHECK_INT(E.row, 3);
    type("n");
    check_screen("onto the row after a full one");
    key(EK_BACK, 0);
    key(EK_BACK, 0);
    check_screen("back off the full row");
    ed_set_below(&E, "a\tb\x01" "c \xe6\xbc\xa2\xe6\xbc\xa2\xe6\xbc\xa2\xe6\xbc\xa2\xe6\xbc\xa2\xe6\xbc\xa2\xe6\xbc\xa2\xe6\xbc\xa2");
    refresh();
    check_screen("a footer with control and wide characters, too long");
    ed_set_below(&E, "");
    refresh();
    check_screen("an empty footer");

    set_prompt("\033[1;35m>\033[0m ", "> ", 2);
    refresh();
    check_screen("a coloured prompt");
    set_prompt("barb> ", "barb> ", 6);
    refresh();
    check_screen("a wider prompt");
    type("xyz");
    check_screen("typing after a wider prompt");
    ed_set_prompt(&E, "> ", -1); CHECK_INT(E.prompt_cols, 2);
    ed_set_prompt(&E, "> ", 21); CHECK_INT(E.prompt_cols, 2);
    set_prompt("> ", "> ", 2);
    refresh();
    check_screen("the prompt back");

    ed_set_above(&E, -1, "x");         // out of range: ignored
    ed_set_above(&E, ED_ABOVE, "x");
    refresh();
    CHECK_INT(E.outn, 0);

    ed_hide(&E); apply();
    check_screen("decorated, hidden");
    ed_set_above(&E, 0, "while hidden");
    refresh();
    CHECK_INT(E.outn, 0);              // nothing drawn while hidden
    ed_show(&E); apply();
    check_screen("shown with what changed while hidden");

    ed_frame(&E, 1, 0); apply();
    check_screen("one row above, no footer");
    ed_frame(&E, 1, 0);
    CHECK_INT(E.outn, 0);
    ed_frame(&E, 9, 9); apply();
    CHECK_INT(E.nabove, ED_ABOVE);
    CHECK_INT(E.has_below, 1);
    check_screen("the frame clamped");
    ed_frame(&E, -3, 0); apply();
    CHECK_INT(E.nabove, 0);
    check_screen("no frame");
    ed_frame(&E, 2, 1); apply();
    check_screen("the frame again");

    // Enter: the decoration goes and the prompt and line stay, as a record of what was sent.
    key(EK_HOME, 0);
    const char* s = enter();
    CHECK_INT(strcmp(s, "helloabcdefghijklxyz"), 0);
    char got[TC * CELL + 1];
    t_row(&T, 1, got);
    CHECK_INT(strcmp(got, "> helloabcdefghijklx"), 0);
    t_row(&T, 2, got);
    CHECK_INT(strcmp(got, "yz"), 0);
    ed_show(&E); apply();
    check_screen("a decorated prompt after Enter");
    type("abcdefghijklmnopqr");        // full at Enter, decoration and all
    enter();
    ed_show(&E); apply();
    check_screen("after Enter on a full row");

    // A resize erases the block as the terminal has reflowed it: a decoration row wider than the
    // new width takes two rows.
    start(40, deferred);
    ed_frame(&E, 2, 1); apply();
    ed_set_above(&E, 0, "012345678901234567890123456789");
    ed_set_above(&E, 1, "short");
    refresh();
    type("hello");
    CHECK_INT(E.row, 2);
    ed_resize(&E, 40);                 // the same width: erased from where it is
    CHECK_INT(E.outn, 8);
    CHECK_INT(memcmp(E.out, "\033[2A\r\033[J", 8), 0);
    apply();
    check_screen("erased for a resize");
    ed_show(&E); apply();
    ed_resize(&E, 20);
    CHECK_INT(E.outn, 8);
    CHECK_INT(memcmp(E.out, "\033[3A\r\033[J", 8), 0);
    CHECK_INT(E.shown, 0);
    CHECK_INT(E.width, 20);
    ed_resize(&E, 3);
    CHECK_INT(E.width, 10);

    // Text too long for its row is cut between whole characters.
    static char big[ED_DECOR * 2];
    for (int i = 0; i + 1 < (int)sizeof big; i += 2) memcpy(big + i, "\xc3\xa9", 2);
    big[sizeof big - 1] = 0;
    ed_set_below(&E, big);
    CHECK_INT((int)strlen(E.below), ED_DECOR - 2);
    CHECK_INT((unsigned char)E.below[ED_DECOR - 3], 0xA9);
    ed_set_prompt(&E, big, 2);
    CHECK_INT((int)strlen(E.prompt) % 2, 0);
    ed_set_below(&E, NULL);
    CHECK_INT(E.below[0], 0);

    CHECK_INT(ed_text_cols("\033[1mab\033[0m\xe6\xbc\xa2", 13), 4);
    CHECK_INT(ed_text_cols("\033" "7x", 3), 1);
    CHECK_INT(ed_text_cols("\033[1", 3), 0);
}

// ---- a random run ----

static uint32_t seed = 12345;
static uint32_t rnd(uint32_t n) { seed = seed * 1103515245u + 12345u; return (seed >> 16) % n; }

static int valid(const ed_t* e) {
    if (e->n != (int)strlen(e->line) || e->cur < 0 || e->cur > e->n) return 0;
    for (int i = 0; i < e->n;) {
        int len;
        uint32_t cp = utf8(e->line + i, e->n - i, &len);
        if (cp == 0xFFFD || i + len > e->n) return 0;
        if (i < e->cur && e->cur < i + len) return 0;      // the cursor inside a character
        i += len;
    }
    return 1;
}

// With `deco`, the decoration changes now and then too: its rows, the footer, the prompt and
// how many rows there are.
static void test_random(int deferred, int width, int deco) {
    static const uint32_t chars[] = { 'a', 'b', 'x', ' ', ' ', 0xE9, 0x4E00, 0x1F600, 0x301, '-' };
    static const char* texts[] = {
        "", "x", "\033[2m\xe2\xa0\x8b waiting \xc2\xb7 12s\033[0m", "a much longer line than any terminal here",
        "\xe6\xbc\xa2\xe6\xbc\xa2\xe6\xbc\xa2\xe6\xbc\xa2\xe6\xbc\xa2\xe6\xbc\xa2\xe6\xbc\xa2", "\033[1;32m\xe2\x97\x8f\033[0m tab\there",
    };
    static const struct { const char *s, *shows; int cols; } prompts[] = {
        { "> ", "> ", 2 }, { "\033[1;35m>\033[0m ", "> ", 2 }, { "barb> ", "barb> ", 6 },
    };
    start(width, deferred);
    if (deco) { ed_frame(&E, 2, 1); apply(); }
    for (int step = 0; step < 4000; step++) {
        int r = (int)rnd(100);
        const char* what;
        if (deco && rnd(8) == 0) {
            int d = (int)rnd(10);
            const char* t = texts[rnd((uint32_t)(sizeof texts / sizeof *texts))];
            if (d < 4) { what = "a decoration row"; ed_set_above(&E, (int)rnd(ED_ABOVE), t); }
            else if (d < 7) { what = "the footer"; ed_set_below(&E, t); }
            else if (d < 8) {
                what = "the prompt";
                int p = (int)rnd(sizeof prompts / sizeof *prompts);
                set_prompt(prompts[p].s, prompts[p].shows, prompts[p].cols);
            }
            else { what = "the frame"; ed_frame(&E, (int)rnd(ED_ABOVE + 1), (int)rnd(2)); apply(); }
            refresh();
        } else if (r < 45) { what = "typing"; if (E.n < 90) key(EK_CHAR, chars[rnd((uint32_t)(sizeof chars / sizeof *chars))]); }
        else if (r < 52) { what = "backspace"; key(EK_BACK, 0); }
        else if (r < 55) { what = "delete"; key(EK_DEL, 0); }
        else if (r < 63) { what = "left"; key(EK_LEFT, 0); }
        else if (r < 69) { what = "right"; key(EK_RIGHT, 0); }
        else if (r < 71) { what = "home"; key(EK_HOME, 0); }
        else if (r < 73) { what = "end"; key(EK_END, 0); }
        else if (r < 75) { what = "word left"; key(EK_WORD_LEFT, 0); }
        else if (r < 77) { what = "word right"; key(EK_WORD_RIGHT, 0); }
        else if (r < 80) { what = "up"; key(EK_UP, 0); }
        else if (r < 82) { what = "down"; key(EK_DOWN, 0); }
        else if (r < 83) { what = "kill to start"; key(EK_KILL_START, 0); }
        else if (r < 84) { what = "kill to end"; key(EK_KILL_END, 0); }
        else if (r < 86) { what = "kill word"; key(EK_KILL_WORD, 0); }
        else if (r < 87) { what = "clear"; key(EK_CLEAR, 0); }
        else if (r < 91) { what = "hide"; ed_hide(&E); apply(); }
        else if (r < 95) { what = "show"; ed_show(&E); apply(); }
        else {
            what = "enter";
            enter();
            if (top > TR - 30) { t_init(&T, width, deferred); t_feed(&T, "above\r\n", 7); top = 1; E.shown = 0; }
            ed_show(&E); apply();
        }
        checks_run++;
        if (!valid(&E)) { checks_failed++; printf("FAIL: the line is not valid UTF-8 after %s\n", what); return; }
        check_screen(what);
        if (step_failed) { printf("  (random run, step %d)\n", step); return; }
    }
}

int main(void) {
    for (int deferred = 0; deferred < 2; deferred++) {
        test_typing_and_wrapping(deferred);
        test_unicode(deferred);
        test_hidden(deferred);
        test_enter_and_history(deferred);
        test_decoration(deferred);
        for (int w = 10; w <= 14; w++) test_random(deferred, w, 0);
        for (int w = 10; w <= 14; w++) test_random(deferred, w, 1);
        test_random(deferred, 23, 1);
    }
    test_moving_and_killing();
    test_limits();
    return checks_report("test_edit");
}
