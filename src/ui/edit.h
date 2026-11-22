// The line editor for barb's prompt: the line being typed, the cursor in it, the history,
// and the terminal output that keeps the screen showing them.
//
// It does no I/O and calls nothing beyond the string functions, so it is as portable as the
// core: the platform turns key presses into ed_key calls and writes `out` to the terminal after
// each call. Windows and POSIX both use it (through screen.c), with the terminal in raw mode (on
// POSIX, keys.c turns the terminal's bytes into keys); RingTail keeps its own small editor.
//
// The screen model: the editor draws a block of rows at the bottom of what is on the screen.
// From the top: `nabove` rows of decoration (screen.c puts the activity line and a rule there),
// the prompt and the line, which may wrap onto the rows below, and an optional footer row.
// Decoration rows are cut to fit one row, so they never wrap. `row` is the row the terminal's
// cursor is on, counted from the top of the block, so the editor can always get back to the
// top to redraw or erase what it drew.
#ifndef BARB_EDIT_H
#define BARB_EDIT_H

#include <stddef.h>
#include <stdint.h>

#define ED_MAX 8192            // bytes in a line
#define ED_HIST 65536          // bytes of history, oldest dropped first
#define ED_ABOVE 2             // decoration rows above the prompt, at most
#define ED_DECOR 512           // bytes in a decoration row, escapes included
#define ED_PROMPT 64           // bytes in the prompt, escapes included

enum {
    EK_CHAR,                   // insert ch (a Unicode code point)
    EK_BACK, EK_DEL,           // the character before / under the cursor
    EK_LEFT, EK_RIGHT, EK_HOME, EK_END,
    EK_WORD_LEFT, EK_WORD_RIGHT,
    EK_UP, EK_DOWN,            // history
    EK_KILL_START,             // Ctrl-U: everything before the cursor
    EK_KILL_END,               // Ctrl-K: everything from the cursor on
    EK_KILL_WORD,              // Ctrl-W: the word before the cursor
    EK_CLEAR                   // Escape: the whole line
};

typedef struct {
    char line[ED_MAX];         // UTF-8, NUL-terminated
    int n, cur;                // its length and the cursor, in bytes
    int width;                 // the terminal's columns
    int shown;                 // the block is on the screen
    int row;                   // the cursor's row below the block's first row
    char hist[ED_HIST];        // past lines, each followed by a NUL, oldest first
    int hn;
    int back;                  // how many lines back in the history the line is (0: the one being typed)
    char draft[ED_MAX];        // the line being typed, kept while the history is shown
    int draftn;
    char prompt[ED_PROMPT];    // "> " unless set; it may carry colour escapes
    int prompt_cols;
    int nabove;                // decoration rows above the prompt (0 to ED_ABOVE)
    int has_below;             // a footer row below the line
    char above[ED_ABOVE][ED_DECOR];
    char below[ED_DECOR];
    int dirty;                 // what changed since it was drawn: 1 the rows above, 2 the prompt or footer
    int above_cols[ED_ABOVE];  // the columns each decoration row took when drawn
    char out[ED_MAX * 2 + 4096];  // what to write to the terminal after the call
    int outn;
} ed_t;

void ed_init(ed_t* e, int width);
void ed_set_width(ed_t* e, int width);   // while hidden; see ed_resize

// Each of these replaces `out` with what the terminal needs. Editing while the block is hidden
// changes it silently; ed_show then draws it as it is.
void ed_key(ed_t* e, int key, uint32_t ch);
void ed_show(ed_t* e);         // draw the block if it is not on the screen
void ed_hide(ed_t* e);         // erase it, leaving the cursor where its first row was

// The decoration. Setting a row or the prompt only records it; ed_refresh brings the screen up to
// date, redrawing as little as it can (the rows above alone are rewritten in place).
void ed_frame(ed_t* e, int nabove, int below);   // how many rows of it; redraws if shown
void ed_set_above(ed_t* e, int i, const char* s);
void ed_set_below(ed_t* e, const char* s);
void ed_set_prompt(ed_t* e, const char* s, int cols);
void ed_refresh(ed_t* e);

// The terminal changed width. If the block is shown it is erased first, on the assumption that
// the terminal has reflowed it to the new width (most do), and left hidden.
void ed_resize(ed_t* e, int width);

// Enter: the line, NUL-terminated (valid until the next call). If shown, the block gives way to
// the prompt and the line alone, which stay on the screen as a record, and the cursor moves to
// the row after them. The line goes into the history, and the editor is empty and hidden.
const char* ed_take(ed_t* e, int* n);

int ed_cols(uint32_t ch);      // the columns a code point takes on a terminal: 0, 1 or 2

// The columns `s` takes on a terminal, escape sequences skipped (they take none).
int ed_text_cols(const char* s, size_t n);

#endif
