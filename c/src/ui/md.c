// Markdown for the terminal: see md.h.
#include "md.h"
#include "edit.h"
#include <stdint.h>
#include <string.h>

enum { S_BOLD = 1, S_DIM = 2, S_ITAL = 4, S_ULINE = 8, S_STRIKE = 16, S_CODE = 32 };

// ---- writing ----

static void w(md_t* m, const char* s, size_t n) { if (n) m->write(m->ctx, s, n); }
static void wz(md_t* m, const char* s) { w(m, s, strlen(s)); }

// Put the terminal in style s: a reset, then each attribute it has.
static void style(md_t* m, int s) {
    if (s == m->cur) return;
    char b[32];
    int k = 0;
    b[k++] = 0x1B;
    b[k++] = '[';
    b[k++] = '0';
    if (s & S_BOLD) { b[k++] = ';'; b[k++] = '1'; }
    if (s & S_DIM) { b[k++] = ';'; b[k++] = '2'; }
    if (s & S_ITAL) { b[k++] = ';'; b[k++] = '3'; }
    if (s & S_ULINE) { b[k++] = ';'; b[k++] = '4'; }
    if (s & S_STRIKE) { b[k++] = ';'; b[k++] = '9'; }
    if (s & S_CODE) { b[k++] = ';'; b[k++] = '3'; b[k++] = '6'; }
    b[k++] = 'm';
    w(m, b, (size_t)k);
    m->cur = s;
}

// Bytes with their styles, a run of one style at a time.
static void span(md_t* m, const char* b, const unsigned char* st, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && st[j] == st[i]) j++;
        style(m, st[i]);
        w(m, b + i, j - i);
        i = j;
    }
}

static void end_line(md_t* m) {
    style(m, 0);
    w(m, "\n", 1);
}

static void spaces(md_t* m, int n) {
    static const char sp[] = "                                ";
    style(m, 0);
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        w(m, sp, (size_t)k);
        n -= k;
    }
}

void md_safe(md_write_fn write, void* ctx, const char* s, size_t n) {
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char rep[4];
        size_t rn = 0, skip = 1;
        if (c == '\r' && i + 1 < n && s[i + 1] == '\n') rn = 0;
        else if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) { rep[0] = '^'; rep[1] = (char)(c == 0x7F ? '?' : c + 0x40); rn = 2; }
        else if (c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0x9F) {
            memcpy(rep, "\xEF\xBF\xBD", 3);
            rn = 3;
            skip = 2;
        } else continue;
        if (i > start) write(ctx, s + start, i - start);
        if (rn) write(ctx, rep, rn);
        start = i + skip;
        i += skip - 1;
    }
    if (n > start) write(ctx, s + start, n - start);
}

// ---- UTF-8 and columns ----

static uint32_t dec(const char* s, size_t n, int* len) {
    const unsigned char* u = (const unsigned char*)s;
    if (u[0] < 0x80 || n < 2) { *len = 1; return u[0]; }
    if ((u[0] & 0xE0) == 0xC0) { *len = 2; return ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F); }
    if ((u[0] & 0xF0) == 0xE0 && n >= 3) { *len = 3; return ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F); }
    if (n >= 4) { *len = 4; return ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) | ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F); }
    *len = 1;
    return u[0];
}

static int cols(const char* s, size_t n) {
    int c = 0;
    for (size_t i = 0; i < n;) {
        int len;
        c += ed_cols(dec(s + i, n - i, &len));
        i += (size_t)len;
    }
    return c;
}

// ---- the line being built ----

static void add1(md_t* m, char c, int st) {
    if (m->n >= MD_LINE) { m->over = 1; return; }
    m->b[m->n] = c;
    m->st[m->n++] = (unsigned char)st;
}

// Text into the line, its control characters made visible as md_safe does (a tab is a space).
static void add(md_t* m, const char* s, size_t n, int st) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t') add1(m, ' ', st);
        else if (c < 0x20 || c == 0x7F) { add1(m, '^', st); add1(m, (char)(c == 0x7F ? '?' : c + 0x40), st); }
        else if (c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0x9F) {
            add1(m, '\xEF', st); add1(m, '\xBF', st); add1(m, '\xBD', st);
            i++;
        } else add1(m, (char)c, st);
    }
}

