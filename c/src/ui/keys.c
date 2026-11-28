// The terminal's key decoder: see keys.h.
#include "keys.h"
#include "edit.h"
#include <string.h>

void kd_init(kd_t* d) { memset(d, 0, sizeof *d); }

static int key(kd_key_t* k, int kind, int ed, uint32_t ch) {
    k->kind = kind;
    k->ed = ed;
    k->ch = ch;
    return 1;
}
static int edit(kd_key_t* k, int ed) { return key(k, KD_EDIT, ed, 0); }

// The first two numbers of a sequence's parameters ("1;5" is 1 and 5); 0 where there is none.
static void params(const unsigned char* s, int n, int* p1, int* p2) {
    int v[2] = { 0, 0 }, at = 0;
    for (int i = 0; i < n && at < 2; i++) {
        if (s[i] >= '0' && s[i] <= '9') { if (v[at] < 100000) v[at] = v[at] * 10 + (s[i] - '0'); }
        else if (s[i] == ';') at++;
    }
    *p1 = v[0];
    *p2 = v[1];
}

// A finished escape sequence: the key it means, or 0 for one the prompt has no use for.
static int sequence(kd_t* d, kd_key_t* k) {
    const unsigned char* s = d->seq;
    int n = d->seqn;
    d->seqn = 0;
    if (d->pasting) {          // pasted text is text: only the end of the paste counts
        if (n == 6 && !memcmp(s, "\033[201~", 6)) { d->pasting = 0; return key(k, KD_PASTE_END, 0, 0); }
        return 0;
    }
    if (s[1] == '[' || s[1] == 'O') {
        int p1, p2;
        params(s + 2, n - 3, &p1, &p2);
        // xterm adds a modifier as 1 + its bits (Shift 1, Alt 2, Ctrl 4); Alt or Ctrl moves by word.
        int word = p2 > 1 && ((p2 - 1) & 6);
        switch (s[n - 1]) {
        case 'A': return edit(k, EK_UP);
        case 'B': return edit(k, EK_DOWN);
        case 'C': return edit(k, word ? EK_WORD_RIGHT : EK_RIGHT);
        case 'D': return edit(k, word ? EK_WORD_LEFT : EK_LEFT);
        case 'H': return edit(k, EK_HOME);
        case 'F': return edit(k, EK_END);
        case '~':
            if (s[1] != '[') return 0;
            switch (p1) {
            case 1: case 7: return edit(k, EK_HOME);
            case 4: case 8: return edit(k, EK_END);
            case 3: return edit(k, EK_DEL);
            case 200: d->pasting = 1; return key(k, KD_PASTE_BEGIN, 0, 0);
            }
        }
        return 0;
    }
    switch (s[1]) {            // ESC and one byte: that key with Alt (Meta)
    case 'b': case 'B': return edit(k, EK_WORD_LEFT);
    case 'f': case 'F': return edit(k, EK_WORD_RIGHT);
    case 0x7F: case 0x08: return edit(k, EK_KILL_WORD);
    }
    return 0;
}

// The next byte of an escape sequence: 1 if it finished one that means a key. A control byte
// breaks a sequence off; it is then read as itself (`*again`).
static int in_sequence(kd_t* d, unsigned char b, kd_key_t* k, int* again) {
    if (b == 0x1B) {           // ESC ESC: the first was the Escape key, the second starts afresh
        if (d->seqn == 1 && !d->pasting) return edit(k, EK_CLEAR);
        d->seqn = 1;
        return 0;
    }
    if (d->seqn == 1 && (b == 0x7F || b == 0x08)) { d->seq[d->seqn++] = b; return sequence(d, k); }
    if (b < 0x20 || b >= 0x7F) { d->seqn = 0; *again = 1; return 0; }
    if (d->seqn == (int)sizeof d->seq) {   // too long to be a real one: read on to its end, and drop it
        d->seq[1] = 0;                     // (a sequence that means nothing)
        d->seqn--;
    }
    d->seq[d->seqn++] = b;
    if (d->seqn == 2) return b == '[' || b == 'O' ? 0 : sequence(d, k);
    if (d->seq[1] == 'O' || (b >= 0x40 && b <= 0x7E)) return sequence(d, k);   // the final byte
    return 0;
}

int kd_feed(kd_t* d, unsigned char b, kd_key_t* k) {
    int after_cr = d->after_cr;
    d->after_cr = 0;
    if (d->seqn) {
        int again = 0;
        int got = in_sequence(d, b, k, &again);
        if (!again) return got;
    }
    if (d->need) {
        if ((b & 0xC0) == 0x80) {
            d->cp = (d->cp << 6) | (b & 0x3F);
            if (--d->need) return 0;
            uint32_t c = d->cp;    // only a real character: no overlong form, surrogate or control
            if (c < d->least || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF) || c < 0xA0) return 0;
            return key(k, KD_EDIT, EK_CHAR, c);
        }
        d->need = 0;           // a broken character is dropped, and this byte read afresh
    }
    if (b >= 0xC0 && b < 0xF8) {
        d->need = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
        d->cp = b & (0x3Fu >> d->need);
        d->least = d->need == 1 ? 0x80 : d->need == 2 ? 0x800 : 0x10000;
        return 0;
    }
    if (b >= 0x80) return 0;   // a continuation byte with nothing to continue
    if (b >= 0x20 && b < 0x7F) return key(k, KD_EDIT, EK_CHAR, b);
    if (b == '\r' || b == '\n') {
        if (b == '\n' && after_cr) return 0;
        d->after_cr = b == '\r';
        return key(k, KD_ENTER, 0, 0);
    }
    if (b == 0x1B) { d->seq[0] = b; d->seqn = 1; return 0; }
    if (b == '\t') return key(k, KD_EDIT, EK_CHAR, '\t');
    if (d->pasting) return 0;  // a control character in pasted text is not a command
    switch (b) {
    case 1: return edit(k, EK_HOME);           // Ctrl-A
    case 2: return edit(k, EK_LEFT);           // Ctrl-B
    case 3: return key(k, KD_INTR, 0, 0);      // Ctrl-C
    case 4: return key(k, KD_EOF, 0, 0);       // Ctrl-D
    case 5: return edit(k, EK_END);            // Ctrl-E
    case 6: return edit(k, EK_RIGHT);          // Ctrl-F
    case 8: case 0x7F: return edit(k, EK_BACK);   // Backspace sends one or the other
    case 11: return edit(k, EK_KILL_END);      // Ctrl-K
    case 14: return edit(k, EK_DOWN);          // Ctrl-N
    case 16: return edit(k, EK_UP);            // Ctrl-P
    case 21: return edit(k, EK_KILL_START);    // Ctrl-U
    case 23: return edit(k, EK_KILL_WORD);     // Ctrl-W
    }
    return 0;
}

int kd_pending(const kd_t* d) { return d->seqn || d->need; }

int kd_timeout(kd_t* d, kd_key_t* k) {
    int lone_esc = d->seqn == 1 && !d->pasting;
    d->seqn = 0;
    d->need = 0;
    return lone_esc ? edit(k, EK_CLEAR) : 0;
}
