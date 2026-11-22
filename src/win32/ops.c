// Operations on Windows: see ops.h.
//
// Processes. Each command is `bash -c COMMAND` in Git Bash, started suspended, put in its own
// job object (which kills everything in it when its handle closes), and then let go. That is
// Windows' process group: stopping a command stops whatever it started too. Commands get a
// console of their own with no window (CREATE_NO_WINDOW), so programs they run open no window
// and the user's Ctrl-C reaches barb, not them. Only the handles meant for the child are
// inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST). There is no SIGTERM: a command that is
// stopped or runs out of time is killed at once.
//
// Output comes through a named pipe read with overlapped I/O, so the main loop can sleep in
// WaitForMultipleObjects on every pipe and process at once and needs no polling at all: a
// background task that runs for an hour costs nothing until it exits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ops.h"

#define MAX_OPS 64
#define MAX_PARKED 64
#define HEAD_KEEP (1 << 20)          // a process's output: the first megabyte...
#define TAIL_KEEP (1 << 20)          // ...and the last, with the middle dropped
#define HTTP_MAX (64 << 20)
#define READ_LIMIT (8 << 20)
#define RBUF 65536
#define DRAIN_MS 5000                // the longest finish() waits for the last of the output

enum { K_FREE, K_PROC, K_HTTP, K_TIMER };

typedef struct {
    char* p;
    size_t n, cap;
} dbuf_t;

typedef struct {
    int kind;
    int64_t id;
    HANDLE proc, job;                // job may be 0 if Windows refused one
    HANDLE pipe;                     // the read end of the output, or 0
    OVERLAPPED ov;
    char* rbuf;
    int reading;                     // a read is in flight on ov
    int bg, exited, killed;
    DWORD code;
    int cancelled, timed_out;
    int64_t started, deadline;       // ms; 0 = none
    // process output
    dbuf_t head;
    char* ring;
    size_t ring_pos;
    uint64_t total;
    // http
    dbuf_t body;
    wchar_t* tmpfile;
} op_t;

static op_t ops[MAX_OPS];
static HANDLE parked[MAX_PARKED];    // jobs of finished background tasks, with whatever they left running
static int nparked;
static int overflow;                 // more handles than the main loop could wait on
static deliver_fn deliver;
static const char* g_base;
static const char* g_key;
static wchar_t* g_shell;             // bash.exe, or 0
static char* g_shell_u8;
static wchar_t* g_curl;              // curl.exe, or 0
static volatile LONG pipe_serial;

int64_t ops_now_ms(void) { return (int64_t)GetTickCount64(); }

// ---- UTF-8 and UTF-16 ----

wchar_t* ops_wide(const char* s, size_t n) {
    int k = n ? MultiByteToWideChar(CP_UTF8, 0, s, (int)n, 0, 0) : 0;
    wchar_t* w = malloc(((size_t)k + 1) * sizeof *w);
    if (!w) return 0;
    if (k) MultiByteToWideChar(CP_UTF8, 0, s, (int)n, w, k);
    w[k] = 0;
    return w;
}

char* ops_utf8(const wchar_t* w, int n) {
    if (n < 0) n = (int)wcslen(w);
    int k = n ? WideCharToMultiByte(CP_UTF8, 0, w, n, 0, 0, 0, 0) : 0;
    char* s = malloc((size_t)k + 1);
    if (!s) return 0;
    if (k) WideCharToMultiByte(CP_UTF8, 0, w, n, s, k, 0, 0);
    s[k] = 0;
    return s;
}

// What Windows says about an error, in UTF-8, without the trailing period and newline.
static const char* why(DWORD err) {
    static char out[512];
    wchar_t w[400];
    DWORD k = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 0, err,
                             MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), w, 400, 0);
    while (k && (w[k - 1] == '\n' || w[k - 1] == '\r' || w[k - 1] == ' ' || w[k - 1] == '.')) k--;
    if (!k) { snprintf(out, sizeof out, "Windows error %lu", (unsigned long)err); return out; }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)k, out, (int)sizeof out - 1, 0, 0);
    out[n > 0 ? n : 0] = 0;
    return out;
}

// ---- growing buffers ----

typedef struct {
    wchar_t* p;
    size_t n, cap;
} wbuf_t;

static void wb_add(wbuf_t* b, const wchar_t* s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        wchar_t* q = realloc(b->p, cap * sizeof *q);
        if (!q) return;
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n * sizeof *s);
    b->n += n;
    b->p[b->n] = 0;
}
static void wb_z(wbuf_t* b, const wchar_t* s) { wb_add(b, s, wcslen(s)); }

