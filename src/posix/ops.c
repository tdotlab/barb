// Operations on Linux and macOS: see ops.h.
#define _GNU_SOURCE
#include "ops.h"
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_OPS 64
#define HEAD_KEEP (1 << 20)          // a process's output: the first megabyte...
#define TAIL_KEEP (1 << 20)          // ...and the last, with the middle dropped
#define HTTP_MAX (64 << 20)
#define READ_LIMIT (8 << 20)
#define KILL_GRACE_MS 2000

enum { K_FREE, K_PROC, K_HTTP, K_TIMER };

typedef struct {
    char* p;
    size_t n, cap;
} dbuf_t;

typedef struct {
    int kind;
    int64_t id;
    pid_t pid;
    int fd;                          // the read end of the child's output, or -1
    int bg;
    int exited, status;
    int cancelled, timed_out;
    int64_t started, deadline, kill_at;   // monotonic ms; 0 = none
    // process output
    dbuf_t head;
    char* ring;
    size_t ring_pos;
    uint64_t total;                  // bytes seen
    // http
    dbuf_t body;
    char* tmpfile;
} op_t;

static op_t ops[MAX_OPS];
static deliver_fn deliver;
static const char* g_base;
static const char* g_key;

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void ops_init(deliver_fn d, const char* base_url, const char* api_key) {
    deliver = d;
    g_base = base_url;
    g_key = api_key;
    for (int i = 0; i < MAX_OPS; i++) ops[i].fd = -1;
}

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
        ops[i].fd = -1;
        ops[i].started = now_ms();
        return &ops[i];
    }
    return 0;
}

static void release(op_t* o) {
    if (o->fd >= 0) close(o->fd);
    free(o->head.p);
    free(o->ring);
    free(o->body.p);
    if (o->tmpfile) { unlink(o->tmpfile); free(o->tmpfile); }
    memset(o, 0, sizeof *o);
    o->fd = -1;
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
    if (!o->ring) { o->ring = malloc(TAIL_KEEP); if (!o->ring) return; }
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
    char* out = malloc(head + strlen(note) + tail + 1);
    if (!out) { *len = 0; return 0; }
    size_t n = 0;
    if (head) { memcpy(out, o->head.p, head); n = head; }
    memcpy(out + n, note, strlen(note));
    n += strlen(note);
    for (size_t i = 0; i < tail; i++)
        out[n + i] = o->ring[(o->ring_pos - tail + i) % TAIL_KEEP];
    n += tail;
    out[n] = 0;
    *len = n;
    return out;
}

// ---- processes ----

static void mkdir_p(const char* path) {          // all the directories above `path`
    char* copy = strdup(path);
    if (!copy) return;
    for (char* p = copy + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(copy, 0777);
        *p = '/';
    }
    free(copy);
}

static void set_nonblock(int fd) {
    int f = fcntl(fd, F_GETFL);
    if (f >= 0) fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static void fail_start(int64_t id, int kind, const char* why) {
    ev_t e;
    memset(&e, 0, sizeof e);
    e.id = id;
    e.note = str_z(why);
    if (kind == K_HTTP) { e.kind = EV_HTTP; e.status = 0; }
    else { e.kind = EV_PROC; e.flags = PROC_FAILED; e.status = -1; }
    deliver(&e);
}

// Start `argv` in its own process group with stdin from `in_fd` (or /dev/null),
// stdout and stderr to `out_fd`. Returns the pid, or -1.
static pid_t start(char* const argv[], int in_fd, int out_fd) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        int null = in_fd < 0 ? open("/dev/null", O_RDONLY) : in_fd;
        dup2(null, 0);
        dup2(out_fd, 1);
        dup2(out_fd, 2);
        for (int fd = 3; fd < 256; fd++) close(fd);
        execvp(argv[0], argv);
        _exit(127);
    }
    setpgid(pid, pid);  // also from this side, so there is no race with a kill
    return pid;
}

