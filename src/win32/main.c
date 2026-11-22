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
// to the screen (screen.c), which keeps the prompt, what is going on and a footer at the bottom
// and draws them with VT sequences. Text goes to the console as UTF-16 (WriteConsoleW), so it
// shows right whatever the console's code page; to a pipe or a file it goes as the UTF-8 the
// agent made.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../core/agent.h"
#include "../ui/screen.h"
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

static sc_t scr;

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

// What the screen writes is gathered and written at once, when the loop is about to wait: a frame
// of the bottom block is many small pieces, and a console shows each write as it comes.
static char pend[1 << 16];
static size_t pend_n;
static int pend_fd;

static void flush_out(void) {
    if (pend_n) { if (pend_fd == 1) out(pend, pend_n); else err(pend, pend_n); }
    pend_n = 0;
}

static void sc_write(void* ctx, int fd, const char* s, size_t n) {
    (void)ctx;
    if (fd != pend_fd || pend_n + n > sizeof pend) { flush_out(); pend_fd = fd; }
    if (n > sizeof pend) { if (fd == 1) out(s, n); else err(s, n); return; }
    memcpy(pend + pend_n, s, n);
    pend_n += n;
}

// A time in ops_now_ms's units, as the wall clock will show it.
static void sc_clock(void* ctx, int64_t at, char* hhmm) {
    (void)ctx;
    time_t t = time(0) + (time_t)((at - ops_now_ms()) / 1000);
    struct tm lt;
    hhmm[0] = 0;
    if (!localtime_s(&lt, &t)) strftime(hhmm, 16, "%H:%M", &lt);
}

static int console_width(void) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    return con_err && GetConsoleScreenBufferInfo(h_err, &info) ? info.dwSize.X : 80;
}

static void draw(void) {
    int prompt_ok = interactive && !awaiting && stdin_open && !done;
    sc_draw(&scr, ops_now_ms(), agent_busy(agent), prompt_ok, !awaiting && !done);
}

// ---- the keyboard ----

static void answer(int ans) {
    int64_t id = awaiting;
    awaiting = 0;
    sc_answer(&scr, ans);
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
    int n;
    const char* line = sc_take(&scr, &n);
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
        case 'y': case 'Y': answer(ANS_YES); break;
        case 'a': case 'A': answer(ANS_ALWAYS); break;
        case 'n': case 'N': case '\r': case '\n': case 27: case 4: answer(ANS_NO); break;
        case 3: deliver_simple(EV_INTERRUPT, 0, 0, 0); break;
        }
        return;
    }
    if (!ch) {
        switch (vk) {
        case VK_LEFT: sc_key(&scr, ctrl ? EK_WORD_LEFT : EK_LEFT, 0); break;
        case VK_RIGHT: sc_key(&scr, ctrl ? EK_WORD_RIGHT : EK_RIGHT, 0); break;
        case VK_HOME: sc_key(&scr, EK_HOME, 0); break;
        case VK_END: sc_key(&scr, EK_END, 0); break;
        case VK_DELETE: sc_key(&scr, EK_DEL, 0); break;
        case VK_UP: sc_key(&scr, EK_UP, 0); break;
        case VK_DOWN: sc_key(&scr, EK_DOWN, 0); break;
        }
        return;
    }
    int k = -1;
    switch (ch) {
    case 3:   // Ctrl-C: stop the work; at an idle prompt, throw away what was typed first
        if (!agent_busy(agent) && scr.ed.n) sc_key(&scr, EK_CLEAR, 0);
        else deliver_simple(EV_INTERRUPT, 0, 0, 0);
        return;
    case 4:   // Ctrl-D: on an empty line, the end; otherwise delete forward, as in a shell
        if (scr.ed.n) { k = EK_DEL; break; }
        sc_hide(&scr);
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
    if (k >= 0) sc_key(&scr, k, 0);
    else sc_key(&scr, EK_CHAR, ch);
}

