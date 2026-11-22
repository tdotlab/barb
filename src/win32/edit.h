// The line editor for barb's prompt: the line being typed, the cursor in it, the history,
// and the terminal output that keeps the screen showing them.
//
// It does no I/O and calls nothing beyond the string functions, so it is as portable as the
// core: the platform turns key presses into ed_key calls and writes `out` to the terminal after
// each call. Windows uses it today because its console has no line discipline worth keeping;
// the other platforms can adopt it when the interface grows.
//
// The screen model: the prompt and the line start at the left of a row and may wrap onto the
// rows below. `row` is the row the terminal's cursor is on, counted from the prompt's row, so
// the editor can always get back to the start to redraw or erase what it drew.
#ifndef BARB_EDIT_H
#define BARB_EDIT_H

#include <stddef.h>
#include <stdint.h>

#define ED_MAX 8192            // bytes in a line
#define ED_HIST 65536          // bytes of history, oldest dropped first

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
    int shown;                 // the prompt and line are on the screen
    int row;                   // the cursor's row below the prompt's row
    char hist[ED_HIST];        // past lines, each followed by a NUL, oldest first
    int hn;
    int back;                  // how many lines back in the history the line is (0: the one being typed)
    char draft[ED_MAX];        // the line being typed, kept while the history is shown
    int draftn;
    char out[ED_MAX * 2 + 256];   // what to write to the terminal after the call
    int outn;
} ed_t;

void ed_init(ed_t* e, int width);
void ed_set_width(ed_t* e, int width);

// Each of these replaces `out` with what the terminal needs. Editing while the line is hidden
// changes it silently; ed_show then draws it as it is.
void ed_key(ed_t* e, int key, uint32_t ch);
void ed_show(ed_t* e);         // draw the prompt and line if they are not on the screen
void ed_hide(ed_t* e);         // erase them, leaving the cursor where the prompt was

// Enter: the line, NUL-terminated (valid until the next call). The screen moves to a new row,
// the line goes into the history, and the editor is empty and hidden.
const char* ed_take(ed_t* e, int* n);

int ed_cols(uint32_t ch);      // the columns a code point takes on a terminal: 0, 1 or 2

#endif