void ops_spawn(int64_t id, str_t cmd, int64_t timeout_ms, int bg, str_t log) {
    op_t* o = alloc_op(K_PROC, id);
    if (!o) { fail_start(id, K_PROC, "too many operations running at once"); return; }
    o->bg = bg;
    char* argv[] = { "sh", "-c", (char*)cmd.p, 0 };
    if (bg) {
        char* lp = strndup(log.p, log.n);
        mkdir_p(lp);
        int fd = open(lp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            char why[256];
            snprintf(why, sizeof why, "cannot open the log file %s: %s", lp, strerror(errno));
            free(lp);
            release(o);
            fail_start(id, K_PROC, why);
            return;
        }
        free(lp);
        o->pid = start(argv, -1, fd);
        close(fd);
    } else {
        int p[2];
        if (pipe(p) < 0) { release(o); fail_start(id, K_PROC, strerror(errno)); return; }
        o->pid = start(argv, -1, p[1]);
        close(p[1]);
        o->fd = p[0];
        set_nonblock(o->fd);
        if (timeout_ms > 0) o->deadline = o->started + timeout_ms;
    }
    if (o->pid < 0) {
        char why[128];
        snprintf(why, sizeof why, "fork failed: %s", strerror(errno));
        release(o);
        fail_start(id, K_PROC, why);
    }
}

static void kill_group(op_t* o, int sig) {
    if (o->pid > 0) kill(-o->pid, sig);
}

// ---- HTTP, through curl ----
//
// The key and the headers go to curl on its standard input as a config file, never on the
// command line (where any user could read them from the process list). The body goes in a
// private temporary file. curl prints the body and then the status on its own last line.

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

void ops_http(int64_t id, str_t path, str_t body) {
    op_t* o = alloc_op(K_HTTP, id);
    if (!o) { fail_start(id, K_HTTP, "too many operations running at once"); return; }

    const char* tmpdir = getenv("TMPDIR");
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof tmpl, "%s/barb-body-XXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp");
    int bf = mkstemp(tmpl);
    if (bf < 0) { release(o); fail_start(id, K_HTTP, "cannot make a temporary file for the request"); return; }
    o->tmpfile = strdup(tmpl);
    fchmod(bf, 0600);
    size_t off = 0;
    while (off < body.n) {
        ssize_t w = write(bf, body.p + off, body.n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { close(bf); release(o); fail_start(id, K_HTTP, "cannot write the request to a temporary file"); return; }
        off += (size_t)w;
    }
    close(bf);

    char url[2048], keyhdr[1024], datafile[PATH_MAX + 2];
    snprintf(url, sizeof url, "%s%.*s", g_base, (int)path.n, path.p);
    snprintf(keyhdr, sizeof keyhdr, "x-api-key: %s", g_key);
    snprintf(datafile, sizeof datafile, "@%s", tmpl);
    dbuf_t cfg = { 0, 0, 0 };
    cfg_quoted(&cfg, "url", url);
    cfg_quoted(&cfg, "request", "POST");
    cfg_quoted(&cfg, "header", "content-type: application/json");
    cfg_quoted(&cfg, "header", "anthropic-version: 2023-06-01");
    cfg_quoted(&cfg, "header", keyhdr);
    cfg_quoted(&cfg, "data-binary", datafile);

    int in[2], out[2];
    if (pipe(in) < 0 || pipe(out) < 0) { free(cfg.p); release(o); fail_start(id, K_HTTP, strerror(errno)); return; }
    char* argv[] = { "curl", "-sS", "--stderr", "-", "--connect-timeout", "30", "--max-time", "1200",
                     "-K", "-", "-w", "\n%{http_code}", 0 };
    o->pid = start(argv, in[0], out[1]);
    close(in[0]);
    close(out[1]);
    o->fd = out[0];
    set_nonblock(o->fd);
    if (o->pid < 0) {
        close(in[1]);
        free(cfg.p);
        release(o);
        fail_start(id, K_HTTP, "cannot start curl");
        return;
    }
    // The config is small enough to sit in the pipe, so this does not block. (SIGPIPE is
    // ignored by the program, so a curl that died at once is an error return, not a signal.)
    ssize_t ignored = write(in[1], cfg.p, cfg.n);
    (void)ignored;
    close(in[1]);
    memset(cfg.p, 0, cfg.cap);      // the key was in there
    free(cfg.p);
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

static int fs_status(int err) {
    if (err == ENOENT || err == ENOTDIR) return FS_NOTFOUND;
    if (err == EACCES || err == EPERM) return FS_DENIED;
    if (err == EISDIR) return FS_ISDIR;
    return FS_ERROR;
}

void ops_read(int64_t id, str_t path) {
    int fd = open(path.p, O_RDONLY);
    if (fd < 0) { file_event(id, fs_status(errno), strerror(errno), 0, 0); return; }
    struct stat st;
    if (fstat(fd, &st) < 0) { file_event(id, FS_ERROR, strerror(errno), 0, 0); close(fd); return; }
    if (S_ISDIR(st.st_mode)) { file_event(id, FS_ISDIR, 0, 0, 0); close(fd); return; }
    if (S_ISREG(st.st_mode) && st.st_size > READ_LIMIT) {
        char note[96];
        snprintf(note, sizeof note, "%lld bytes, the limit is %d", (long long)st.st_size, READ_LIMIT);
        file_event(id, FS_TOOBIG, note, 0, 0);
        close(fd);
        return;
    }
    dbuf_t b = { 0, 0, 0 };
    char buf[65536];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        dbuf_add(&b, buf, (size_t)r, READ_LIMIT + 1);
        if (b.n > (size_t)READ_LIMIT) break;
    }
    int err = errno;
    close(fd);
    if (b.n > (size_t)READ_LIMIT) { free(b.p); file_event(id, FS_TOOBIG, "more than the size limit", 0, 0); return; }
    if (r < 0) { free(b.p); file_event(id, fs_status(err), strerror(err), 0, 0); return; }
    file_event(id, FS_OK, 0, b.p ? b.p : "", b.n);
    free(b.p);
}