static void addz(md_t* m, const char* s, int st) { add(m, s, strlen(s), st); }

// ---- inline: code, emphasis, links ----

static int is_space(char c) { return c == ' ' || c == '\t'; }
static int is_alnum(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u >= 0x80;
}
static int is_punct(char c) { return c > 32 && c < 127 && !is_alnum(c); }
static int special(char c) { return c == '\\' || c == '`' || c == '*' || c == '_' || c == '~' || c == '[' || c == '!' || c == '<'; }

// Is there a closing delimiter d after `from`, so that an opening one there means something?
static int closer_ahead(const char* s, size_t n, size_t from, const char* d, size_t dl) {
    for (size_t j = from + 1; j + dl <= n; j++) {
        if (memcmp(s + j, d, dl)) continue;
        if (is_space(s[j - 1])) continue;
        if (dl == 1 && s[j - 1] == d[0]) continue;
        if (d[0] == '_' && j + dl < n && is_alnum(s[j + dl])) continue;
        return 1;
    }
    return 0;
}

static size_t run_of(const char* s, size_t n, size_t i, char c) {
    size_t j = i;
    while (j < n && s[j] == c) j++;
    return j - i;
}

static int starts(const char* s, size_t n, const char* p) {
    size_t k = strlen(p);
    return n >= k && !memcmp(s, p, k);
}

static void inl(md_t* m, const char* s, size_t n, int base) {
    int open = 0;
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        int st = base | open;
        if (c == '\\' && i + 1 < n && is_punct(s[i + 1])) { add(m, s + i + 1, 1, st); i += 2; continue; }
        if (c == '`') {
            size_t k = run_of(s, n, i, '`'), j = i + k, found = 0;
            while (j < n) {
                if (s[j] != '`') { j++; continue; }
                size_t r = run_of(s, n, j, '`');
                if (r == k) { found = j; break; }
                j += r;
            }
            if (found) {
                const char* p = s + i + k;
                size_t len = found - (i + k), blank = 0;
                while (blank < len && p[blank] == ' ') blank++;
                if (len >= 2 && p[0] == ' ' && p[len - 1] == ' ' && blank < len) { p++; len -= 2; }
                add(m, p, len, st | S_CODE);
                i = found + k;
            } else {
                add(m, s + i, k, st);
                i += k;
            }
            continue;
        }
        if (c == '*' || c == '_' || (c == '~' && i + 1 < n && s[i + 1] == '~')) {
            size_t dl = i + 1 < n && s[i + 1] == c ? 2 : 1;
            int bit = c == '~' ? S_STRIKE : dl == 2 ? S_BOLD : S_ITAL;
            char d[2] = { c, c };
            if ((open & bit) && i > 0 && !is_space(s[i - 1]) && (c != '_' || i + dl >= n || !is_alnum(s[i + dl]))) {
                open &= ~bit;
                i += dl;
                continue;
            }
            if (!(open & bit) && i + dl < n && !is_space(s[i + dl]) && (c != '_' || i == 0 || !is_alnum(s[i - 1])) &&
                closer_ahead(s, n, i + dl, d, dl)) {
                open |= bit;
                i += dl;
                continue;
            }
            add(m, s + i, dl, st);
            i += dl;
            continue;
        }
        if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {
            size_t a = c == '!' ? i + 2 : i + 1, close = a;
            int depth = 1;
            while (close < n) {
                if (s[close] == '\\') { close += 2; continue; }
                if (s[close] == '[') depth++;
                else if (s[close] == ']' && --depth == 0) break;
                close++;
            }
            if (close + 1 < n && s[close] == ']' && s[close + 1] == '(') {
                size_t u = close + 2, e = u;
                int pd = 1;
                while (e < n && !is_space(s[e])) {
                    if (s[e] == '(') pd++;
                    else if (s[e] == ')' && --pd == 0) break;
                    e++;
                }
                if (e < n && s[e] == ')') {
                    size_t tl = close - a, ul = e - u;
                    if (c == '!') { if (tl) inl(m, s + a, tl, st); else addz(m, "image", st); }
                    else inl(m, s + a, tl, st | S_ULINE);
                    if (c == '!' || tl != ul || memcmp(s + a, s + u, tl)) {
                        addz(m, " (", S_DIM);
                        add(m, s + u, ul, S_DIM);
                        addz(m, ")", S_DIM);
                    }
                    i = e + 1;
                    continue;
                }
            }
        }
        if (c == '<' && (starts(s + i + 1, n - i - 1, "http://") || starts(s + i + 1, n - i - 1, "https://") ||
                         starts(s + i + 1, n - i - 1, "mailto:"))) {
            size_t e = i + 1;
            while (e < n && s[e] != '>' && s[e] != '<' && !is_space(s[e])) e++;
            if (e < n && s[e] == '>') {
                add(m, s + i + 1, e - i - 1, st | S_ULINE);
                i = e + 1;
                continue;
            }
        }
        size_t j = i + 1;
        while (j < n && !special(s[j])) j++;
        add(m, s + i, j - i, st);
        i = j;
    }
}