static WCHAR high_surrogate;
static uint32_t last_ch;

static void console_record(const INPUT_RECORD* r) {
    if (r->EventType == WINDOW_BUFFER_SIZE_EVENT) {
        int w = console_width();
        if (w != scr.cfg.width) sc_resize(&scr, w);
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

static const char not_allowed[] =
    "(not allowed: there is no one to ask. Run with --yes to let barb write files and run commands unasked.)";

static void ask(const act_t* a) {
    if (oneshot || !interactive) {
        sc_say(&scr, SAY_ERROR, not_allowed, sizeof not_allowed - 1);
        deliver_simple(EV_ANSWER, a->id, ANS_NO, 0);
        return;
    }
    read_console();   // keys typed before the question go to the line, so they cannot answer it
    sc_question(&scr, a->a.p, a->a.n, a->b.p, a->b.n);
    awaiting = a->id;
}

// Every action is shown to the screen first (ACT_SAY is only that), then carried out.
static void exec(const act_t* a) {
    sc_action(&scr, a, ops_now_ms());
    switch (a->kind) {
    case ACT_SAY: break;
    case ACT_HTTP: ops_http(a->id, a->a, a->b); break;
    case ACT_READ: ops_read(a->id, a->a); break;
    case ACT_WRITE: ops_write(a->id, a->a, a->b); break;
    case ACT_SPAWN: ops_spawn(a->id, a->a, a->n, a->flag, a->c); break;
    case ACT_TIMER: ops_timer(a->id, a->n); break;
    case ACT_CANCEL:
        if (a->id == awaiting) { awaiting = 0; sc_hide(&scr); }
        else ops_cancel(a->id);
        break;
    case ACT_ASK: ask(a); break;
    case ACT_DONE: done = 1; exit_code = (int)a->n; break;
    }
}

static void step(const ev_t* ev) {
    sc_event(&scr, ev);
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
        draw();
        flush_out();
        int n = 0, slot_console = -1, slot_lines = -1;
        h[n++] = wake;
        if (stdin_open && !oneshot) {
            if (interactive) { slot_console = n; h[n++] = h_in; }
            else if (in_ready) { slot_lines = n; h[n++] = in_ready; }
        }
        n += ops_handles((void**)(h + n), MAXIMUM_WAIT_OBJECTS - n);
        int timeout = ops_timeout_ms(), anim = sc_next_ms(&scr, ops_now_ms());   // the spinner's next frame
        if (anim >= 0 && (timeout < 0 || timeout > anim)) timeout = anim;
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
    sc_hide(&scr);
    flush_out();
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
    // The screen: the whole bottom block for an interactive session, a status line on a console
    // that is not ours to edit, plain text for pipes and files. Windows Terminal's fonts have
    // the braille for the spinner; the old console's may not.
    char* home = env_utf8(L"USERPROFILE");
    sc_cfg_t sc;
    memset(&sc, 0, sizeof sc);
    sc.mode = interactive ? SC_REGION : tty_out ? SC_LITE : SC_PIPE;
    sc.vt_out = con_out && restore_out;
    sc.vt_err = tty_out;
    sc.fancy = GetEnvironmentVariableW(L"WT_SESSION", 0, 0) > 0 || GetEnvironmentVariableW(L"TERM_PROGRAM", 0, 0) > 0;
    sc.width = console_width();
    sc.model = model;
    sc.cwd = cwd ? cwd : ".";
    sc.home = home;
    sc.write = sc_write;
    sc.clock = sc_clock;
    sc_init(&scr, &sc);
    free(home);

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
        sc_banner(&scr, VERSION);
        if (!ops_shell()) {
            static const char no_bash[] =
                "barb: Git Bash was not found, so commands cannot run. Install Git for Windows, or set BARB_SHELL to a bash.exe.";
            sc_say(&scr, SAY_ERROR, no_bash, sizeof no_bash - 1);
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