// One argument of a command line, quoted the way CommandLineToArgvW and the C runtimes
// (Git Bash's included) take it apart: backslashes are literal unless they come before a quote.
static void wb_arg(wbuf_t* b, const wchar_t* a) {
    if (b->n) wb_z(b, L" ");
    wb_z(b, L"\"");
    for (const wchar_t* c = a;; c++) {
        size_t bs = 0;
        while (*c == '\\') { bs++; c++; }
        if (!*c) { for (size_t i = 0; i < bs * 2; i++) wb_z(b, L"\\"); break; }
        if (*c == '"') { for (size_t i = 0; i < bs * 2 + 1; i++) wb_z(b, L"\\"); }
        else { for (size_t i = 0; i < bs; i++) wb_z(b, L"\\"); }
        wb_add(b, c, 1);
    }
    wb_z(b, L"\"");
}

static void dbuf_add(dbuf_t* b, const char* p, size_t n, size_t max) {
    if (b->n + n > max) n = max > b->n ? max - b->n : 0;
    if (!n) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        char* q = realloc(b->p, cap);
        if (!q) return;
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}

// ---- finding programs ----

static int is_file(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static wchar_t* wjoin(const wchar_t* a, const wchar_t* b) {
    size_t na = wcslen(a), nb = wcslen(b);
    wchar_t* r = malloc((na + nb + 2) * sizeof *r);
    if (!r) return 0;
    memcpy(r, a, na * sizeof *r);
    size_t k = na;
    if (k && r[k - 1] != '\\' && r[k - 1] != '/') r[k++] = '\\';
    memcpy(r + k, b, (nb + 1) * sizeof *r);
    return r;
}

static wchar_t* env_w(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, 0, 0);
    if (!n) return 0;
    wchar_t* v = malloc(n * sizeof *v);
    if (v && GetEnvironmentVariableW(name, v, n) != n - 1) { free(v); v = 0; }
    return v;
}

// A program in a directory on PATH. Only absolute directories count: Windows' own search
// would try the working directory first, and a project that ships a curl.exe must not get
// the API key.
static wchar_t* on_path(const wchar_t* exe) {
    wchar_t* path = env_w(L"PATH");
    if (!path) return 0;
    wchar_t* found = 0;
    for (wchar_t *dir = path, *end; dir && *dir && !found; dir = end ? end + 1 : 0) {
        end = wcschr(dir, ';');
        if (end) *end = 0;
        size_t n = wcslen(dir);
        if (n >= 2 && dir[0] == '"' && dir[n - 1] == '"') { dir[n - 1] = 0; dir++; n -= 2; }
        int absolute = n >= 3 && ((dir[1] == ':' && (dir[2] == '\\' || dir[2] == '/')) || (dir[0] == '\\' && dir[1] == '\\'));
        if (!absolute) continue;
        wchar_t* p = wjoin(dir, exe);
        if (p && is_file(p)) found = p;
        else free(p);
    }
    free(path);
    return found;
}

// bin\bash.exe under a Git for Windows installation: the launcher that sets up PATH so that
// ls, grep and the rest are found (usr\bin\bash.exe alone does not).
static wchar_t* git_bash(const wchar_t* root) {
    wchar_t* p = wjoin(root, L"bin\\bash.exe");
    if (p && is_file(p)) return p;
    free(p);
    return 0;
}

static wchar_t* find_shell(void) {
    wchar_t* p = env_w(L"BARB_SHELL");
    if (p) {
        if (is_file(p)) return p;
        free(p);
        return 0;                    // set, and wrong: say so rather than use something else
    }
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof buf;
    HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    for (int i = 0; i < 2; i++) {
        size = sizeof buf;
        if (RegGetValueW(roots[i], L"SOFTWARE\\GitForWindows", L"InstallPath", RRF_RT_REG_SZ, 0, buf, &size) == ERROR_SUCCESS &&
            (p = git_bash(buf)))
            return p;
    }
    // git.exe on PATH is Git\cmd\git.exe or Git\mingw64\bin\git.exe.
    wchar_t* git = on_path(L"git.exe");
    if (git) {
        wchar_t* cut = wcsrchr(git, '\\');
        for (int up = 0; cut && up < 3; up++) {
            *cut = 0;
            if ((p = git_bash(git))) { free(git); return p; }
            cut = wcsrchr(git, '\\');
        }
        free(git);
    }
    const wchar_t* dirs[3][2] = { { L"ProgramW6432", L"Git" }, { L"ProgramFiles", L"Git" }, { L"LOCALAPPDATA", L"Programs\\Git" } };
    for (int i = 0; i < 3; i++) {
        wchar_t* base = env_w(dirs[i][0]);
        if (!base) continue;
        wchar_t* root = wjoin(base, dirs[i][1]);
        free(base);
        if (root && (p = git_bash(root))) { free(root); return p; }
        free(root);
    }
    return 0;
}

static wchar_t* find_curl(void) {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n && n < MAX_PATH) {
        wchar_t* p = wjoin(sys, L"curl.exe");
        if (p && is_file(p)) return p;
        free(p);
    }
    return on_path(L"curl.exe");
}

const char* ops_shell(void) { return g_shell_u8; }