// ---- wrapping ----

static int wrap_width(const md_t* m) { return m->width - 1 < 20 ? 20 : m->width - 1; }

// Show the line built so far, wrapped: `p1` (it takes c1 columns) goes before the first row and
// `p2` (c2) before the rest. The prefixes may carry escapes.
static void flow(md_t* m, const char* p1, int c1, const char* p2, int c2) {
    int W = wrap_width(m);
    size_t n = (size_t)m->n, i = 0;
    style(m, 0);
    wz(m, p1);
    int col = c1, empty = 1;
    while (i < n && m->b[i] == ' ') i++;
    while (i < n) {
        size_t sp = i;
        while (i < n && m->b[i] == ' ') i++;
        size_t nsp = i - sp;
        if (i >= n) break;                     // spaces at the end of the line
        size_t ws = i;
        while (i < n && m->b[i] != ' ') i++;
        int wc = cols(m->b + ws, i - ws);
        if (!empty && col + (int)nsp + wc > W) {
            end_line(m);
            wz(m, p2);
            col = c2;
            nsp = 0;
            empty = 1;
        }
        if (nsp) { span(m, m->b + sp, m->st + sp, nsp); col += (int)nsp; }
        if (col + wc <= W) {
            span(m, m->b + ws, m->st + ws, i - ws);
            col += wc;
        } else {                               // longer than a row: break it where it fills one
            for (size_t k = ws; k < i;) {
                int len;
                int cw = ed_cols(dec(m->b + k, i - k, &len));
                if (col + cw > W && !empty) { end_line(m); wz(m, p2); col = c2; }
                span(m, m->b + k, m->st + k, (size_t)len);
                col += cw;
                empty = 0;
                k += (size_t)len;
            }
        }
        empty = 0;
    }
    end_line(m);
}

// A line too long to style: as it is, made safe.
static void plain_line(md_t* m, const char* s, size_t n) {
    style(m, 0);
    md_safe(m->write, m->ctx, s, n);
    w(m, "\n", 1);
}

static void para(md_t* m, const char* s, size_t n, int base, const char* p1, int c1, const char* p2, int c2) {
    m->n = m->over = 0;
    inl(m, s, n, base);
    if (m->over) plain_line(m, s, n);
    else flow(m, p1, c1, p2, c2);
}

// ---- blocks ----

static size_t indent_of(const char* s, size_t n) {
    size_t i = 0;
    while (i < n && s[i] == ' ') i++;
    return i;
}

static int is_blank(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) if (!is_space(s[i])) return 0;
    return 1;
}

// A fence: three or more backticks or tildes. Its length, or 0.
static size_t fence_len(const char* s, size_t n, char* ch) {
    if (!n || (s[0] != '`' && s[0] != '~')) return 0;
    size_t k = run_of(s, n, 0, s[0]);
    if (k < 3) return 0;
    if (s[0] == '`') for (size_t i = k; i < n; i++) if (s[i] == '`') return 0;   // `` ``` x ``` `` is inline
    *ch = s[0];
    return k;
}

static int is_rule(const char* s, size_t n) {
    char c = n ? s[0] : 0;
    if (c != '-' && c != '*' && c != '_') return 0;
    int k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == c) k++;
        else if (!is_space(s[i])) return 0;
    }
    return k >= 3;
}

static void rule(md_t* m) {
    int W = wrap_width(m), k = W > 80 ? 80 : W;
    style(m, S_DIM);
    for (int i = 0; i < k; i++) wz(m, m->ascii ? "-" : "\xE2\x94\x80");
    end_line(m);
}

