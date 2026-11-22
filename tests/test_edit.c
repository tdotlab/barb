// Tests for the prompt's line editor (src/ui/edit.c).
//
// The editor only produces bytes for a terminal, so the test plays them on a small terminal
// emulator and looks at the screen: what each row shows and where the cursor is. It does this
// for both of the ways terminals treat a row filled to the last column (move to the next row at
// once, or wait for the next character), because the editor must keep track of the cursor on
// either. After the scripted cases, a long random run checks the screen after every key.
#include "check.h"
#include "../src/ui/edit.h"

// ---- a terminal ----

#define TR 160
#define TC 40
#define CELL 24

typedef struct {
    char cell[TR][TC][CELL];   // the UTF-8 in each cell: "" is empty, "\x01" the right half of a wide character
    int r, c;                  // the cursor
    int pend;                  // waiting to wrap with the next character (the deferred kind)
    int width, deferred;
    int lr, lc;                // the cell last written, where a combining mark goes
    int bad;                   // sequences this terminal does not know, or a cursor sent off the screen
} term_t;

static uint32_t utf8(const char* s, int n, int* len) {
    const unsigned char* u = (const unsigned char*)s;
    if (u[0] < 0x80) { *len = 1; return u[0]; }
    if ((u[0] & 0xE0) == 0xC0 && n >= 2) { *len = 2; return ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F); }
    if ((u[0] & 0xF0) == 0xE0 && n >= 3) { *len = 3; return ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F); }
    if ((u[0] & 0xF8) == 0xF0 && n >= 4) { *len = 4; return ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) | ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F); }
    *len = 1;
    return 0xFFFD;
}

static void t_init(term_t* t, int width, int deferred) {
    memset(t, 0, sizeof *t);
    t->width = width;
    t->deferred = deferred;
    t->lr = t->lc = -1;
}

static void t_erase(term_t* t, int r, int c) {   // from (r, c) to the end of the screen
    for (; r < TR; r++) {
        for (; c < TC; c++) t->cell[r][c][0] = 0;
        c = 0;
    }
}

static void t_put(term_t* t, uint32_t cp, const char* b, int len) {
    int w = ed_cols(cp);
    if (w == 0) {
        if (t->lr >= 0 && strlen(t->cell[t->lr][t->lc]) + (size_t)len < CELL)
            strncat(t->cell[t->lr][t->lc], b, (size_t)len);
        return;
    }
    if (t->pend) { t->r++; t->c = 0; t->pend = 0; }
    if (t->c + w > t->width) { t->r++; t->c = 0; }
    if (t->r >= TR) { t->bad++; t->r = TR - 1; return; }
    memcpy(t->cell[t->r][t->c], b, (size_t)len);
    t->cell[t->r][t->c][len] = 0;
    if (w == 2) strcpy(t->cell[t->r][t->c + 1], "\x01");
    t->lr = t->r;
    t->lc = t->c;
    t->c += w;
    if (t->c >= t->width) {
        if (t->deferred) { t->c = t->width - 1; t->pend = 1; }
        else { t->r++; t->c = 0; }
    }
}

static void t_feed(term_t* t, const char* s, int n) {
    for (int i = 0; i < n;) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == 0x1B) {
            if (i + 1 >= n || s[i + 1] != '[') { t->bad++; i++; continue; }
            int j = i + 2, num = 0, has = 0;
            while (j < n && s[j] >= '0' && s[j] <= '9') { num = num * 10 + (s[j] - '0'); has = 1; j++; }
            if (j >= n) { t->bad++; return; }
            char f = s[j];
            i = j + 1;
            switch (f) {
            case 'A': t->r -= has ? num : 1; if (t->r < 0) { t->bad++; t->r = 0; } t->pend = 0; break;
            case 'C': t->c += has ? num : 1; if (t->c > t->width - 1) t->c = t->width - 1; t->pend = 0; break;
            case 'J': if (num) t->bad++; else t_erase(t, t->r, t->c); break;
            default: t->bad++;
            }
            continue;
        }
        if (ch == '\r') { t->c = 0; t->pend = 0; i++; continue; }
        if (ch == '\n') { t->r++; t->pend = 0; i++; continue; }
        if (ch < 0x20 || ch == 0x7F) { t->bad++; i++; continue; }
        int len;
        uint32_t cp = utf8(s + i, n - i, &len);
        t_put(t, cp, s + i, len);
        i += len;
    }
}

// A row as text, without the spaces at its end.
static void t_row(const term_t* t, int r, char* b) {
    int k = 0;
    for (int c = 0; c < TC; c++) {
        const char* x = t->cell[r][c];
        if (!strcmp(x, "\x01")) continue;
        if (!*x) { b[k++] = ' '; continue; }
        size_t l = strlen(x);
        memcpy(b + k, x, l);
        k += (int)l;
    }
    while (k && b[k - 1] == ' ') k--;
    b[k] = 0;
}

// ---- what the screen should show ----

typedef struct {
    char rows[TR][TC * 4 + 1];
    int n;                     // rows the prompt and line take
    int cr, cc;                // where the cursor should be, from the prompt's row
} layout_t;

// Worked out independently of the editor: the prompt, then each character on the row it fits
// in, a row filled to the last column putting the cursor at the start of the next.
static void layout(layout_t* L, const char* line, int n, int cur, int width) {
    memset(L, 0, sizeof *L);
    int r = 0, c = 2, k = 2;
    strcpy(L->rows[0], "> ");
    L->cr = 0; L->cc = 2;
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
    ed_show(&E);
    apply();
}

// The screen shows exactly the prompt and the line, with the cursor in place; or, hidden, nothing.
static void check_screen(const char* when) {
    char got[TC * CELL + 1];
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
    layout(&L, E.line, E.n, E.cur, T.width);
    for (int i = 0; i < L.n; i++) {
        t_row(&T, top + i, got);
        if (strcmp(got, L.rows[i])) {
            char d[TC * CELL * 2 + 64];
            snprintf(d, sizeof d, "%s: row %d is \"%s\", should be \"%s\"", when, i, got, L.rows[i]);
            fail("the screen does not show the line", d);
            return;
        }
    }
    for (int r = top + L.n; r < TR; r++) {
        t_row(&T, r, got);
        if (*got) { fail("something is left below the line", when); return; }
    }
    if (T.pend || T.r != top + L.cr || T.c != L.cc) {
        char d[200];
        snprintf(d, sizeof d, "%s: at row %d col %d%s, should be row %d col %d", when, T.r - top, T.c,
                 T.pend ? " waiting to wrap" : "", L.cr, L.cc);
        fail("the cursor is in the wrong place", d);
        return;
    }
    if (E.row != L.cr) fail("the editor lost track of the cursor's row", when);
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

static void test_random(int deferred, int width) {
    static const uint32_t chars[] = { 'a', 'b', 'x', ' ', ' ', 0xE9, 0x4E00, 0x1F600, 0x301, '-' };
    start(width, deferred);
    for (int step = 0; step < 4000; step++) {
        int r = (int)rnd(100);
        const char* what;
        if (r < 45) { what = "typing"; if (E.n < 90) key(EK_CHAR, chars[rnd((uint32_t)(sizeof chars / sizeof *chars))]); }
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
        for (int w = 10; w <= 14; w++) test_random(deferred, w);
    }
    test_moving_and_killing();
    test_limits();
    return checks_report("test_edit");
}