// Write to a temporary file beside the target and rename it into place, so a crash
// or a full disk never leaves half a file. A symbolic link is followed, so the link stays.
void ops_write(int64_t id, str_t path, str_t data) {
    char target[PATH_MAX];
    if (!realpath(path.p, target)) snprintf(target, sizeof target, "%s", path.p);
    mkdir_p(target);
    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof tmp, "%s.barb-tmp-%d", target, (int)getpid());
    struct stat st;
    mode_t mode = stat(target, &st) == 0 ? (st.st_mode & 07777) : 0644;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { file_event(id, fs_status(errno), strerror(errno), 0, 0); return; }
    size_t off = 0;
    int err = 0;
    while (off < data.n) {
        ssize_t w = write(fd, data.p + off, data.n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { err = errno ? errno : EIO; break; }
        off += (size_t)w;
    }
    if (!err && fchmod(fd, mode) < 0) err = errno;
    if (close(fd) < 0 && !err) err = errno;
    if (!err && rename(tmp, target) < 0) err = errno;
    if (err) { unlink(tmp); file_event(id, fs_status(err), strerror(err), 0, 0); return; }
    file_event(id, FS_OK, 0, 0, 0);
}

// ---- timers and the rest ----

void ops_timer(int64_t id, int64_t ms) {
    op_t* o = alloc_op(K_TIMER, id);
    if (!o) return;
    o->deadline = now_ms() + (ms > 0 ? ms : 0);
}

void ops_cancel(int64_t id) {
    op_t* o = find(id);
    if (!o) return;
    if (o->kind == K_TIMER) { release(o); return; }
    o->cancelled = 1;
    kill_group(o, SIGTERM);
    o->kill_at = now_ms() + KILL_GRACE_MS;
}

int ops_nfds(void) {
    int n = 0;
    for (int i = 0; i < MAX_OPS; i++) if (ops[i].kind != K_FREE && ops[i].fd >= 0) n++;
    return n;
}

