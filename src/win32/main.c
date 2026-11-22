// barb for Windows: the command line, the console, and the loop that joins the agent to the system.
//
//   barb                       an interactive session (type a task; /help for commands)
//   barb -p "task"             do one task and exit; the answer goes to standard output
//
// The same program as src/posix/main.c, on Win32. The loop sleeps in WaitForMultipleObjects on
// the console, the operations' pipes and processes, and an event the Ctrl-C handler sets, with
// a timeout only when a timer or a time limit is due: an agent waiting an hour for a timer or a
// background task uses no CPU in the meantime.
//
// The console is put in raw mode for an interactive session: keys arrive one at a time and go
// to the line editor (edit.c), which draws the prompt with VT sequences. Text goes to the
// console as UTF-16 (WriteConsoleW), so it shows right whatever the console's code page; to a
// pipe or a file it goes as the UTF-8 the agent made.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../core/agent.h"
#include "../ui/edit.h"
#include "ops.h"

#define VERSION "0.2.0"
#define DEFAULT_MODEL "claude-sonnet-5-5"

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

// ---- events waiting for the agent ----

typedef struct qev {
    struct qev* next;
    ev_t ev;
    char* data;
    char* note;
} qev_t;

static qev_t *qhead, *qtail;

static char* dup_n(const char* p, size_t n) {
    char* s = malloc(n + 1);
    if (!s) return 0;
    memcpy(s, p, n);
    s[n] = 0;
    return s;
}

static void deliver(const ev_t* ev) {
    qev_t* q = calloc(1, sizeof *q);
    if (!q) return;
    q->ev = *ev;
    if (ev->data.p) { q->data = dup_n(ev->data.p, ev->data.n); q->ev.data = str_of(q->data, q->data ? ev->data.n : 0); }
    if (ev->note.p) { q->note = dup_n(ev->note.p, ev->note.n); q->ev.note = str_of(q->note, q->note ? ev->note.n : 0); }
    if (qtail) qtail->next = q; else qhead = q;
    qtail = q;
}

static void deliver_simple(evk_t kind, int64_t id, int status, const char* data) {
    ev_t e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.id = id;
    e.status = status;
    if (data) e.data = str_z(data);
    deliver(&e);
}

// ---- state ----

static void* m_alloc(size_t n) { return malloc(n); }
static void m_release(void* p) { free(p); }
static const mem_t mem = { m_alloc, m_release };

static agent_t* agent;
static int done, exit_code;
static int64_t awaiting;        // the question the user is being asked, if any
static int interactive;         // the console is ours: raw keys, the line editor
static int tty_out;             // standard error is a console that understands VT sequences
static int oneshot;             // -p
static int stdin_open = 1;

static HANDLE h_in, h_out, h_err;
static int con_in, con_out, con_err;            // which of them are consoles
static DWORD mode_in, mode_out, mode_err;       // their modes before ours
static int restore_in, restore_out, restore_err;

static HANDLE wake;                             // set by the Ctrl-C handler
static HANDLE cleaned;                          // set when barb has tidied up, for the close handler
static volatile LONG interrupts, terminate_now;

static ed_t ed;
static int64_t http_id, http_since;
static int spin_shown;
static int64_t spin_secs;

// ---- output ----

static void write_handle(HANDLE h, int con, const char* s, size_t n) {
    if (!n) return;
    if (!con) {
        while (n) {
            DWORD chunk = n > (1u << 20) ? (1u << 20) : (DWORD)n, w = 0;
            if (!WriteFile(h, s, chunk, &w, 0) || !w) return;
            s += w;
            n -= w;
        }
        return;
    }
    wchar_t* w = ops_wide(s, n);
    if (!w) return;
    size_t len = wcslen(w);   // UTF-8 with a NUL in it stops there, which is fine for a console
    for (size_t at = 0; at < len;) {
        size_t chunk = len - at > 8192 ? 8192 : len - at;
        if (at + chunk < len && w[at + chunk - 1] >= 0xD800 && w[at + chunk - 1] <= 0xDBFF) chunk--;   // keep pairs whole
        DWORD wrote = 0;
        if (!WriteConsoleW(h, w + at, (DWORD)chunk, &wrote, 0) || !wrote) break;
        at += wrote;
    }
    free(w);
}