// ---- tables ----

// The cells of a table row: offsets and lengths, trimmed. The count, or -1 if there are too many.
static int split_cells(const char* s, size_t n, size_t* off, size_t* len, int max) {
    size_t a = 0, b = n;
    while (a < b && is_space(s[a])) a++;
    while (b > a && is_space(s[b - 1])) b--;
    if (a < b && s[a] == '|') a++;
    if (b > a && s[b - 1] == '|' && !(b - 1 > a && s[b - 2] == '\\')) b--;
    int k = 0, code = 0;
    size_t start = a;
    for (size_t i = a; i <= b; i++) {
        if (i < b && s[i] == '\\') { i++; continue; }
        if (i < b && s[i] == '`') { code = !code; continue; }
        if (i < b && (s[i] != '|' || code)) continue;
        if (k == max) return -1;
        size_t x = start, y = i;
        while (x < y && is_space(s[x])) x++;
        while (y > x && is_space(s[y - 1])) y--;
        off[k] = x;
        len[k] = y - x;
        k++;
        start = i + 1;
    }
    return k;
}

// The row under a table's header: |---|:--:|--:|. The number of columns, with their alignment
// (0 left, 1 centre, 2 right), or 0.
static int delim_row(const char* s, size_t n, int* align) {
    size_t off[MD_COLS], len[MD_COLS];
    if (!memchr(s, '-', n)) return 0;
    int k = split_cells(s, n, off, len, MD_COLS);
    if (k <= 0) return 0;
    for (int c = 0; c < k; c++) {
        const char* p = s + off[c];
        size_t l = len[c], i = 0;
        int left = l && p[0] == ':', right = l && p[l - 1] == ':';
        if (left) i++;
        size_t dashes = 0;
        while (i < l - (size_t)right && p[i] == '-') { i++; dashes++; }
        if (!dashes || i != l - (size_t)right) return 0;
        align[c] = left && right ? 1 : right ? 2 : 0;
    }
    return k;
}

static size_t line_end(const char* s, size_t n, size_t i) {
    while (i < n && s[i] != '\n') i++;
    return i;
}

static size_t trim_cr(const char* s, size_t a, size_t e) { return e > a && s[e - 1] == '\r' ? e - 1 : e; }

// Keep the line built in m->b as table cell (r, c). 0 if there is no room.
static int keep_cell(md_t* m, int r, int c) {
    if (m->over || m->tn + m->n > MD_TABLE) return 0;
    memcpy(m->tb + m->tn, m->b, (size_t)m->n);
    memcpy(m->tst + m->tn, m->st, (size_t)m->n);
    m->cell[r][c].off = m->tn;
    m->cell[r][c].len = m->n;
    m->cell[r][c].cols = cols(m->b, (size_t)m->n);
    m->tn += m->n;
    return 1;
}

static void cell_out(md_t* m, int r, int c) {
    const md_cell_t* x = &m->cell[r][c];
    span(m, m->tb + x->off, m->tst + x->off, (size_t)x->len);
}

