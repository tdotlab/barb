// Markdown for the terminal: the model's replies, drawn with bold, italics, code, lists, quotes and
// tables instead of the asterisks and pipes they were written with.
//
// Like edit.c it does no I/O and calls nothing beyond the string functions: it hands what to show
// to a write callback. It works a line at a time, the way models write (a paragraph is one line),
// and wraps each line at the width with a hanging indent for list items and quotes. A fenced code
// block is shown as it is, fences and all, so it can be copied. Every control character in the
// text is made visible (ESC shows as "^["), so a reply cannot move the cursor or set the title.
#ifndef BARB_MD_H
#define BARB_MD_H

#include <stddef.h>

#define MD_LINE 8192           // bytes of one line, once styled; a longer line is shown plain
#define MD_TABLE 16384         // bytes of a table's cells
#define MD_ROWS 64             // a table's rows, the header included
#define MD_COLS 12             // and columns

typedef void (*md_write_fn)(void* ctx, const char* s, size_t n);

typedef struct { int off, len, cols; } md_cell_t;

typedef struct {
    md_write_fn write;
    void* ctx;
    int width;                 // the terminal's columns
    int ascii;                 // no box-drawing characters or bullets (the Linux console)
    // Scratch: the line being drawn, a byte of style for each of its bytes, and a table's cells.
    char b[MD_LINE];
    unsigned char st[MD_LINE];
    int n, over;
    char tb[MD_TABLE];
    unsigned char tst[MD_TABLE];
    int tn;
    md_cell_t cell[MD_ROWS][MD_COLS];
    int cur;                   // the style the terminal is in
} md_t;

void md_init(md_t* m, md_write_fn write, void* ctx, int width, int ascii);

// Draw one reply: whole lines, each ended with a newline (so "" draws one empty line).
void md_render(md_t* m, const char* s, size_t n);

// Write s with its control characters made visible: C0 as "^X", DEL as "^?", C1 as U+FFFD.
// Newlines and tabs pass, and a CR before a newline is dropped.
void md_safe(md_write_fn write, void* ctx, const char* s, size_t n);

#endif