void ops_init(deliver_fn d, const char* base_url, const char* api_key) {
    deliver = d;
    g_base = base_url;
    g_key = api_key;
    g_shell = find_shell();
    if (g_shell) g_shell_u8 = ops_utf8(g_shell, -1);
    g_curl = find_curl();
}

// ---- operations ----

static op_t* find(int64_t id) {
    for (int i = 0; i < MAX_OPS; i++) if (ops[i].kind != K_FREE && ops[i].id == id) return &ops[i];
    return 0;
}

static op_t* alloc_op(int kind, int64_t id) {
    for (int i = 0; i < MAX_OPS; i++) {
        if (ops[i].kind != K_FREE) continue;
        memset(&ops[i], 0, sizeof ops[i]);
        ops[i].kind = kind;
        ops[i].id = id;
        ops[i].started = ops_now_ms();
        return &ops[i];
    }
    return 0;
}

static void close_pipe(op_t* o) {
    if (!o->pipe) return;
    if (o->reading) {                // the buffer must outlive the read
        DWORD got;
        CancelIoEx(o->pipe, &o->ov);
        GetOverlappedResult(o->pipe, &o->ov, &got, TRUE);
        o->reading = 0;
    }
    CloseHandle(o->pipe);
    o->pipe = 0;
}

static void release(op_t* o) {
    close_pipe(o);
    if (o->ov.hEvent) CloseHandle(o->ov.hEvent);
    if (o->proc) CloseHandle(o->proc);
    if (o->job) CloseHandle(o->job);      // kills anything still in it
    free(o->rbuf);
    free(o->head.p);
    free(o->ring);
    free(o->body.p);
    if (o->tmpfile) { DeleteFileW(o->tmpfile); free(o->tmpfile); }
    memset(o, 0, sizeof *o);
}

static void kill_op(op_t* o) {
    if (o->exited) return;
    o->killed = 1;
    if (o->job) TerminateJobObject(o->job, 1);
    else if (o->proc) TerminateProcess(o->proc, 1);
}

static void fail_start(int64_t id, int kind, const char* note) {
    ev_t e;
    memset(&e, 0, sizeof e);
    e.id = id;
    e.note = str_z(note);
    if (kind == K_HTTP) { e.kind = EV_HTTP; e.status = 0; }
    else { e.kind = EV_PROC; e.flags = PROC_FAILED; e.status = -1; }
    deliver(&e);
}

// ---- output ----

// Keep the head of the output, then a ring of the most recent bytes.
static void proc_add(op_t* o, const char* p, size_t n) {
    o->total += n;
    if (o->head.n < HEAD_KEEP) {
        size_t take = HEAD_KEEP - o->head.n;
        if (take > n) take = n;
        dbuf_add(&o->head, p, take, HEAD_KEEP);
        p += take;
        n -= take;
    }
    if (!n) return;
    if (!o->ring && !(o->ring = malloc(TAIL_KEEP))) { o->total -= n; return; }
    for (size_t i = 0; i < n; i++) o->ring[(o->ring_pos + i) % TAIL_KEEP] = p[i];
    o->ring_pos += n;
}

// The output as one string: the head, a note if the middle was dropped, the tail.
static char* proc_output(op_t* o, size_t* len) {
    size_t head = o->head.n;
    uint64_t rest = o->total - head;
    size_t tail = rest > TAIL_KEEP ? TAIL_KEEP : (size_t)rest;
    uint64_t dropped = rest - tail;
    char note[80] = "";
    if (dropped) snprintf(note, sizeof note, "\n[... %llu bytes of output dropped here ...]\n", (unsigned long long)dropped);
    size_t nn = strlen(note);
    char* out = malloc(head + nn + tail + 1);
    if (!out) { *len = 0; return 0; }
    size_t n = 0;
    if (head) { memcpy(out, o->head.p, head); n = head; }
    memcpy(out + n, note, nn);
    n += nn;
    for (size_t i = 0; i < tail; i++) out[n + i] = o->ring[(o->ring_pos - tail + i) % TAIL_KEEP];
    n += tail;
    out[n] = 0;
    *len = n;
    return out;
}

static void take(op_t* o, const char* p, size_t n) {
    if (o->kind == K_PROC) proc_add(o, p, n);
    else dbuf_add(&o->body, p, n, HTTP_MAX);
}

// Collect what has arrived and keep a read in flight, until the pipe is empty or closed.
static void pump(op_t* o) {
    for (int rounds = 0; o->pipe && rounds < 64; rounds++) {
        if (o->reading) {
            DWORD got;
            if (!GetOverlappedResult(o->pipe, &o->ov, &got, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) return;
                o->reading = 0;
                close_pipe(o);           // the end of the output (every writer has closed it)
                return;
            }
            o->reading = 0;
            take(o, o->rbuf, got);
        }
        HANDLE ev = o->ov.hEvent;
        memset(&o->ov, 0, sizeof o->ov);
        o->ov.hEvent = ev;
        if (ReadFile(o->pipe, o->rbuf, RBUF, 0, &o->ov) || GetLastError() == ERROR_IO_PENDING) {
            o->reading = 1;              // done at once or later: GetOverlappedResult tells
            continue;
        }
        close_pipe(o);
    }
}