// A table whose header line is s[i..e). Where the text after it starts (n + 1 at the end).
static size_t table(md_t* m, const char* s, size_t n, size_t i, size_t e, int ncols, const int* align) {
    size_t starts_at[MD_ROWS], ends_at[MD_ROWS];
    int rows = 0, fits = 1;
    starts_at[rows] = i;
    ends_at[rows++] = trim_cr(s, i, e);
    size_t next = line_end(s, n, e + 1);              // the end of the last line taken: the delimiter row
    while (next < n) {
        size_t a = next + 1, b = line_end(s, n, a), t = trim_cr(s, a, b);
        if (is_blank(s + a, t - a) || !memchr(s + a, '|', t - a)) break;
        if (rows == MD_ROWS) { fits = 0; break; }
        starts_at[rows] = a;
        ends_at[rows++] = t;
        next = b;
    }
    size_t after = next >= n ? n + 1 : next + 1;

    m->tn = 0;
    for (int r = 0; r < rows && fits; r++) {
        size_t off[MD_COLS + 1], len[MD_COLS + 1];
        const char* p = s + starts_at[r];
        int k = split_cells(p, ends_at[r] - starts_at[r], off, len, MD_COLS + 1);
        if (k < 0) k = MD_COLS + 1;
        for (int c = 0; c < ncols && fits; c++) {
            m->n = m->over = 0;
            if (c < k) inl(m, p + off[c], len[c], r == 0 ? S_BOLD : 0);
            fits = keep_cell(m, r, c);
        }
    }
    if (!fits) {                                      // as plain lines
        for (size_t a = i; a < n || a == i;) {
            size_t b = line_end(s, n, a);
            para(m, s + a, trim_cr(s, a, b) - a, 0, "", 0, "", 0);
            if (b >= n || b + 1 >= after) break;
            a = b + 1;
        }
        return after;
    }

    int width[MD_COLS], total = 0, W = wrap_width(m);
    for (int c = 0; c < ncols; c++) {
        width[c] = 0;
        for (int r = 0; r < rows; r++) if (m->cell[r][c].cols > width[c]) width[c] = m->cell[r][c].cols;
        total += width[c] + (c ? 3 : 0);
    }
    const char* bar = m->ascii ? " | " : " \xE2\x94\x82 ";
    if (total <= W) {
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < ncols; c++) {
                if (c) { style(m, S_DIM); wz(m, bar); }
                int pad = width[c] - m->cell[r][c].cols, before = align[c] == 2 ? pad : align[c] == 1 ? pad / 2 : 0;
                spaces(m, before);
                cell_out(m, r, c);
                if (c < ncols - 1) spaces(m, pad - before);
            }
            end_line(m);
            if (r == 0) {
                style(m, S_DIM);
                for (int c = 0; c < ncols; c++) {
                    if (c) wz(m, m->ascii ? "-+-" : "\xE2\x94\x80\xE2\x94\xBC\xE2\x94\x80");
                    for (int k = 0; k < width[c]; k++) wz(m, m->ascii ? "-" : "\xE2\x94\x80");
                }
                end_line(m);
            }
        }
        return after;
    }
    // Too wide for the terminal: each row as a record, a field on each line.
    if (rows == 1) {                                  // a header and nothing under it: one line
        m->n = 0;
        for (int c = 0; c < ncols; c++) {
            const md_cell_t* h = &m->cell[0][c];
            if (c) add(m, bar, strlen(bar), S_DIM);
            if (m->n + h->len > MD_LINE) break;
            memcpy(m->b + m->n, m->tb + h->off, (size_t)h->len);
            memcpy(m->st + m->n, m->tst + h->off, (size_t)h->len);
            m->n += h->len;
        }
        flow(m, "", 0, "", 0);
        return after;
    }
    for (int r = 1; r < rows; r++) {
        if (r > 1) w(m, "\n", 1);
        for (int c = 0; c < ncols; c++) {
            const md_cell_t* h = &m->cell[0][c];
            const md_cell_t* x = &m->cell[r][c];
            m->n = 0;
            memcpy(m->b, m->tb + h->off, (size_t)h->len);
            memcpy(m->st, m->tst + h->off, (size_t)h->len);
            m->n = h->len;
            add(m, ": ", 2, 0);
            if (m->n + x->len <= MD_LINE) {
                memcpy(m->b + m->n, m->tb + x->off, (size_t)x->len);
                memcpy(m->st + m->n, m->tst + x->off, (size_t)x->len);
                m->n += x->len;
            }
            flow(m, "", 0, "  ", 2);
        }
    }
    return after;
}

// ---- a reply ----

void md_init(md_t* m, md_write_fn write, void* ctx, int width, int ascii) {
    memset(m, 0, sizeof *m);
    m->write = write;
    m->ctx = ctx;
    m->width = width;
    m->ascii = ascii;
}

