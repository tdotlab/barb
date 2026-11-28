// A small terminal for the UI tests: it plays the bytes the editor and the screen write and
// keeps what each cell shows and where the cursor is. It knows the few sequences they use (cursor
// up, down and right, erase to the end of the line or the screen, colours) and counts anything
// else as bad, along with a cursor sent off the screen. Both ways terminals treat a row filled to
// the last column are there: moving to the next row at once, or waiting for the next character.
// A test may set TR (rows) and TC (columns) before including it.
#ifndef BARB_TERM_H
#define BARB_TERM_H

#include <stdint.h>
#include <string.h>
#include "../src/ui/edit.h"

#ifndef TR
#define TR 160
#endif
#ifndef TC
#define TC 40
#endif
#define CELL 24

typedef struct {
    char cell[TR][TC][CELL];   // the UTF-8 in each cell: "" is empty, "\x01" the right half of a wide character
    int r, c;                  // the cursor
    int pend;                  // waiting to wrap with the next character (the deferred kind)
    int width, deferred;
    int lr, lc;                // the cell last written, where a combining mark goes
    int bad;                   // sequences this terminal does not know, or a cursor sent off the screen
    int scroll;                // a new line at the bottom scrolls the screen up (otherwise it is bad)
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

// Below the last row: scroll up, if this terminal does.
static void t_bottom(term_t* t) {
    while (t->scroll && t->r >= TR) {
        memmove(t->cell[0], t->cell[1], sizeof t->cell[0] * (TR - 1));
        memset(t->cell[TR - 1], 0, sizeof t->cell[0]);
        t->r--;
        t->lr--;
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
    t_bottom(t);
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
            int j = i + 2, num = 0, has = 0, semi = 0;
            while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) {
                if (s[j] == ';') semi = 1;
                else { num = num * 10 + (s[j] - '0'); has = 1; }
                j++;
            }
            if (j >= n) { t->bad++; return; }
            char f = s[j];
            i = j + 1;
            if (semi && f != 'm') { t->bad++; continue; }
            switch (f) {
            case 'A': t->r -= has ? num : 1; if (t->r < 0) { t->bad++; t->r = 0; } t->pend = 0; break;
            case 'B': t->r += has ? num : 1; if (t->r >= TR) { t->bad++; t->r = TR - 1; } t->pend = 0; break;
            case 'C': t->c += has ? num : 1; if (t->c > t->width - 1) t->c = t->width - 1; t->pend = 0; break;
            case 'J': if (num) t->bad++; else t_erase(t, t->r, t->c); break;
            case 'K': if (num) t->bad++; else for (int c = t->c; c < TC; c++) t->cell[t->r][c][0] = 0; break;
            case 'm': break;           // colours: the screen's text is what is checked
            default: t->bad++;
            }
            continue;
        }
        if (ch == '\r') { t->c = 0; t->pend = 0; i++; continue; }
        if (ch == '\t') { t->c = (t->c / 8 + 1) * 8; if (t->c > t->width - 1) t->c = t->width - 1; t->pend = 0; i++; continue; }
        if (ch == '\n') { t->r++; t->pend = 0; t_bottom(t); i++; continue; }
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


#endif