// After the process has gone (and taken everything it started with it), the rest of its output.
static void drain_all(op_t* o) {
    int64_t until = ops_now_ms() + DRAIN_MS;
    while (o->pipe) {
        pump(o);
        if (!o->pipe) return;
        int64_t left = until - ops_now_ms();
        if (left <= 0 || WaitForSingleObject(o->ov.hEvent, (DWORD)left) == WAIT_TIMEOUT) { close_pipe(o); return; }
    }
}

// ---- processes ----

static SECURITY_ATTRIBUTES inheritable = { sizeof(SECURITY_ATTRIBUTES), 0, TRUE };

// A pipe for a child's output: an overlapped read end for barb, an inheritable write end for the child.
static int make_pipe(op_t* o, HANDLE* child_end) {
    wchar_t name[96];
    swprintf(name, 96, L"\\\\.\\pipe\\barb-%lu-%ld", (unsigned long)GetCurrentProcessId(), InterlockedIncrement(&pipe_serial));
    o->pipe = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                               PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, RBUF, 0, 0);
    if (o->pipe == INVALID_HANDLE_VALUE) { o->pipe = 0; return 0; }
    // FILE_READ_ATTRIBUTES: Git Bash's runtime looks at what kind of handle it was given.
    *child_end = CreateFileW(name, GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, &inheritable, OPEN_EXISTING, 0, 0);
    if (*child_end == INVALID_HANDLE_VALUE) { CloseHandle(o->pipe); o->pipe = 0; return 0; }
    o->ov.hEvent = CreateEventW(0, TRUE, FALSE, 0);
    o->rbuf = malloc(RBUF);
    if (!o->ov.hEvent || !o->rbuf) { CloseHandle(*child_end); return 0; }
    return 1;
}

// Start `cmdline` with `app` in a job of its own, its standard input from `in` and its output
// to `out` (both inheritable). 0 on failure, with the reason in GetLastError.
static int start(op_t* o, const wchar_t* app, wchar_t* cmdline, HANDLE in, HANDLE out) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(0, 1, 0, &size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = malloc(size);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &size)) { free(attrs); SetLastError(ERROR_OUTOFMEMORY); return 0; }
    HANDLE list[2] = { in, out };
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, (in == out ? 1 : 2) * sizeof(HANDLE), 0, 0);
    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = out;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(app, cmdline, 0, 0, TRUE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                             0, 0, &si.StartupInfo, &pi);
    DWORD err = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    free(attrs);
    if (!ok) { SetLastError(err); return 0; }
    o->job = CreateJobObjectW(0, 0);
    if (o->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(o->job, JobObjectExtendedLimitInformation, &li, sizeof li) ||
            !AssignProcessToJobObject(o->job, pi.hProcess)) {
            CloseHandle(o->job);     // run it anyway; stopping it then stops only the process itself
            o->job = 0;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    o->proc = pi.hProcess;
    return 1;
}

static HANDLE open_nul(void) {
    return CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, 0);
}

static wchar_t* wpath(str_t p);
static void mkdir_parents(wchar_t* path);