void md_render(md_t* m, const char* s, size_t n) {
    size_t fence = 0;
    char fch = 0;
    m->cur = 0;
    for (size_t i = 0;;) {
        size_t e = line_end(s, n, i), t = trim_cr(s, i, e);
        const char* L = s + i;
        size_t ln = t - i, ind = indent_of(L, ln);
        const char* p = L + ind;
        size_t pn = ln - ind;
        char ch = 0;
        size_t fl;
        if (fence) {                                  // inside a code block: as it is
            fl = ind <= 3 ? fence_len(p, pn, &ch) : 0;
            if (fl >= fence && ch == fch && is_blank(p + fl, pn - fl)) {
                style(m, S_DIM);
                md_safe(m->write, m->ctx, L, ln);
                end_line(m);
                fence = 0;
            } else plain_line(m, L, ln);
        } else if (ind <= 3 && (fl = fence_len(p, pn, &ch)) != 0) {
            style(m, S_DIM);
            md_safe(m->write, m->ctx, L, ln);
            end_line(m);
            fence = fl;
            fch = ch;
        } else if (is_blank(L, ln)) {
            w(m, "\n", 1);
        } else if (ind <= 3 && p[0] == '#' && run_of(p, pn, 0, '#') <= 6 &&
                   (run_of(p, pn, 0, '#') == pn || is_space(p[run_of(p, pn, 0, '#')]))) {
            size_t h = run_of(p, pn, 0, '#'), a = h, b = pn;
            while (a < b && is_space(p[a])) a++;
            while (b > a && is_space(p[b - 1])) b--;
            size_t c = b;                             // a closing run of #s
            while (c > a && p[c - 1] == '#') c--;
            if (c < b && (c == a || is_space(p[c - 1]))) { b = c; while (b > a && is_space(p[b - 1])) b--; }
            para(m, p + a, b - a, h == 1 ? S_BOLD | S_ULINE : S_BOLD, "", 0, "", 0);
        } else if (ind <= 3 && is_rule(p, pn)) {
            rule(m);
        } else if (ind <= 3 && p[0] == '>') {
            int levels = 0;
            size_t a = 0;
            while (a < pn && p[a] == '>') {
                levels++;
                a++;
                if (a < pn && p[a] == ' ') a++;
                while (a < pn && p[a] == ' ' && a + 1 < pn && p[a + 1] == '>') a++;
            }
            if (levels > 8) levels = 8;
            char pre[96];
            size_t k = 0;
            memcpy(pre, "\033[2m", 4); k = 4;
            for (int l = 0; l < levels; l++) {
                const char* g = m->ascii ? "| " : "\xE2\x94\x82 ";
                memcpy(pre + k, g, strlen(g));
                k += strlen(g);
            }
            memcpy(pre + k, "\033[0m", 5);
            para(m, p + a, pn - a, 0, pre, levels * 2, pre, levels * 2);
        } else {
            int align[MD_COLS];
            size_t ne = e < n ? line_end(s, n, e + 1) : n;
            int nc = memchr(L, '|', ln) && e < n ? delim_row(s + e + 1, trim_cr(s, e + 1, ne) - (e + 1), align) : 0;
            size_t hoff[MD_COLS + 1], hlen[MD_COLS + 1];
            if (nc && split_cells(L, ln, hoff, hlen, MD_COLS + 1) == nc) {
                i = table(m, s, n, i, e, nc, align);
                if (i > n) break;
                continue;
            }
            char pre[48];
            size_t dl = 0;
            while (dl < pn && dl < 9 && p[dl] >= '0' && p[dl] <= '9') dl++;
            int in = ind > 16 ? 16 : (int)ind;
            if (pn >= 2 && (p[0] == '-' || p[0] == '*' || p[0] == '+') && is_space(p[1])) {
                memset(pre, ' ', (size_t)in);
                const char* bullet = m->ascii ? "- " : "\xE2\x80\xA2 ";
                memcpy(pre + in, bullet, strlen(bullet) + 1);
                char rest[24];
                memset(rest, ' ', (size_t)in + 2);
                rest[in + 2] = 0;
                para(m, p + 2, pn - 2, 0, pre, in + 2, rest, in + 2);
            } else if (dl && dl + 1 < pn && (p[dl] == '.' || p[dl] == ')') && is_space(p[dl + 1])) {
                memset(pre, ' ', (size_t)in);
                memcpy(pre + in, p, dl + 1);
                pre[in + (int)dl + 1] = ' ';
                pre[in + (int)dl + 2] = 0;
                char rest[32];
                int rc = in + (int)dl + 2;
                memset(rest, ' ', (size_t)rc);
                rest[rc] = 0;
                para(m, p + dl + 2, pn - dl - 2, 0, pre, rc, rest, rc);
            } else {
                memset(pre, ' ', (size_t)in);
                pre[in] = 0;
                para(m, p, pn, 0, pre, in, pre, in);
            }
        }
        if (e >= n) break;
        i = e + 1;
    }
    style(m, 0);
}
