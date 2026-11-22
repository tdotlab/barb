// The prompt's line editor: see edit.h.
#include "edit.h"
#include <string.h>

// ---- UTF-8 ----

static uint32_t decode(const char* s, int n, int* len) {
    const unsigned char* u = (const unsigned char*)s;
    if (u[0] < 0x80 || n < 2) { *len = 1; return u[0]; }
    if ((u[0] & 0xE0) == 0xC0) { *len = 2; return ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F); }
    if ((u[0] & 0xF0) == 0xE0 && n >= 3) { *len = 3; return ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F); }
    if (n >= 4) { *len = 4; return ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) | ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F); }
    *len = 1;
    return u[0];
}

static int encode(uint32_t c, char* b) {
    if (c < 0x80) { b[0] = (char)c; return 1; }
    if (c < 0x800) { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    b[2] = (char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

static int is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

// The start of the character before byte i, and the end of the one at i.
static int prev_char(const ed_t* e, int i) { if (i > 0) i--; while (i > 0 && is_cont(e->line[i])) i--; return i; }
static int next_char(const ed_t* e, int i) { if (i < e->n) i++; while (i < e->n && is_cont(e->line[i])) i++; return i; }

// Combining marks, zero-width characters and variation selectors take no column; East Asian
// wide characters and most emoji take two. Close enough to what terminals do to keep the
// cursor in the right place for the text people type.
int ed_cols(uint32_t c) {
    if (c == 0) return 0;
    if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489) || (c >= 0x0591 && c <= 0x05BD) ||
        (c >= 0x0610 && c <= 0x061A) || (c >= 0x064B && c <= 0x065F) || (c >= 0x1AB0 && c <= 0x1AFF) ||
        (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x200B && c <= 0x200F) || (c >= 0x20D0 && c <= 0x20FF) ||
        (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) || c == 0xFEFF ||
        (c >= 0xE0100 && c <= 0xE01EF))
        return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
        (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
        (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
        (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

// The length of the escape sequence at s (ESC [ ... and its final byte, or ESC and one byte), or 0.
static int esc_len(const char* s, size_t n) {
    if (n < 2 || s[0] != 0x1B) return 0;
    if (s[1] != '[') return 2;
    size_t i = 2;
    while (i < n && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7E)) i++;
    return i < n ? (int)i + 1 : (int)n;
}

int ed_text_cols(const char* s, size_t n) {
    int cols = 0;
    for (size_t i = 0; i < n;) {
        int k = esc_len(s + i, n - i);
        if (k) { i += (size_t)k; continue; }
        int len;
        cols += ed_cols(decode(s + i, (int)(n - i), &len));
        i += (size_t)len;
    }
    return cols;
}

// ---- output ----

static void put(ed_t* e, const char* s, int n) {
    if (n > (int)sizeof e->out - e->outn) n = (int)sizeof e->out - e->outn;
    memcpy(e->out + e->outn, s, (size_t)n);
    e->outn += n;
}
static void putz(ed_t* e, const char* s) { put(e, s, (int)strlen(s)); }

static void put_csi(ed_t* e, int n, char final) {   // ESC [ n final
    char b[16];
    int k = 0;
    char digits[12];
    int d = 0;
    b[k++] = 0x1B;
    b[k++] = '[';
    do { digits[d++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (d) b[k++] = digits[--d];
    b[k++] = final;
    put(e, b, k);
}

static void put_up(ed_t* e, int n) { if (n > 0) put_csi(e, n, 'A'); }
static void put_down(ed_t* e, int n) { if (n > 0) put_csi(e, n, 'B'); }

// A decoration row, cut to leave the last column free (so it never wraps) and with its colours
// reset after it. Control characters become spaces. The columns it took.
static int put_decor(ed_t* e, const char* s) {
    int cols = 0, styled = 0;
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        int k = esc_len(s + i, n - i);
        if (k) { put(e, s + i, k); styled = 1; i += (size_t)k; continue; }
        int len;
        uint32_t c = decode(s + i, (int)(n - i), &len);
        int ctl = c < 0x20 || c == 0x7F;
        int w = ctl ? 1 : ed_cols(c);
        if (cols + w > e->width - 1) break;
        if (ctl) put(e, " ", 1); else put(e, s + i, len);
        cols += w;
        i += (size_t)len;
    }
    if (styled) putz(e, "\033[0m");
    return cols;
}

// ---- where things are ----

// Where the terminal's cursor is after the prompt and the first `upto` bytes of the line:
// the row below the prompt's, and the column, which is `width` when the row is full and the
// terminal is waiting to wrap with the next character.
static void where(const ed_t* e, int upto, int* row, int* col) {
    int r = 0, c = e->prompt_cols;
    for (int i = 0; i < upto;) {
        int len;
        int w = ed_cols(decode(e->line + i, e->n - i, &len));
        if (c + w > e->width) { r++; c = 0; }
        c += w;
        i += len;
    }
    *row = r;
    *col = c;
}

// The cursor's place for byte `upto`, from the prompt's row: at the start of the next row when
// the one it is on is full.
static void cursor_at(const ed_t* e, int upto, int* row, int* col) {
    where(e, upto, row, col);
    if (*col >= e->width) { (*row)++; *col = 0; }
}

// A row filled to the last column leaves the cursor there on some terminals, waiting to wrap,
// and on the next row on others. A space and a carriage return put it at the start of the next
// row on both, so the editor always knows where it is.
static int settle(ed_t* e, int row, int col) {
    if (col < e->width) return row;
    putz(e, " \r");
    return row + 1;
}

// ---- drawing ----

static void hide(ed_t* e) {
    if (!e->shown) return;
    put_up(e, e->row);
    putz(e, "\r\033[J");
    e->shown = 0;
    e->row = 0;
}

// From the start of the prompt's row: the prompt, the line and the footer, then the cursor put
// in place.
static void draw_line(ed_t* e) {
    putz(e, e->prompt);
    put(e, e->line, e->n);
    int er, ec;
    where(e, e->n, &er, &ec);
    er = settle(e, er, ec);
    if (e->has_below) {        // below the line, or below the empty row after a full one
        putz(e, "\r\n");
        er++;
        put_decor(e, e->below);
    }
    e->shown = 1;
    e->dirty &= ~2;
    int cr, cc;
    cursor_at(e, e->cur, &cr, &cc);
    e->row = e->nabove + cr;
    if (e->cur == e->n && !e->has_below) return;   // at the end of the line: already there
    put_up(e, er - cr);
    putz(e, "\r");
    if (cc) put_csi(e, cc, 'C');
}

// Draw the whole block from the start of the cursor's row.
static void draw(ed_t* e) {
    for (int i = 0; i < e->nabove; i++) {
        e->above_cols[i] = put_decor(e, e->above[i]);
        putz(e, "\r\n");
    }
    e->dirty &= ~1;
    draw_line(e);
}

// The prompt, the line and the footer again; the rows above stay as they are.
static void redraw(ed_t* e) {
    if (!e->shown) return;
    put_up(e, e->row - e->nabove);
    putz(e, "\r\033[J");
    draw_line(e);
}

// Rewrite the rows above in place and come back to the cursor.
static void redraw_above(ed_t* e) {
    e->dirty &= ~1;
    if (!e->nabove) return;
    put_up(e, e->row);
    for (int i = 0; i < e->nabove; i++) {
        if (i) put_down(e, 1);
        putz(e, "\r");
        e->above_cols[i] = put_decor(e, e->above[i]);
        putz(e, "\033[K");
    }
    put_down(e, e->row - (e->nabove - 1));
    putz(e, "\r");
    int cr, cc;
    cursor_at(e, e->cur, &cr, &cc);
    if (cc) put_csi(e, cc, 'C');
}

void ed_hide(ed_t* e) {
    e->outn = 0;
    hide(e);
}

void ed_show(ed_t* e) {
    e->outn = 0;
    if (!e->shown) draw(e);
}

void ed_refresh(ed_t* e) {
    e->outn = 0;
    if (!e->shown) return;
    if (e->dirty & 1) redraw_above(e);
    if (e->dirty & 2) redraw(e);
}

void ed_frame(ed_t* e, int nabove, int below) {
    e->outn = 0;
    if (nabove < 0) nabove = 0;
    if (nabove > ED_ABOVE) nabove = ED_ABOVE;
    below = below != 0;
    if (nabove == e->nabove && below == e->has_below) return;
    int shown = e->shown;
    hide(e);
    e->nabove = nabove;
    e->has_below = below;
    if (shown) draw(e);
}

// Copy s into dst (cut whole characters to fit) and set `bit` in dirty if it changed.
static void set_text(char* dst, size_t cap, const char* s, int* dirty, int bit) {
    if (!s) s = "";
    size_t n = strlen(s);
    if (n > cap - 1) {
        n = cap - 1;
        while (n && is_cont(s[n])) n--;
    }
    if (!strncmp(dst, s, n) && dst[n] == 0) return;
    memcpy(dst, s, n);
    dst[n] = 0;
    *dirty |= bit;
}

void ed_set_above(ed_t* e, int i, const char* s) {
    if (i < 0 || i >= ED_ABOVE) return;
    set_text(e->above[i], sizeof e->above[i], s, &e->dirty, 1);
}

void ed_set_below(ed_t* e, const char* s) { set_text(e->below, sizeof e->below, s, &e->dirty, 2); }

void ed_set_prompt(ed_t* e, const char* s, int cols) {
    if (cols < 0 || cols > 20) cols = 2;
    set_text(e->prompt, sizeof e->prompt, s, &e->dirty, 2);
    if (cols != e->prompt_cols) { e->prompt_cols = cols; e->dirty |= 2; }
}

void ed_init(ed_t* e, int width) {
    memset(e, 0, sizeof *e);
    strcpy(e->prompt, "> ");
    e->prompt_cols = 2;
    ed_set_width(e, width);
}

void ed_set_width(ed_t* e, int width) { e->width = width < 10 ? 10 : width; }

void ed_resize(ed_t* e, int width) {
    e->outn = 0;
    if (width < 10) width = 10;
    if (e->shown && width != e->width) {
        // Where the cursor is now, if the terminal rewrapped every row of the block to the new
        // width: a decoration row wider than it takes more rows, and the line wraps afresh.
        int up = 0;
        for (int i = 0; i < e->nabove; i++) up += e->above_cols[i] > width ? (e->above_cols[i] + width - 1) / width : 1;
        e->width = width;
        int cr, cc;
        cursor_at(e, e->cur, &cr, &cc);
        e->row = up + cr;
    }
    hide(e);
    e->width = width;
}

// ---- history ----

// The start of the history line `back` lines from the end (1 is the most recent), or -1.
static int hist_at(const ed_t* e, int back) {
    int i = e->hn;
    for (int k = 0; k < back; k++) {
        if (i == 0) return -1;
        i--;                                   // the NUL that ends the line before
        while (i > 0 && e->hist[i - 1]) i--;
    }
    return i;
}

static void hist_add(ed_t* e) {
    if (!e->n) return;
    int last = hist_at(e, 1);
    if (last >= 0 && !strcmp(e->hist + last, e->line)) return;   // the same as the one before
    int need = e->n + 1;
    if (need > ED_HIST) return;
    while (e->hn + need > ED_HIST) {           // drop the oldest lines
        int drop = (int)strlen(e->hist) + 1;
        memmove(e->hist, e->hist + drop, (size_t)(e->hn - drop));
        e->hn -= drop;
    }
    memcpy(e->hist + e->hn, e->line, (size_t)need);
    e->hn += need;
}

static void load(ed_t* e, const char* s, int n) {
    if (n > ED_MAX - 1) n = ED_MAX - 1;
    memmove(e->line, s, (size_t)n);
    e->n = e->cur = n;
    e->line[n] = 0;
}

static void hist_move(ed_t* e, int dir) {
    int to = e->back + dir;
    if (to < 0) return;
    int at = to ? hist_at(e, to) : 0;
    if (to && at < 0) return;                  // already at the oldest
    if (e->back == 0) { memcpy(e->draft, e->line, (size_t)e->n); e->draftn = e->n; }
    e->back = to;
    if (to == 0) load(e, e->draft, e->draftn);
    else load(e, e->hist + at, (int)strlen(e->hist + at));
    redraw(e);
}

// ---- editing ----

static void cut(ed_t* e, int from, int to) {
    memmove(e->line + from, e->line + to, (size_t)(e->n - to));
    e->n -= to - from;
    e->line[e->n] = 0;
    if (e->cur > to) e->cur -= to - from;
    else if (e->cur > from) e->cur = from;
}

static int is_space_at(const ed_t* e, int i) { return e->line[i] == ' ' || e->line[i] == '\t'; }

static int word_left(const ed_t* e) {
    int i = e->cur;
    while (i > 0 && is_space_at(e, prev_char(e, i))) i = prev_char(e, i);
    while (i > 0 && !is_space_at(e, prev_char(e, i))) i = prev_char(e, i);
    return i;
}

static int word_right(const ed_t* e) {
    int i = e->cur;
    while (i < e->n && is_space_at(e, i)) i = next_char(e, i);
    while (i < e->n && !is_space_at(e, i)) i = next_char(e, i);
    return i;
}

static void insert(ed_t* e, uint32_t ch) {
    if (ch == '\t') ch = ' ';
    if (ch < 0x20 || ch == 0x7F || (ch >= 0xD800 && ch <= 0xDFFF) || ch > 0x10FFFF) return;
    char b[4];
    int k = encode(ch, b);
    if (e->n + k > ED_MAX - 1) return;
    // Typing at the end needs only the character, unless it is one that takes no column (that
    // belongs to the character before, which may be on the row above the cursor), or, with a
    // footer below, one that moves the end of the line onto another row.
    int r0, c0;
    where(e, e->n, &r0, &c0);
    int at_end = e->cur == e->n && ed_cols(ch) > 0;
    memmove(e->line + e->cur + k, e->line + e->cur, (size_t)(e->n - e->cur));
    memcpy(e->line + e->cur, b, (size_t)k);
    e->n += k;
    e->cur += k;
    e->line[e->n] = 0;
    if (!e->shown) return;
    int r, c;
    where(e, e->n, &r, &c);
    if (at_end && e->has_below && (r != r0 || c >= e->width || c0 >= e->width)) at_end = 0;
    if (at_end) {
        put(e, b, k);
        e->row = e->nabove + settle(e, r, c);
    } else {
        redraw(e);
    }
}

void ed_key(ed_t* e, int key, uint32_t ch) {
    e->outn = 0;
    int was = e->cur;
    switch (key) {
    case EK_CHAR: insert(e, ch); return;
    case EK_BACK:
        if (!e->cur) return;
        cut(e, prev_char(e, e->cur), e->cur);
        break;
    case EK_DEL:
        if (e->cur == e->n) return;
        cut(e, e->cur, next_char(e, e->cur));
        break;
    case EK_LEFT: e->cur = prev_char(e, e->cur); break;
    case EK_RIGHT: e->cur = next_char(e, e->cur); break;
    case EK_HOME: e->cur = 0; break;
    case EK_END: e->cur = e->n; break;
    case EK_WORD_LEFT: e->cur = word_left(e); break;
    case EK_WORD_RIGHT: e->cur = word_right(e); break;
    case EK_UP: hist_move(e, 1); return;
    case EK_DOWN: hist_move(e, -1); return;
    case EK_KILL_START: if (!e->cur) return; cut(e, 0, e->cur); break;
    case EK_KILL_END: if (e->cur == e->n) return; cut(e, e->cur, e->n); break;
    case EK_KILL_WORD: if (!e->cur) return; cut(e, word_left(e), e->cur); break;
    case EK_CLEAR: if (!e->n) return; e->n = e->cur = 0; e->line[0] = 0; e->back = 0; break;
    default: return;
    }
    if (key >= EK_LEFT && key <= EK_WORD_RIGHT && e->cur == was) return;   // nowhere to go
    redraw(e);
}

const char* ed_take(ed_t* e, int* n) {
    e->outn = 0;
    if (e->shown) {
        if (e->nabove || e->has_below) {
            // The decoration goes; the prompt and the line are written again where the block was.
            hide(e);
            int nab = e->nabove, below = e->has_below;
            e->nabove = e->has_below = 0;
            e->cur = e->n;
            draw_line(e);
            e->nabove = nab;
            e->has_below = below;
        } else if (e->cur != e->n) {
            e->cur = e->n;
            redraw(e);
        }
        int r, c;
        where(e, e->n, &r, &c);
        if (c < e->width) putz(e, "\r\n");     // a full last row has already put it on a new one
    }
    e->shown = 0;
    e->row = 0;
    hist_add(e);
    // The returned line lives in `draft`, which is free again now that the history is not shown.
    memcpy(e->draft, e->line, (size_t)e->n + 1);
    *n = e->n;
    e->n = e->cur = 0;
    e->line[0] = 0;
    e->back = 0;
    e->draftn = 0;
    return e->draft;
}