static void out(const char* s, size_t n) { write_handle(h_out, con_out, s, n); }
static void err(const char* s, size_t n) { write_handle(h_err, con_err, s, n); }
static void errz(const char* s) { err(s, strlen(s)); }

static void ed_flush(void) {
    if (ed.outn) err(ed.out, (size_t)ed.outn);
    ed.outn = 0;
}

// Take the typed line or the waiting line off the screen, so output can go where it was.
static void clear_line(void) {
    if (ed.shown) { ed_hide(&ed); ed_flush(); }
    if (spin_shown) { errz("\r\033[K"); spin_shown = 0; }
}

static void ed_draw(void) {
    if (!interactive || awaiting || !stdin_open || done) return;
    if (agent_busy(agent) && ed.n == 0) return;   // no prompt while it works, unless you are typing
    if (ed.shown) return;
    if (spin_shown) clear_line();
    ed_show(&ed);
    ed_flush();
}

static void spin_draw(void) {
    if (!http_id || !tty_out || awaiting || ed.shown) return;
    int64_t secs = (ops_now_ms() - http_since) / 1000;
    if (secs < 2 || (spin_shown && secs == spin_secs)) return;
    char b[96];
    snprintf(b, sizeof b, "\r\033[K\033[2m  waiting for the model, %llds (Ctrl-C stops it)\033[0m", (long long)secs);
    errz(b);
    spin_shown = 1;
    spin_secs = secs;
}

static const char* color(int style) {
    if (!tty_out) return "";
    switch (style) {
    case SAY_TOOL: return "\033[36m";
    case SAY_INFO: return "\033[2m";
    case SAY_ERROR: return "\033[31m";
    }
    return "";
}

static void show(int style, str_t s) {
    clear_line();
    if (style == SAY_ASSISTANT) {
        out(s.p, s.n);
        out("\n", 1);
        return;
    }
    const char* on = color(style);
    errz(on);
    if (style == SAY_TOOL && s.n && s.p[0] != ' ') errz("> ");   // a tool's name line
    err(s.p, s.n);
    errz(*on ? "\033[0m\n" : "\n");
}

// ---- the keyboard ----

static void answer(int ans, const char* word) {
    int64_t id = awaiting;
    awaiting = 0;
    errz(word);
    errz("\n");
    deliver_simple(EV_ANSWER, id, ans, 0);
}

// A paste arrives as keystrokes with an Enter after every line. Lines that are followed at once by
// more text belong to one message, so they are gathered here and delivered together, with the
// newlines the user pasted, when a line ends with nothing more waiting behind it.
static char* paste;
static size_t paste_n, paste_cap;

static int paste_add(const char* p, size_t n) {
    if (paste_n + n + 1 > paste_cap) {
        size_t cap = paste_cap ? paste_cap : 4096;
        while (cap < paste_n + n + 1) cap *= 2;
        char* q = realloc(paste, cap);
        if (!q) return 0;
        paste = q;
        paste_cap = cap;
    }
    memcpy(paste + paste_n, p, n);
    paste_n += n;
    paste[paste_n] = 0;
    return 1;
}

// Is there typed text already waiting behind the key being handled? `rest` is what is left of the
// batch the loop is reading; the console's own queue is looked at too. Part-way through a paste the
// next chunk may still be on its way, so it is given a moment.
static INPUT_RECORD* batch;
static DWORD batch_i, batch_n;

static int text_key(const INPUT_RECORD* r) {
    return r->EventType == KEY_EVENT && r->Event.KeyEvent.bKeyDown && r->Event.KeyEvent.uChar.UnicodeChar >= 32;
}

static int more_text_waiting(void) {
    for (DWORD i = batch_i + 1; i < batch_n; i++) if (text_key(&batch[i])) return 1;
    for (int tries = 0; tries < 2; tries++) {
        INPUT_RECORD pk[64];
        DWORD got = 0;
        if (PeekConsoleInputW(h_in, pk, 64, &got))
            for (DWORD i = 0; i < got; i++) if (text_key(&pk[i])) return 1;
        if (!paste_n || tries) break;
        WaitForSingleObject(h_in, 40);
    }
    return 0;
}