void ops_spawn(int64_t id, str_t cmd, int64_t timeout_ms, int bg, str_t log) {
    if (!g_shell) {
        fail_start(id, K_PROC, "Git Bash was not found. barb runs commands in the bash that comes with Git for Windows: "
                               "install Git for Windows, or set BARB_SHELL to the full path of a bash.exe");
        return;
    }
    op_t* o = alloc_op(K_PROC, id);
    if (!o) { fail_start(id, K_PROC, "too many operations running at once"); return; }
    o->bg = bg;
    wchar_t* wcmd = ops_wide(cmd.p, cmd.n);
    wbuf_t line = { 0, 0, 0 };
    wb_arg(&line, g_shell);
    wb_arg(&line, L"-c");
    if (wcmd) wb_arg(&line, wcmd);
    free(wcmd);
    if (!line.p) { release(o); fail_start(id, K_PROC, "out of memory"); return; }

    HANDLE in = open_nul(), out = INVALID_HANDLE_VALUE;
    char note[400];
    if (bg) {
        wchar_t* lp = wpath(log);
        if (lp) {
            mkdir_parents(lp);
            out = CreateFileW(lp, GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        }
        free(lp);
        if (out == INVALID_HANDLE_VALUE) {
            snprintf(note, sizeof note, "cannot open the log file %s: %s", log.p, why(GetLastError()));
            if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
            free(line.p);
            release(o);
            fail_start(id, K_PROC, note);
            return;
        }
    } else if (!make_pipe(o, &out)) {
        snprintf(note, sizeof note, "cannot make a pipe for the output: %s", why(GetLastError()));
        if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
        free(line.p);
        release(o);
        fail_start(id, K_PROC, note);
        return;
    }
    int ok = in != INVALID_HANDLE_VALUE && start(o, g_shell, line.p, in, out);
    DWORD err = GetLastError();
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    CloseHandle(out);                // the child has its own; this end must close for the pipe to end
    free(line.p);
    if (!ok) {
        snprintf(note, sizeof note, "cannot start %s: %s", g_shell_u8, why(err));
        release(o);
        fail_start(id, K_PROC, note);
        return;
    }
    if (!bg) {
        if (timeout_ms > 0) o->deadline = o->started + timeout_ms;
        pump(o);
    }
}

// ---- HTTP, through curl ----
//
// As on Linux: the key and the headers go to curl on its standard input as a config file, never
// on the command line; the body goes in a temporary file; curl prints the body and then the status
// on its own last line.

static void cfg_quoted(dbuf_t* b, const char* key, const char* value) {
    dbuf_add(b, key, strlen(key), SIZE_MAX);
    dbuf_add(b, " = \"", 4, SIZE_MAX);
    for (const char* c = value; *c; c++) {
        if (*c == '"' || *c == '\\') dbuf_add(b, "\\", 1, SIZE_MAX);
        if (*c == '\n' || *c == '\r') continue;
        dbuf_add(b, c, 1, SIZE_MAX);
    }
    dbuf_add(b, "\"\n", 2, SIZE_MAX);
}

static int write_all(HANDLE h, const char* p, size_t n) {
    while (n) {
        DWORD chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n, w = 0;
        if (!WriteFile(h, p, chunk, &w, 0) || !w) return 0;
        p += w;
        n -= w;
    }
    return 1;
}

void ops_http(int64_t id, str_t path, str_t body) {
    if (!g_curl) { fail_start(id, K_HTTP, "curl.exe was not found (Windows 10 and later have it in System32)"); return; }
    op_t* o = alloc_op(K_HTTP, id);
    if (!o) { fail_start(id, K_HTTP, "too many operations running at once"); return; }

    wchar_t dir[MAX_PATH + 1], file[MAX_PATH + 1];
    if (!GetTempPathW(MAX_PATH + 1, dir) || !GetTempFileNameW(dir, L"brb", 0, file)) {
        release(o);
        fail_start(id, K_HTTP, "cannot make a temporary file for the request");
        return;
    }
    o->tmpfile = _wcsdup(file);
    HANDLE bf = CreateFileW(file, GENERIC_WRITE, 0, 0, TRUNCATE_EXISTING, FILE_ATTRIBUTE_TEMPORARY, 0);
    int wrote = bf != INVALID_HANDLE_VALUE && write_all(bf, body.p, body.n);
    if (bf != INVALID_HANDLE_VALUE) CloseHandle(bf);
    if (!wrote) { release(o); fail_start(id, K_HTTP, "cannot write the request to a temporary file"); return; }
    // curl may open files with the ANSI code page, so give it the short (8.3) name, which has
    // no characters a user name might bring into the temporary directory's path.
    wchar_t shortname[MAX_PATH + 1];
    DWORD sn = GetShortPathNameW(file, shortname, MAX_PATH + 1);
    char* data_path = ops_utf8(sn && sn <= MAX_PATH ? shortname : file, -1);

    char* url = malloc(strlen(g_base) + path.n + 1);
    char* keyhdr = malloc(strlen(g_key) + 16);
    char* datafile = data_path ? malloc(strlen(data_path) + 2) : 0;
    if (!url || !keyhdr || !datafile) {
        free(url); free(keyhdr); free(datafile); free(data_path);
        release(o);
        fail_start(id, K_HTTP, "out of memory");
        return;
    }
    sprintf(url, "%s%.*s", g_base, (int)path.n, path.p);
    sprintf(keyhdr, "x-api-key: %s", g_key);
    sprintf(datafile, "@%s", data_path);
    free(data_path);
    dbuf_t cfg = { 0, 0, 0 };
    cfg_quoted(&cfg, "url", url);
    cfg_quoted(&cfg, "request", "POST");
    cfg_quoted(&cfg, "header", "content-type: application/json");
    cfg_quoted(&cfg, "header", "anthropic-version: 2023-06-01");
    cfg_quoted(&cfg, "header", keyhdr);
    cfg_quoted(&cfg, "data-binary", datafile);
    SecureZeroMemory(keyhdr, strlen(keyhdr));
    free(url); free(keyhdr); free(datafile);

    wbuf_t line = { 0, 0, 0 };
    const wchar_t* args[] = { L"-sS", L"--stderr", L"-", L"--connect-timeout", L"30", L"--max-time", L"1200",
                              L"-K", L"-", L"-w", L"\\n%{http_code}" };
    wb_arg(&line, g_curl);
    for (size_t i = 0; i < sizeof args / sizeof *args; i++) wb_arg(&line, args[i]);

    HANDLE in_r = 0, in_w = 0, out = INVALID_HANDLE_VALUE;
    int ok = line.p && cfg.p && CreatePipe(&in_r, &in_w, 0, 1 << 16) &&
             SetHandleInformation(in_r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) && make_pipe(o, &out) &&
             start(o, g_curl, line.p, in_r, out);
    DWORD err = GetLastError();
    if (in_r) CloseHandle(in_r);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    free(line.p);
    if (ok) {
        // The config is far smaller than the pipe's buffer, so this does not block. (A curl
        // that died at once makes it fail, which is harmless: its exit says what happened.)
        write_all(in_w, cfg.p, cfg.n);
    }
    if (in_w) CloseHandle(in_w);
    if (cfg.p) { SecureZeroMemory(cfg.p, cfg.cap); free(cfg.p); }
    if (!ok) {
        char note[300];
        snprintf(note, sizeof note, "cannot start curl: %s", why(err));
        release(o);
        fail_start(id, K_HTTP, note);
        return;
    }
    pump(o);
}

// ---- files ----

static void file_event(int64_t id, int status, const char* note, const char* data, size_t n) {
    ev_t e;
    memset(&e, 0, sizeof e);
    e.kind = EV_FILE;
    e.id = id;
    e.status = status;
    if (note) e.note = str_z(note);
    if (data) e.data = str_of(data, n);
    deliver(&e);
}

static int fs_status(DWORD err) {
    switch (err) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_DRIVE: case ERROR_BAD_NETPATH: case ERROR_BAD_NET_NAME: case ERROR_DIRECTORY:
        return FS_NOTFOUND;
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: case ERROR_WRITE_PROTECT:
        return FS_DENIED;
    }
    return FS_ERROR;
}

