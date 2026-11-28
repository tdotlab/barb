// The keys a terminal sends, read from the bytes it sends them as.
//
// A terminal in raw mode reports a key press as bytes: a character as its UTF-8, an arrow as an
// escape sequence (ESC [ D), Ctrl-A as byte 1. The decoder turns them back into the editor's keys
// (edit.h) and the few that the prompt handles itself: Enter, Ctrl-D, Ctrl-C, and the start and
// end of a bracketed paste (ESC [ 200 ~ ... ESC [ 201 ~), which a terminal sends around pasted text
// once it is asked to.
//
// Like the editor, it does no I/O. Feed it bytes as they arrive. A lone ESC is ambiguous (the
// Escape key, or the start of a sequence whose rest has not arrived yet), so when kd_pending says
// a key is part-way, wait a moment for more bytes and then call kd_timeout.
#ifndef BARB_KEYS_H
#define BARB_KEYS_H

#include <stdint.h>

enum {
    KD_EDIT,                   // an editor key: `ed` (EK_*), with `ch` for EK_CHAR
    KD_ENTER,                  // CR, LF or CR LF
    KD_EOF,                    // Ctrl-D
    KD_INTR,                   // Ctrl-C (only seen when the terminal does not make it a signal)
    KD_PASTE_BEGIN,
    KD_PASTE_END
};

typedef struct {
    int kind;
    int ed;
    uint32_t ch;
} kd_key_t;

typedef struct {
    unsigned char seq[32];     // an escape sequence so far, from its ESC
    int seqn;
    uint32_t cp;               // a UTF-8 character so far
    int need;                  // its continuation bytes still to come
    uint32_t least;            // the smallest value its length may carry (less is an overlong form)
    int pasting;               // between the brackets of a paste: everything is text
    int after_cr;              // the last byte was CR, so an LF right after it is the same Enter
} kd_t;

void kd_init(kd_t* d);

// One byte in; 1 if it finished a key, which is then in *k.
int kd_feed(kd_t* d, unsigned char b, kd_key_t* k);

// A key is part-way through (an escape sequence or a UTF-8 character).
int kd_pending(const kd_t* d);

// No more bytes came: what is part-way is given up, except a lone ESC, which was the Escape key.
// 1 if that makes a key.
int kd_timeout(kd_t* d, kd_key_t* k);

#endif