static void enter(void) {
    if (!ed.shown) { if (spin_shown) clear_line(); ed_show(&ed); ed_flush(); }
    int n;
    const char* line = ed_take(&ed, &n);
    ed_flush();
    if (more_text_waiting() && paste_add(line, (size_t)n) && paste_add("\n", 1)) return;
    if (!paste_n) { deliver_simple(EV_INPUT, 0, 0, line); return; }
    if (n) paste_add(line, (size_t)n);
    deliver_simple(EV_INPUT, 0, 0, paste);
    paste_n = 0;
}

// One key: `ch` is the character it typed (0 for none), `vk` the key, `ctrl` whether Ctrl was down.
static void key(uint32_t ch, WORD vk, int ctrl) {
    if (awaiting) {   // one key answers; the line being typed waits
        switch (ch) {
        case 'y': case 'Y': answer(ANS_YES, "yes"); break;
        case 'a': case 'A': answer(ANS_ALWAYS, "always"); break;
        case 'n': case 'N': case '\r': case '\n': case 27: case 4: answer(ANS_NO, "no"); break;
        case 3: deliver_simple(EV_INTERRUPT, 0, 0, 0); break;
        }
        return;
    }
    if (!ch) {
        switch (vk) {
        case VK_LEFT: ed_key(&ed, ctrl ? EK_WORD_LEFT : EK_LEFT, 0); break;
        case VK_RIGHT: ed_key(&ed, ctrl ? EK_WORD_RIGHT : EK_RIGHT, 0); break;
        case VK_HOME: ed_key(&ed, EK_HOME, 0); break;
        case VK_END: ed_key(&ed, EK_END, 0); break;
        case VK_DELETE: ed_key(&ed, EK_DEL, 0); break;
        case VK_UP: ed_key(&ed, EK_UP, 0); break;
        case VK_DOWN: ed_key(&ed, EK_DOWN, 0); break;
        default: return;
        }
        ed_flush();
        return;
    }
    int k = -1;
    switch (ch) {
    case 3:   // Ctrl-C: stop the work; at an idle prompt, throw away what was typed first
        if (!agent_busy(agent) && ed.n) { ed_key(&ed, EK_CLEAR, 0); ed_flush(); }
        else deliver_simple(EV_INTERRUPT, 0, 0, 0);
        return;
    case 4:   // Ctrl-D: on an empty line, the end; otherwise delete forward, as in a shell
        if (ed.n) { k = EK_DEL; break; }
        clear_line();
        errz("\n");
        stdin_open = 0;
        deliver_simple(EV_EOF, 0, 0, 0);
        return;
    case '\r': case '\n': enter(); return;
    case 8: k = EK_BACK; break;
    case 0x7F: k = EK_KILL_WORD; break;     // Ctrl-Backspace
    case 1: k = EK_HOME; break;             // Ctrl-A
    case 5: k = EK_END; break;              // Ctrl-E
    case 2: k = EK_LEFT; break;             // Ctrl-B
    case 6: k = EK_RIGHT; break;            // Ctrl-F
    case 11: k = EK_KILL_END; break;        // Ctrl-K
    case 21: k = EK_KILL_START; break;      // Ctrl-U
    case 23: k = EK_KILL_WORD; break;       // Ctrl-W
    case 16: k = EK_UP; break;              // Ctrl-P
    case 14: k = EK_DOWN; break;            // Ctrl-N
    case 27: k = EK_CLEAR; break;           // Escape
    }
    if (k >= 0) ed_key(&ed, k, 0);
    else ed_key(&ed, EK_CHAR, ch);
    ed_flush();
}

static WCHAR high_surrogate;
static uint32_t last_ch;