static void file_error(int64_t id, DWORD err) { file_event(id, fs_status(err), why(err), 0, 0); }

// A path the model gave, as Windows wants it: UTF-16, absolute, with the Git Bash spellings
// (/c/Users/..., /tmp/...) turned into Windows ones, and the \\?\ prefix when it is long.
static wchar_t* wpath(str_t p) {
    const char* s = p.p;
    size_t n = p.n;
    wchar_t* w = 0;
    if (n >= 2 && s[0] == '/' && ((s[1] >= 'a' && s[1] <= 'z') || (s[1] >= 'A' && s[1] <= 'Z')) && (n == 2 || s[2] == '/')) {
        char* t = malloc(n + 2);
        if (!t) return 0;
        t[0] = s[1];
        t[1] = ':';
        t[2] = '/';
        if (n > 3) memcpy(t + 3, s + 3, n - 3);
        t[n > 3 ? n : 3] = 0;
        w = ops_wide(t, strlen(t));
        free(t);
    } else if (n >= 4 && !memcmp(s, "/tmp", 4) && (n == 4 || s[4] == '/')) {
        wchar_t tmp[MAX_PATH + 1];
        DWORD k = GetTempPathW(MAX_PATH + 1, tmp);
        wchar_t* rest = ops_wide(s + 4 + (n > 4), n - 4 - (n > 4));
        if (k && rest) w = wjoin(tmp, rest);
        free(rest);
    } else {
        w = ops_wide(s, n);
    }
    if (!w) return 0;
    DWORD need = GetFullPathNameW(w, 0, 0, 0);
    if (!need) return w;
    wchar_t* full = malloc(((size_t)need + 8) * sizeof *full);
    if (!full) return w;
    DWORD got = GetFullPathNameW(w, need, full + 7, 0);
    free(w);
    if (!got || got >= need) { free(full); return 0; }
    wchar_t* r = full + 7;
    if (got >= MAX_PATH - 12 && wcsncmp(r, L"\\\\?\\", 4)) {
        if (r[0] == '\\' && r[1] == '\\') { r -= 6; memcpy(r, L"\\\\?\\UNC", 7 * sizeof *r); }   // the second \ of \\server stays
        else { r -= 4; memcpy(r, L"\\\\?\\", 4 * sizeof *r); }
    }
    memmove(full, r, (wcslen(r) + 1) * sizeof *r);
    return full;
}

// All the directories above `path` (an absolute path from wpath), made if they are missing.
static void mkdir_parents(wchar_t* path) {
    wchar_t* p = path;
    int unc = 0;
    if (!wcsncmp(p, L"\\\\?\\UNC\\", 8)) { p += 8; unc = 1; }
    else if (!wcsncmp(p, L"\\\\?\\", 4)) p += 4;
    else if (p[0] == '\\' && p[1] == '\\') { p += 2; unc = 1; }
    if (unc) {                       // past server\share
        p = wcschr(p, '\\');
        if (p) p = wcschr(p + 1, '\\');
        if (!p) return;
    } else if (p[0] && p[1] == ':') {
        p += 2;
    }
    if (!*p) return;
    for (wchar_t* c = p + 1; *c; c++) {
        if (*c != '\\') continue;
        *c = 0;
        CreateDirectoryW(path, 0);
        *c = '\\';
    }
}