void ops_fill(struct pollfd* fds) {
    int n = 0;
    for (int i = 0; i < MAX_OPS; i++) {
        if (ops[i].kind == K_FREE || ops[i].fd < 0) continue;
        fds[n].fd = ops[i].fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
}

int ops_timeout_ms(void) {
    int64_t now = now_ms(), best = -1;
    int fg = 0, bg = 0;
    for (int i = 0; i < MAX_OPS; i++) {
        op_t* o = &ops[i];
        if (o->kind == K_FREE) continue;
        if (o->kind == K_PROC && o->bg) bg = 1;
        else if (o->kind == K_PROC || o->kind == K_HTTP) fg = 1;
        int64_t d = 0;
        if (o->kind == K_TIMER) d = o->deadline;
        else if (o->kind == K_PROC && o->deadline && !o->timed_out) d = o->deadline;
        if (o->kill_at && (!d || o->kill_at < d)) d = o->kill_at;
        if (d && (best < 0 || d - now < best)) best = d - now < 0 ? 0 : d - now;
    }
    // Children are noticed by polling (a SIGCHLD handler would be quicker, but this cannot miss
    // one). A command the user is waiting for is checked often; a background task, which only
    // has to wake the agent when it ends, rarely; and with nothing running there is no polling.
    if (fg && (best < 0 || best > 50)) best = 50;
    else if (bg && (best < 0 || best > 250)) best = 250;
    return best > INT_MAX ? INT_MAX : (int)best;
}

static void drain(op_t* o) {
    char buf[65536];
    while (o->fd >= 0) {
        ssize_t r = read(o->fd, buf, sizeof buf);
        if (r > 0) {
            if (o->kind == K_PROC) proc_add(o, buf, (size_t)r);
            else dbuf_add(&o->body, buf, (size_t)r, HTTP_MAX);
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        close(o->fd);   // end of output (or an error)
        o->fd = -1;
        return;
    }
}

static void finish(op_t* o) {
    drain(o);
    ev_t e;
    memset(&e, 0, sizeof e);
    e.id = o->id;
    e.ms = now_ms() - o->started;
    int sig = WIFSIGNALED(o->status);
    int code = WIFEXITED(o->status) ? WEXITSTATUS(o->status) : -1;

    if (o->kind == K_PROC) {
        if (!o->cancelled || o->bg) {             // an interrupted foreground command has no listener
            size_t n = 0;
            char* out = o->bg ? 0 : proc_output(o, &n);
            e.kind = EV_PROC;
            e.status = code;
            e.flags = (o->timed_out ? PROC_TIMEDOUT : 0) | (sig || o->cancelled ? PROC_SIGNALED : 0);
            if (out) e.data = str_of(out, n);
            deliver(&e);
            free(out);
        }
    } else if (o->kind == K_HTTP && !o->cancelled) {
        e.kind = EV_HTTP;
        char* body = o->body.p ? o->body.p : (char*)"";
        size_t n = o->body.n;
        if (code != 0) {
            // curl itself failed: what it printed (with --stderr -) is the reason. It also
            // appends the status line, "000" after a failure, which is not part of the reason.
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
            int status = cut ? atoi(body + cut) : 0;
            e.status = status;
            e.data = str_of(body, cut ? cut - 1 : 0);
            deliver(&e);
        }
    }
    release(o);
}

void ops_service(const struct pollfd* fds) {
    (void)fds;                        // every descriptor is simply tried: reads never block
    int64_t now = now_ms();

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
        drain(o);
        if (o->pid > 0 && !o->exited) {
            int st;
            pid_t r = waitpid(o->pid, &st, WNOHANG);
            if (r == o->pid) { o->exited = 1; o->status = st; }
        }
        if (o->kind == K_PROC && !o->exited && o->deadline && !o->timed_out && now >= o->deadline) {
            o->timed_out = 1;
            kill_group(o, SIGTERM);
            o->kill_at = now + KILL_GRACE_MS;
        }
        if (!o->exited && o->kill_at && now >= o->kill_at) {
            kill_group(o, SIGKILL);
            o->kill_at = 0;
        }
        if (o->exited) {
            // Anything the command left running (a server it started) is no longer wanted
            // unless this is a background task, which owns its group until it ends.
            if (!o->bg) kill_group(o, SIGKILL);
            finish(o);
        }
    }
}

void ops_shutdown(void) {
    for (int i = 0; i < MAX_OPS; i++) {
        op_t* o = &ops[i];
        if (o->kind == K_PROC || o->kind == K_HTTP) { kill_group(o, SIGKILL); waitpid(o->pid, 0, 0); }
        if (o->kind != K_FREE) release(o);
    }
}