static void console_record(const INPUT_RECORD* r) {
    if (r->EventType == WINDOW_BUFFER_SIZE_EVENT) {
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(h_err, &info)) {
            int shown = ed.shown;
            if (shown) { ed_hide(&ed); ed_flush(); }   // best effort: the old layout is gone either way
            ed_set_width(&ed, info.dwSize.X);
        }
        return;
    }
    if (r->EventType != KEY_EVENT) return;
    const KEY_EVENT_RECORD* k = &r->Event.KeyEvent;
    WCHAR wc = k->uChar.UnicodeChar;
    // Alt+numpad and some input methods deliver their character on the key's release.
    if (!k->bKeyDown && !(k->wVirtualKeyCode == VK_MENU && wc)) return;
    int ctrl = (k->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    uint32_t ch = wc;
    if (wc >= 0xD800 && wc <= 0xDBFF) { high_surrogate = wc; return; }
    if (wc >= 0xDC00 && wc <= 0xDFFF) {
        if (!high_surrogate) return;
        ch = 0x10000 + (((uint32_t)high_surrogate - 0xD800) << 10) + (wc - 0xDC00);
    }
    high_surrogate = 0;
    WORD repeat = k->wRepeatCount ? k->wRepeatCount : 1;
    for (WORD i = 0; i < repeat && !done && stdin_open; i++) {
        if (ch == '\n' && last_ch == '\r') { last_ch = 0; continue; }   // CR LF is one Enter
        last_ch = ch;
        key(ch, k->wVirtualKeyCode, ctrl);
    }
}

// Whatever the console has for us, without waiting.
static void read_console(void) {
    INPUT_RECORD recs[512];
    DWORD avail = 0, got = 0;
    while (stdin_open && !done && GetNumberOfConsoleInputEvents(h_in, &avail) && avail) {
        if (!ReadConsoleInputW(h_in, recs, 512, &got) || !got) break;
        batch = recs;
        batch_n = got;
        for (DWORD i = 0; i < got && stdin_open && !done; i++) {
            batch_i = i;
            console_record(&recs[i]);
        }
        batch = 0;
        batch_n = 0;
    }
}

// Standard input that is not ours to edit (a pipe, a file, a console in line mode) is read by a
// thread, which hands what arrives to the loop.
static CRITICAL_SECTION in_lock;
static HANDLE in_ready;
static char* in_pending;
static size_t in_pending_n, in_pending_cap;
static int in_eof;

static void in_add(const char* p, size_t n) {
    EnterCriticalSection(&in_lock);
    if (in_pending_n + n > in_pending_cap) {
        size_t cap = in_pending_cap ? in_pending_cap : 65536;
        while (cap < in_pending_n + n) cap *= 2;
        char* q = realloc(in_pending, cap);
        if (q) { in_pending = q; in_pending_cap = cap; }
    }
    if (in_pending_n + n <= in_pending_cap) { memcpy(in_pending + in_pending_n, p, n); in_pending_n += n; }
    LeaveCriticalSection(&in_lock);
    SetEvent(in_ready);
}

static DWORD WINAPI reader(void* arg) {
    (void)arg;
    if (con_in) {   // a console in line mode: lines in UTF-16, Ctrl-Z at the start of one ends input
        wchar_t w[4096];
        DWORD got;
        while (ReadConsoleW(h_in, w, 4096, &got, 0) && got) {
            if (w[0] == 26) break;
            char* s = ops_utf8(w, (int)got);
            if (s) { in_add(s, strlen(s)); free(s); }
        }
    } else {
        static char b[65536];
        DWORD got;
        while (ReadFile(h_in, b, sizeof b, &got, 0) && got) in_add(b, got);
    }
    EnterCriticalSection(&in_lock);
    in_eof = 1;
    LeaveCriticalSection(&in_lock);
    SetEvent(in_ready);
    return 0;
}

static char line_buf[65536];
static size_t line_n;

static void line(char* s, size_t n) {
    while (n && s[n - 1] == '\r') n--;
    s[n] = 0;
    deliver_simple(EV_INPUT, 0, 0, s);
}

static void read_lines(void) {
    EnterCriticalSection(&in_lock);
    char* p = in_pending;
    size_t n = in_pending_n;
    int eof = in_eof;
    ResetEvent(in_ready);       // under the lock: whatever comes after this sets it again
    in_pending = 0;
    in_pending_n = in_pending_cap = 0;
    LeaveCriticalSection(&in_lock);
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') { line(line_buf, line_n); line_n = 0; continue; }
        line_buf[line_n++] = p[i];
        if (line_n == sizeof line_buf - 1) { line(line_buf, line_n); line_n = 0; }   // one enormous line
    }
    free(p);
    if (eof) {
        stdin_open = 0;
        if (line_n) { line(line_buf, line_n); line_n = 0; }
        deliver_simple(EV_EOF, 0, 0, 0);
    }
}