void ops_read(int64_t id, str_t path) {
    wchar_t* w = wpath(path);
    if (!w) { file_event(id, FS_ERROR, "out of memory", 0, 0); return; }
    DWORD attrs = GetFileAttributesW(w);
    if (attrs == INVALID_FILE_ATTRIBUTES) { file_error(id, GetLastError()); free(w); return; }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) { file_event(id, FS_ISDIR, 0, 0, 0); free(w); return; }
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, 0);
    free(w);
    if (h == INVALID_HANDLE_VALUE) { file_error(id, GetLastError()); return; }
    if (GetFileType(h) != FILE_TYPE_DISK) { CloseHandle(h); file_event(id, FS_ERROR, "not a file on disk (a device such as CON or NUL)", 0, 0); return; }
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart > READ_LIMIT) {
        char note[96];
        snprintf(note, sizeof note, "%lld bytes, the limit is %d", (long long)size.QuadPart, READ_LIMIT);
        CloseHandle(h);
        file_event(id, FS_TOOBIG, note, 0, 0);
        return;
    }
    dbuf_t b = { 0, 0, 0 };
    char buf[65536];
    DWORD r = 0, err = 0;
    for (;;) {
        if (!ReadFile(h, buf, sizeof buf, &r, 0)) { err = GetLastError(); break; }
        if (!r) break;
        dbuf_add(&b, buf, r, READ_LIMIT + 1);
        if (b.n > (size_t)READ_LIMIT) break;
    }
    CloseHandle(h);
    if (b.n > (size_t)READ_LIMIT) { free(b.p); file_event(id, FS_TOOBIG, "more than the size limit", 0, 0); return; }
    if (err) { free(b.p); file_error(id, err); return; }
    file_event(id, FS_OK, 0, b.p ? b.p : "", b.n);
    free(b.p);
}

