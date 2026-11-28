// What barb shows on a terminal: the record of the session (replies, tool lines, questions) and,
// at the bottom, a block that stays there while the session runs: a line saying what is going on
// (waiting for the model, running a command, how long it has taken), a rule, the prompt, and a
// footer with the model, the directory and the context's size.
//
//     ● bash ls src
//       └ exit 0, 3 lines
//
//     Three files: core, ui and posix.
//
//     ⠹ waiting for the model · 4s
//     ────────────────────────────────────────────
//     > _
//       claude-sonnet · ~/src/barb · 12.4k tokens       Ctrl-C stops it
//
// Like edit.c and md.c it does no I/O: what to show goes to a write callback, and the platform
// tells it what happens (the agent's events and actions, the keys, the time). Both platforms use
// it, so they look the same.
//
// Three modes. REGION is an interactive session on a terminal: the block above. LITE is a
// terminal that is not ours to edit (barb -p on a terminal, or a terminal in line mode): the
// record is styled the same way and a one-line status ("⠹ waiting for the model · 4s") shows while
// the agent works. PIPE is everything else: plain text, as barb has always written it, so scripts
// and logs see no escapes. Replies are drawn as markdown whenever standard output is a terminal.
#ifndef BARB_SCREEN_H
#define BARB_SCREEN_H

#include <stddef.h>
#include <stdint.h>
#include "../core/agent.h"
#include "edit.h"
#include "md.h"

enum { SC_PIPE, SC_LITE, SC_REGION };

typedef void (*sc_write_fn)(void* ctx, int fd, const char* s, size_t n);   // fd 1 or 2
typedef void (*sc_clock_fn)(void* ctx, int64_t at_ms, char* hhmm);         // "14:32" for a time in now's units

typedef struct {
    int mode;                  // SC_*
    int vt_out, vt_err;        // standard output / error is a terminal that takes escape sequences
    int fancy;                 // the font has braille, for the spinner
    int ascii;                 // no box-drawing characters or bullets (the Linux console)
    int width;                 // the terminal's columns
    const char* model;
    const char* cwd;
    const char* home;          // shown as ~ at the start of cwd; may be 0
    sc_write_fn write;
    sc_clock_fn clock;         // may be 0: times are then not shown
    void* ctx;
} sc_cfg_t;

#define SC_OPS 32              // background tasks and timers it keeps track of

typedef struct {
    sc_cfg_t cfg;
    char model[96], cwd[256];  // cleaned up for showing
    ed_t ed;
    md_t md;
    int64_t http_id, http_since;               // the request out, and when it went
    int64_t fg_id, fg_since;                   // the command running in the foreground
    char fg_cmd[160];          // its first line, cleaned
    int fg_more;               // it had more than that
    int64_t retry_id, retry_due;               // the timer before a failed request is retried
    int64_t task[SC_OPS];                      // background tasks
    int ntasks;
    int64_t timer[SC_OPS], timer_due[SC_OPS];  // the model's wakeups
    int ntimers;
    int64_t tokens;            // the size of the last request, from SAY_USAGE
    int http_failed;           // the event being handled is a request that failed in a way that is retried
    int last;                  // what was shown last, for the blank lines between things
    int midline;               // a question waits on the line for its answer
    int status_shown;          // LITE: the status line is on the screen
    char status[ED_DECOR];
    int animating;             // the activity line has a spinner
} sc_t;

void sc_init(sc_t* s, const sc_cfg_t* cfg);

// Tell it what happens: every event just before the agent sees it, every action just before it
// is carried out (ACT_SAY is shown here; the rest only noted). `now` is in milliseconds.
void sc_event(sc_t* s, const ev_t* ev);
void sc_action(sc_t* s, const act_t* a, int64_t now);

void sc_say(sc_t* s, int style, const char* p, size_t n);   // SAY_*
void sc_banner(sc_t* s, const char* version);
void sc_question(sc_t* s, const char* tool, size_t tn, const char* desc, size_t dn);
void sc_answer(sc_t* s, int ans);                            // ANS_*: shown after the question
void sc_raw(sc_t* s, int fd, const char* text);              // written as it is

// Take the block and the status line off the screen (and end a question's line), leaving the
// cursor at the start of a free row: before exiting or stopping, and when a question goes.
void sc_hide(sc_t* s);

// Bring the bottom of the screen up to date. busy: the agent is working. prompt_ok: the user
// can type (REGION: the block is shown). status_ok: a status line may be shown when the block
// is not. Cheap when nothing changed.
void sc_draw(sc_t* s, int64_t now, int busy, int prompt_ok, int status_ok);

// Milliseconds until sc_draw has something new to show (the spinner), or -1.
int sc_next_ms(const sc_t* s, int64_t now);

// The line editor: a key (EK_*), Enter (the line, valid until the next call), a new width.
void sc_key(sc_t* s, int key, uint32_t ch);
const char* sc_take(sc_t* s, int* n);
void sc_resize(sc_t* s, int width);

#endif