// ---- carrying out actions ----

static void ask(const act_t* a) {
    if (oneshot || !interactive) {
        clear_line();
        char b[200];
        snprintf(b, sizeof b, "%s(not allowed: there is no one to ask. Run with --yes to let barb write files and run commands unasked.)%s\n",
                 color(SAY_ERROR), tty_out ? "\033[0m" : "");
        errz(b);
        deliver_simple(EV_ANSWER, a->id, ANS_NO, 0);
        return;
    }
    clear_line();
    read_console();   // keys typed before the question go to the line, so they cannot answer it
    clear_line();
    errz("\n");
    errz(color(SAY_TOOL));
    err(a->a.p, a->a.n);
    errz(" wants to:");
    errz(tty_out ? "\033[0m\n" : "\n");
    err(a->b.p, a->b.n);
    errz("\nAllow? [y]es / [n]o / [a]lways this session: ");
    awaiting = a->id;
}

static void exec(const act_t* a) {
    switch (a->kind) {
    case ACT_SAY: show(a->flag, a->a); break;
    case ACT_HTTP: ops_http(a->id, a->a, a->b); http_id = a->id; http_since = ops_now_ms(); break;
    case ACT_READ: ops_read(a->id, a->a); break;
    case ACT_WRITE: ops_write(a->id, a->a, a->b); break;
    case ACT_SPAWN: ops_spawn(a->id, a->a, a->n, a->flag, a->c); break;
    case ACT_TIMER: ops_timer(a->id, a->n); break;
    case ACT_CANCEL:
        if (a->id == http_id) { http_id = 0; clear_line(); }
        if (a->id == awaiting) { awaiting = 0; errz("\n"); }
        else ops_cancel(a->id);
        break;
    case ACT_ASK: ask(a); break;
    case ACT_DONE: done = 1; exit_code = (int)a->n; break;
    }
}

static void step(const ev_t* ev) {
    if (ev->kind == EV_HTTP && ev->id == http_id) { http_id = 0; if (spin_shown) clear_line(); }
    size_t n;
    const act_t* acts = agent_step(agent, ev, &n);
    for (size_t i = 0; i < n; i++) exec(&acts[i]);
}

static void drain_queue(void) {
    while (qhead && !done) {
        qev_t* q = qhead;
        qhead = q->next;
        if (!qhead) qtail = 0;
        step(&q->ev);
        free(q->data);
        free(q->note);
        free(q);
    }
}

// ---- Ctrl-C, Ctrl-Break and closing the window ----

static BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedIncrement(&interrupts);
        SetEvent(wake);
        return TRUE;
    }
    // The window is closing, or the user is logging off: stop, the way SIGTERM stops barb
    // elsewhere. Windows ends the process when this returns, so wait (briefly) for the main
    // thread to kill the commands and remove the temporary files first.
    InterlockedExchange(&terminate_now, 1);
    SetEvent(wake);
    WaitForSingleObject(cleaned, 4000);
    return TRUE;
}

static void take_signals(void) {
    for (LONG n = InterlockedExchange(&interrupts, 0); n > 0; n--) deliver_simple(EV_INTERRUPT, 0, 0, 0);
    if (terminate_now) { done = 1; exit_code = 143; }
}

// ---- the loop ----

static void loop(void) {
    HANDLE h[MAXIMUM_WAIT_OBJECTS];
    for (;;) {
        take_signals();
        drain_queue();
        if (done) return;
        ed_draw();
        spin_draw();
        int n = 0, slot_console = -1, slot_lines = -1;
        h[n++] = wake;
        if (stdin_open && !oneshot) {
            if (interactive) { slot_console = n; h[n++] = h_in; }
            else if (in_ready) { slot_lines = n; h[n++] = in_ready; }
        }
        n += ops_handles((void**)(h + n), MAXIMUM_WAIT_OBJECTS - n);
        int timeout = ops_timeout_ms();
        if (http_id && tty_out && (timeout < 0 || timeout > 250)) timeout = 250;   // keep the clock moving
        if (qhead) timeout = 0;
        DWORD r = WaitForMultipleObjects((DWORD)n, h, FALSE, timeout < 0 ? INFINITE : (DWORD)timeout);
        (void)r;
        // Whatever woke the loop, look at everything that might be ready; each check is cheap.
        if (slot_console >= 0) read_console();
        if (slot_lines >= 0 && WaitForSingleObject(in_ready, 0) == WAIT_OBJECT_0) read_lines();
        ops_service();
    }
}