// Write to a temporary file beside the target and move it into place, so a crash or a full disk
// never leaves half a file. ReplaceFileW keeps the old file's attributes and permissions. A
// symbolic link is followed, so the link stays.
void ops_write(int64_t id, str_t path, str_t data) {
    wchar_t* target = wpath(path);
    if (!target) { file_event(id, FS_ERROR, "out of memory", 0, 0); return; }
    int exists = 0;
    HANDLE h = CreateFileW(target, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (h != INVALID_HANDLE_VALUE) {
        exists = 1;
        BY_HANDLE_FILE_INFORMATION info;
        if (GetFileInformationByHandle(h, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            CloseHandle(h);
            free(target);
            file_event(id, FS_ISDIR, "it is a directory", 0, 0);
            return;
        }
        DWORD need = GetFinalPathNameByHandleW(h, 0, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        wchar_t* final = need ? malloc(((size_t)need + 1) * sizeof *final) : 0;
        if (final && GetFinalPathNameByHandleW(h, final, need + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) - 1 < need) {
            free(target);
            target = final;
        } else {
            free(final);
        }
        CloseHandle(h);
    }
    mkdir_parents(target);
    size_t tn = wcslen(target);
    wchar_t* tmp = malloc((tn + 40) * sizeof *tmp);
    if (!tmp) { free(target); file_event(id, FS_ERROR, "out of memory", 0, 0); return; }
    swprintf(tmp, tn + 40, L"%ls.barb-tmp-%lu", target, (unsigned long)GetCurrentProcessId());
    DWORD err = 0;
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) {
        err = GetLastError();
    } else {
        if (!write_all(f, data.p, data.n)) err = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        if (!CloseHandle(f) && !err) err = GetLastError();
        if (!err) {
            int moved = exists && ReplaceFileW(target, tmp, 0, REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, 0, 0);
            if (!moved && !MoveFileExW(tmp, target, MOVEFILE_REPLACE_EXISTING)) err = GetLastError();
        }
        if (err) DeleteFileW(tmp);
    }
    free(tmp);
    free(target);
    if (err) { file_error(id, err); return; }
    file_event(id, FS_OK, 0, 0, 0);
}

// ---- timers and the rest ----

void ops_timer(int64_t id, int64_t ms) {
    op_t* o = alloc_op(K_TIMER, id);
    if (!o) return;
    o->deadline = ops_now_ms() + (ms > 0 ? ms : 0);
}

void ops_cancel(int64_t id) {
    op_t* o = find(id);
    if (!o) return;
    if (o->kind == K_TIMER) { release(o); return; }
    o->cancelled = 1;
    kill_op(o);
}

int ops_handles(void** h, int max) {
    int n = 0;
    overflow = 0;
    for (int i = 0; i < MAX_OPS; i++) {
        op_t* o = &ops[i];
        if (o->kind == K_FREE || o->kind == K_TIMER) continue;
        if (o->pipe && o->reading) { if (n < max) h[n++] = o->ov.hEvent; else overflow = 1; }
        if (o->proc && !o->exited) { if (n < max) h[n++] = o->proc; else overflow = 1; }
    }
    return n;
}

int ops_timeout_ms(void) {
    int64_t now = ops_now_ms(), best = -1;
    for (int i = 0; i < MAX_OPS; i++) {
        op_t* o = &ops[i];
        int64_t d = 0;
        if (o->kind == K_TIMER) d = o->deadline;
        else if (o->kind == K_PROC && o->deadline && !o->timed_out && !o->exited) d = o->deadline;
        if (d && (best < 0 || d - now < best)) best = d - now < 0 ? 0 : d - now;
    }
    if (overflow && (best < 0 || best > 50)) best = 50;   // what could not be waited on is checked often
    return best > INT_MAX ? INT_MAX : (int)best;
}

// A background task's job outlives the task, with anything the task left running (a server it
// started with &), until barb exits.
static void park(op_t* o) {
    if (!o->job) return;
    if (nparked == MAX_PARKED) { CloseHandle(parked[0]); memmove(parked, parked + 1, (MAX_PARKED - 1) * sizeof *parked); nparked--; }
    parked[nparked++] = o->job;
    o->job = 0;
}

static void finish(op_t* o) {
    // What a foreground command left running is no longer wanted, and must go before its output
    // can end: it holds the pipe open.
    if (!o->bg && o->job) TerminateJobObject(o->job, 1);
    drain_all(o);
    ev_t e;
    memset(&e, 0, sizeof e);
    e.id = o->id;
    e.ms = ops_now_ms() - o->started;
    int code = o->killed ? -1 : (int)o->code;

    if (o->kind == K_PROC) {
        if (!o->cancelled || o->bg) {             // an interrupted foreground command has no listener
            size_t n = 0;
            char* out = o->bg ? 0 : proc_output(o, &n);
            e.kind = EV_PROC;
            e.status = code;
            e.flags = (o->timed_out ? PROC_TIMEDOUT : 0) | (o->killed ? PROC_SIGNALED : 0);
            if (out) e.data = str_of(out, n);
            deliver(&e);
            free(out);
        }
        if (o->bg) park(o);
    } else if (o->kind == K_HTTP && !o->cancelled) {
        e.kind = EV_HTTP;
        char* body = o->body.p ? o->body.p : (char*)"";
        size_t n = o->body.n;
        if (code != 0) {
            // curl itself failed: what it printed (with --stderr -) is the reason. It also appends
            // the status line, "000" after a failure, which is not part of the reason.
            while (n && (body[n - 1] == '\n' || body[n - 1] == '\r' || body[n - 1] == ' ')) n--;
            if (n >= 3 && !memcmp(body + n - 3, "000", 3)) n -= 3;
            while (n && (body[n - 1] == '\n' || body[n - 1] == '\r' || body[n - 1] == ' ')) n--;
            char note[400];
            if (n) snprintf(note, sizeof note, "%.*s", n > 300 ? 300 : (int)n, body);
            else snprintf(note, sizeof note, "curl failed with exit code %d", code);
            e.status = 0;
            e.note = str_z(note);
            deliver(&e);
        } else {
            size_t cut = n;
            while (cut && body[cut - 1] != '\n') cut--;
            e.status = cut ? atoi(body + cut) : 0;
            e.data = str_of(body, cut ? cut - 1 : 0);
            deliver(&e);
        }
    }
    release(o);
}

void ops_service(void) {
    int64_t now = ops_now_ms();
    for (int i = 0; i < MAX_OPS; i++) {
        op_t* o = &ops[i];
        if (o->kind == K_FREE) continue;
        if (o->kind == K_TIMER) {
            if (now >= o->deadline) {
                ev_t e;
                memset(&e, 0, sizeof e);
                e.kind = EV_TIMER;
                e.id = o->id;
                release(o);
                deliver(&e);
            }
            continue;
        }
        pump(o);
        if (o->proc && !o->exited && WaitForSingleObject(o->proc, 0) == WAIT_OBJECT_0) {
            o->exited = 1;
            if (!GetExitCodeProcess(o->proc, &o->code)) o->code = (DWORD)-1;
        }
        if (o->kind == K_PROC && !o->exited && o->deadline && !o->timed_out && now >= o->deadline) {
            o->timed_out = 1;
            kill_op(o);
            continue;                // its exit is noticed the next time round
        }
        if (o->exited) finish(o);
    }
}

void ops_shutdown(void) {
    for (int i = 0; i < MAX_OPS; i++) {
        op_t* o = &ops[i];
        if (o->kind == K_FREE) continue;
        kill_op(o);
        if (o->proc) WaitForSingleObject(o->proc, 2000);
        release(o);
    }
    for (int i = 0; i < nparked; i++) { TerminateJobObject(parked[i], 1); CloseHandle(parked[i]); }
    nparked = 0;
}