// ---- starting up ----

static void usage(int to_err) {
    char b[1400];
    snprintf(b, sizeof b,
        "barb %s: a coding agent for the terminal\n\n"
        "usage: barb [options]\n"
        "       barb -p TASK [options]\n\n"
        "  -p, --prompt TASK   do one task and exit (TASK of - reads it from standard input)\n"
        "  -m, --model NAME    the model (default %s, or %%BARB_MODEL%%)\n"
        "  -y, --yes           do not ask before writing files or running commands\n"
        "  -C, --cwd DIR       work in DIR\n"
        "      --max-turns N   stop after N model calls without you typing (default 60)\n"
        "      --no-instructions  do not read AGENTS.md or CLAUDE.md from the working directory\n"
        "  -V, --version\n"
        "  -h, --help\n\n"
        "environment: ANTHROPIC_API_KEY (required), ANTHROPIC_BASE_URL, BARB_MODEL,\n"
        "             BARB_SHELL (the bash.exe to run commands in; Git for Windows' is found by itself)\n",
        VERSION, DEFAULT_MODEL);
    if (to_err) errz(b); else out(b, strlen(b));
}

static char* env_utf8(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, 0, 0);
    if (!n) return 0;
    wchar_t* w = malloc(n * sizeof *w);
    if (!w) return 0;
    char* s = GetEnvironmentVariableW(name, w, n) == n - 1 ? ops_utf8(w, -1) : 0;
    SecureZeroMemory(w, n * sizeof *w);   // it may be the key
    free(w);
    return s;
}

// The task from standard input (-p -): from a console, until Ctrl-Z.
static char* read_all_stdin(void) {
    size_t cap = 65536, n = 0;
    char* b = malloc(cap);
    if (!b) return 0;
    if (con_in) {
        wchar_t w[4096];
        DWORD got;
        for (;;) {
            if (!ReadConsoleW(h_in, w, 4096, &got, 0) || !got) break;
            DWORD end = 0;
            while (end < got && w[end] != 26) end++;
            char* s = ops_utf8(w, (int)end);
            size_t k = s ? strlen(s) : 0;
            while (n + k + 1 > cap) { cap *= 2; char* q = realloc(b, cap); if (!q) { free(s); b[n] = 0; return b; } b = q; }
            memcpy(b + n, s, k);
            n += k;
            free(s);
            if (end < got) break;
        }
    } else {
        DWORD got;
        while (ReadFile(h_in, b + n, (DWORD)(cap - n - 1), &got, 0) && got) {
            n += got;
            if (cap - n < 2) { cap *= 2; char* q = realloc(b, cap); if (!q) break; b = q; }
        }
    }
    b[n] = 0;
    return b;
}

static void platform_string(char* buf, size_t size) {
    typedef LONG(WINAPI * rtl_fn)(OSVERSIONINFOW*);
    OSVERSIONINFOW v;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    rtl_fn rtl = nt ? (rtl_fn)(void*)GetProcAddress(nt, "RtlGetVersion") : 0;
    if (!rtl || rtl(&v) != 0) v.dwMajorVersion = 0;
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const char* arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64"
                     : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64"
                     : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? "x86" : "unknown";
    char name[64];
    if (v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000) snprintf(name, sizeof name, "Windows 11 (build %lu)", (unsigned long)v.dwBuildNumber);
    else if (v.dwMajorVersion) snprintf(name, sizeof name, "Windows %lu.%lu (build %lu)", (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion, (unsigned long)v.dwBuildNumber);
    else snprintf(name, sizeof name, "Windows");
    const char* sh = ops_shell();
    if (sh)
        snprintf(buf, size, "%s %s. Commands run in Git Bash (bash -c, not sh), where paths are written /c/Users/...; "
                 "the file tools take C:/Users/... or /c/Users/.... Windows programs (cmd /c, powershell -Command, "
                 "py, where.exe) can be run from bash too", name, arch);
    else
        snprintf(buf, size, "%s %s. Commands cannot run: no Git Bash was found", name, arch);
}

static int cleanup_started;

static void cleanup(void) {
    if (cleanup_started) return;
    cleanup_started = 1;
    clear_line();
    ops_shutdown();
    if (restore_in) SetConsoleMode(h_in, mode_in);
    if (restore_out) SetConsoleMode(h_out, mode_out);
    if (restore_err) SetConsoleMode(h_err, mode_err);
    if (cleaned) SetEvent(cleaned);
}

static int arg_is(const char* a, const char* s, const char* l) { return !strcmp(a, s) || (l && !strcmp(a, l)); }

int wmain(int argc, wchar_t** wargv) {
    h_in = GetStdHandle(STD_INPUT_HANDLE);
    h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    h_err = GetStdHandle(STD_ERROR_HANDLE);
    con_in = h_in && h_in != INVALID_HANDLE_VALUE && GetConsoleMode(h_in, &mode_in);
    con_out = h_out && h_out != INVALID_HANDLE_VALUE && GetConsoleMode(h_out, &mode_out);
    con_err = h_err && h_err != INVALID_HANDLE_VALUE && GetConsoleMode(h_err, &mode_err);

    char** argv = calloc((size_t)argc + 1, sizeof *argv);
    if (!argv) return 1;
    for (int i = 0; i < argc; i++) argv[i] = ops_utf8(wargv[i], -1);

    const char* prompt = 0;
    char* model_env = env_utf8(L"BARB_MODEL");
    const char* model = model_env;
    const char* cwd_arg = 0;
    int yes = 0, max_turns = 0, no_instructions = 0;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i] ? argv[i] : "";
        int has_val = i + 1 < argc && argv[i + 1];
        if (arg_is(a, "-h", "--help")) { usage(0); return 0; }
        else if (arg_is(a, "-V", "--version")) { char b[40]; snprintf(b, sizeof b, "barb %s\n", VERSION); out(b, strlen(b)); return 0; }
        else if (arg_is(a, "-y", "--yes")) yes = 1;
        else if (arg_is(a, "--no-instructions", 0)) no_instructions = 1;
        else if (arg_is(a, "-p", "--prompt") && has_val) prompt = argv[++i];
        else if (arg_is(a, "-m", "--model") && has_val) model = argv[++i];
        else if (arg_is(a, "-C", "--cwd") && has_val) cwd_arg = argv[++i];
        else if (arg_is(a, "--max-turns", 0) && has_val) max_turns = atoi(argv[++i]);
        else {
            errz("barb: unknown or incomplete option ");
            errz(a);
            errz("\n\n");
            usage(1);
            return 2;
        }
    }
    if (!model || !*model) model = DEFAULT_MODEL;

    // Take the key out of the environment: every command the model runs inherits it otherwise,
    // and `echo $ANTHROPIC_API_KEY` would show it. Only the platform layer holds it from here.
    char* key = env_utf8(L"ANTHROPIC_API_KEY");
    SetEnvironmentVariableW(L"ANTHROPIC_API_KEY", 0);
    if (!key || !*key) {
        errz("barb: ANTHROPIC_API_KEY is not set.\n");
        return 2;
    }
    char* base = env_utf8(L"ANTHROPIC_BASE_URL");
    if (!base || !*base) { free(base); base = dup_n("https://api.anthropic.com", 25); }
    if (!base) return 1;
    size_t bl = strlen(base);
    while (bl && base[bl - 1] == '/') base[--bl] = 0;
    if (!strncmp(base, "http://", 7) && strncmp(base + 7, "127.0.0.1", 9) &&
        strncmp(base + 7, "localhost", 9) && strncmp(base + 7, "[::1]", 5))
        errz("barb: warning: ANTHROPIC_BASE_URL is plain http, so your API key will cross the network unencrypted.\n");

    if (cwd_arg) {
        wchar_t* w = ops_wide(cwd_arg, strlen(cwd_arg));
        if (!w || !SetCurrentDirectoryW(w)) {
            errz("barb: cannot change to ");
            errz(cwd_arg);
            errz("\n");
            return 2;
        }
        free(w);
    }
    wchar_t wcwd[32768];
    DWORD cn = GetCurrentDirectoryW(32768, wcwd);
    char* cwd = cn && cn < 32768 ? ops_utf8(wcwd, (int)cn) : dup_n(".", 1);
    char date[16];
    time_t now = time(0);
    struct tm lt;
    localtime_s(&lt, &now);
    strftime(date, sizeof date, "%Y-%m-%d", &lt);

    char* stdin_prompt = 0;
    if (prompt && !strcmp(prompt, "-")) { stdin_prompt = read_all_stdin(); prompt = stdin_prompt ? stdin_prompt : ""; }
    oneshot = prompt != 0;

    // VT sequences on the console, for the colours and the line editor. A console without them
    // (very old Windows) is treated like a pipe: plain text, no editor.
    if (con_out && SetConsoleMode(h_out, mode_out | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) restore_out = 1;
    if (con_err && SetConsoleMode(h_err, mode_err | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) restore_err = 1;
    tty_out = con_err && restore_err;
    if (!oneshot && con_in && tty_out) {
        DWORD raw = (mode_in & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)) | ENABLE_WINDOW_INPUT;
        if (SetConsoleMode(h_in, raw)) { restore_in = 1; interactive = 1; }
    }
    if (interactive) {
        CONSOLE_SCREEN_BUFFER_INFO info;
        ed_init(&ed, GetConsoleScreenBufferInfo(h_err, &info) ? info.dwSize.X : 80);
    }

    wake = CreateEventW(0, FALSE, FALSE, 0);
    cleaned = CreateEventW(0, TRUE, FALSE, 0);
    if (!wake || !cleaned) { errz("barb: cannot create an event\n"); return 1; }
    SetConsoleCtrlHandler(0, FALSE);   // a parent may have told this process to ignore Ctrl-C
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    ops_init(deliver, base, key);
    char plat[400];
    platform_string(plat, sizeof plat);

    agent_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.model = str_z(model);
    cfg.cwd = str_z(cwd ? cwd : ".");
    cfg.platform = str_z(plat);
    cfg.date = str_z(date);
    cfg.auto_approve = yes;
    cfg.max_turns = max_turns;
    cfg.instructions = !no_instructions;
    agent = agent_new(&mem, &cfg);
    if (!agent) { errz("barb: out of memory\n"); cleanup(); return 1; }

    if (!oneshot) {
        if (!interactive && h_in && h_in != INVALID_HANDLE_VALUE) {
            InitializeCriticalSection(&in_lock);
            in_ready = CreateEventW(0, TRUE, FALSE, 0);    // manual: the wait must not use it up
            HANDLE t = in_ready ? CreateThread(0, 0, reader, 0, 0, 0) : 0;
            if (t) CloseHandle(t);
            else stdin_open = 0;
        } else if (!interactive) {
            stdin_open = 0;
        }
        char b[600];
        snprintf(b, sizeof b, "barb %s, model %s, in %s\n/help for commands; Ctrl-D to exit.\n", VERSION, model, cwd ? cwd : ".");
        errz(b);
        if (!ops_shell()) {
            snprintf(b, sizeof b, "%sbarb: Git Bash was not found, so commands cannot run. Install Git for Windows, or set BARB_SHELL to a bash.exe.%s\n",
                     color(SAY_ERROR), tty_out ? "\033[0m" : "");
            errz(b);
        }
        if (!stdin_open) deliver_simple(EV_EOF, 0, 0, 0);
    } else {
        deliver_simple(EV_INPUT, 0, 0, prompt);
        deliver_simple(EV_EOF, 0, 0, 0);
    }
    loop();
    cleanup();

    agent_free(agent);
    SecureZeroMemory(key, strlen(key));
    free(key);
    free(paste);
    free(base);
    free(cwd);
    free(model_env);
    free(stdin_prompt);
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    // The reader thread may still be blocked on standard input; ending the process ends it.
    ExitProcess((UINT)exit_code);
}
