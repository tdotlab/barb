// barb for Linux and macOS: the command line, and the loop that joins the agent to the system.
//
//   barb                       an interactive session (type a task; /help for commands)
//   barb -p "task"             do one task and exit; the answer goes to standard output
//
// This file is the only place that touches the terminal. The agent (src/core) decides what
// to do; ops.c does it; this loop waits for the next thing to happen (the user typing, a
// process or reply arriving, a timer) and hands it to the agent. When nothing is happening
// it sleeps in poll(), which is what "waiting costs nothing" means in practice.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "../core/agent.h"
#include "ops.h"

#define VERSION "0.1.0"
#define DEFAULT_MODEL "claude-sonnet-5-5"

// ---- events waiting for the agent ----

typedef struct qev {
    struct qev* next;
    ev_t ev;
    char* data;
    char* note;
} qev_t;

static qev_t *qhead, *qtail;

static void deliver(const ev_t* ev) {
    qev_t* q = calloc(1, sizeof *q);
    if (!q) return;
    q->ev = *ev;
    if (ev->data.p) { q->data = malloc(ev->data.n + 1); if (q->data) { memcpy(q->data, ev->data.p, ev->data.n); q->data[ev->data.n] = 0; } q->ev.data = str_of(q->data, ev->data.n); }
    if (ev->note.p) { q->note = strndup(ev->note.p, ev->note.n); q->ev.note = str_z(q->note); }
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
static int64_t awaiting;       // the question the user is being asked, if any
static int tty_in, tty_err;
static int oneshot;            // -p: no prompts, no reading the terminal
static int need_prompt;
static int selfpipe[2];

static void on_signal(int sig) {
    char c = sig == SIGINT ? 'i' : 't';
    ssize_t r = write(selfpipe[1], &c, 1);
    (void)r;
}

// ---- showing things to the user ----

static const char* color(int style) {
    if (!tty_err) return "";
    switch (style) {
    case SAY_TOOL: return "\033[36m";
    case SAY_INFO: return "\033[2m";
    case SAY_ERROR: return "\033[31m";
    }
    return "";
}

static void show(int style, str_t s) {
    if (style == SAY_ASSISTANT) {
        fwrite(s.p, 1, s.n, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        need_prompt = 1;
        return;
    }
    fflush(stdout);
    const char* on = color(style);
    const char* off = tty_err && *on ? "\033[0m" : "";
    // A tool's name line gets a marker; its follow-up lines (they start with a space) do not.
    fprintf(stderr, "%s%s%.*s%s\n", on, style == SAY_TOOL && s.n && s.p[0] != ' ' ? "> " : "", (int)s.n, s.p, off);
    need_prompt = 1;
}

// ---- carrying out actions ----

static void ask(const act_t* a) {
    if (oneshot || !tty_in) {
        fflush(stdout);
        fprintf(stderr, "%s(not allowed: there is no one to ask. Run with --yes to let barb write files and run commands unasked.)%s\n",
                color(SAY_ERROR), tty_err ? "\033[0m" : "");
        deliver_simple(EV_ANSWER, a->id, ANS_NO, 0);
        return;
    }
    fflush(stdout);
    fprintf(stderr, "\n%s%.*s wants to:%s\n%.*s\n", color(SAY_TOOL), (int)a->a.n, a->a.p, tty_err ? "\033[0m" : "", (int)a->b.n, a->b.p);
    fprintf(stderr, "Allow? [y]es / [n]o / [a]lways this session: ");
    awaiting = a->id;
}

static void exec(const act_t* a) {
    switch (a->kind) {
    case ACT_SAY: show(a->flag, a->a); break;
    case ACT_HTTP: ops_http(a->id, a->a, a->b); break;
    case ACT_READ: ops_read(a->id, a->a); break;
    case ACT_WRITE: ops_write(a->id, a->a, a->b); break;
    case ACT_SPAWN: ops_spawn(a->id, a->a, a->n, a->flag, a->c); break;
    case ACT_TIMER: ops_timer(a->id, a->n); break;
    case ACT_CANCEL:
        if (a->id == awaiting) { awaiting = 0; fputc('\n', stderr); }
        else ops_cancel(a->id);
        break;
    case ACT_ASK: ask(a); break;
    case ACT_DONE: done = 1; exit_code = (int)a->n; break;
    }
}

static void step(const ev_t* ev) {
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

// ---- the terminal ----

static char inbuf[65536];
static size_t inlen;
static int stdin_open = 1;

static void handle_line(char* line, size_t n) {
    while (n && (line[n - 1] == '\r')) n--;
    line[n] = 0;
    if (awaiting) {
        int64_t id = awaiting;
        awaiting = 0;
        int ans = ANS_NO;
        if (!strcasecmp(line, "y") || !strcasecmp(line, "yes")) ans = ANS_YES;
        else if (!strcasecmp(line, "a") || !strcasecmp(line, "always")) ans = ANS_ALWAYS;
        deliver_simple(EV_ANSWER, id, ans, 0);
        return;
    }
    deliver_simple(EV_INPUT, 0, 0, line);
}

static void read_stdin(void) {
    ssize_t r = read(0, inbuf + inlen, sizeof inbuf - inlen - 1);
    if (r < 0 && (errno == EINTR || errno == EAGAIN)) return;
    if (r <= 0) {
        stdin_open = 0;
        if (inlen) { inbuf[inlen] = 0; handle_line(inbuf, inlen); inlen = 0; }
        if (awaiting) { int64_t id = awaiting; awaiting = 0; deliver_simple(EV_ANSWER, id, ANS_NO, 0); }
        deliver_simple(EV_EOF, 0, 0, 0);
        if (tty_in) fputc('\n', stderr);
        return;
    }
    inlen += (size_t)r;
    size_t start = 0;
    for (size_t i = 0; i < inlen; i++) {
        if (inbuf[i] != '\n') continue;
        handle_line(inbuf + start, i - start);
        start = i + 1;
    }
    if (start) { memmove(inbuf, inbuf + start, inlen - start); inlen -= start; }
    if (inlen >= sizeof inbuf - 1) {     // one enormous line: hand over what there is
        inbuf[inlen] = 0;
        handle_line(inbuf, inlen);
        inlen = 0;
    }
}

static void read_signals(void) {
    char buf[32];
    ssize_t r = read(selfpipe[0], buf, sizeof buf);
    for (ssize_t i = 0; i < r; i++) {
        if (buf[i] == 'i') deliver_simple(EV_INTERRUPT, 0, 0, 0);
        else done = 1, exit_code = 143;     // SIGTERM
    }
}

static void loop(void) {
    for (;;) {
        drain_queue();
        if (done) return;
        if (need_prompt && tty_in && !oneshot && stdin_open && !awaiting) {
            fputs("> ", stderr);
            need_prompt = 0;
        }
        int nops = ops_nfds();
        struct pollfd* fds = calloc((size_t)nops + 2, sizeof *fds);
        if (!fds) { exit_code = 1; return; }
        int n = 0;
        fds[n].fd = selfpipe[0]; fds[n].events = POLLIN; n++;
        int stdin_slot = -1;
        if (stdin_open && !oneshot) { stdin_slot = n; fds[n].fd = 0; fds[n].events = POLLIN; n++; }
        ops_fill(fds + n);
        int r = poll(fds, (nfds_t)(n + nops), ops_timeout_ms());
        if (r > 0) {
            if (fds[0].revents) read_signals();
            if (stdin_slot >= 0 && fds[stdin_slot].revents) read_stdin();
        }
        free(fds);
        ops_service(0);
    }
}

// ---- starting up ----

static void usage(FILE* f) {
    fprintf(f,
        "barb %s: a coding agent for the terminal\n\n"
        "usage: barb [options]\n"
        "       barb -p TASK [options]\n\n"
        "  -p, --prompt TASK   do one task and exit (TASK of - reads it from standard input)\n"
        "  -m, --model NAME    the model (default %s, or $BARB_MODEL)\n"
        "  -y, --yes           do not ask before writing files or running commands\n"
        "  -C, --cwd DIR       work in DIR\n"
        "      --max-turns N   stop after N model calls without you typing (default 60)\n"
        "  -V, --version\n"
        "  -h, --help\n\n"
        "environment: ANTHROPIC_API_KEY (required), ANTHROPIC_BASE_URL, BARB_MODEL\n",
        VERSION, DEFAULT_MODEL);
}

static char* read_all_stdin(void) {
    size_t cap = 4096, n = 0;
    char* b = malloc(cap);
    ssize_t r;
    while (b && (r = read(0, b + n, cap - n - 1)) > 0) {
        n += (size_t)r;
        if (cap - n < 2) { cap *= 2; b = realloc(b, cap); }
    }
    if (b) b[n] = 0;
    return b;
}

int main(int argc, char** argv) {
    const char* prompt = 0;
    const char* model = getenv("BARB_MODEL");
    const char* cwd_arg = 0;
    int yes = 0, max_turns = 0;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        int has_val = i + 1 < argc;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("barb %s\n", VERSION); return 0; }
        else if (!strcmp(a, "-y") || !strcmp(a, "--yes")) yes = 1;
        else if ((!strcmp(a, "-p") || !strcmp(a, "--prompt")) && has_val) prompt = argv[++i];
        else if ((!strcmp(a, "-m") || !strcmp(a, "--model")) && has_val) model = argv[++i];
        else if ((!strcmp(a, "-C") || !strcmp(a, "--cwd")) && has_val) cwd_arg = argv[++i];
        else if (!strcmp(a, "--max-turns") && has_val) max_turns = atoi(argv[++i]);
        else { fprintf(stderr, "barb: unknown or incomplete option %s\n\n", a); usage(stderr); return 2; }
    }
    if (!model || !*model) model = DEFAULT_MODEL;

    const char* key = getenv("ANTHROPIC_API_KEY");
    if (!key || !*key) {
        fprintf(stderr, "barb: ANTHROPIC_API_KEY is not set.\n");
        return 2;
    }
    const char* base = getenv("ANTHROPIC_BASE_URL");
    if (!base || !*base) base = "https://api.anthropic.com";
    char* base_copy = strdup(base);
    size_t bl = strlen(base_copy);
    while (bl && base_copy[bl - 1] == '/') base_copy[--bl] = 0;
    if (!strncmp(base_copy, "http://", 7) && strncmp(base_copy + 7, "127.0.0.1", 9) &&
        strncmp(base_copy + 7, "localhost", 9) && strncmp(base_copy + 7, "[::1]", 5))
        fprintf(stderr, "barb: warning: ANTHROPIC_BASE_URL is plain http, so your API key will cross the network unencrypted.\n");

    if (cwd_arg && chdir(cwd_arg) != 0) {
        fprintf(stderr, "barb: cannot change to %s: %s\n", cwd_arg, strerror(errno));
        return 2;
    }
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
    char date[16];
    time_t now = time(0);
    strftime(date, sizeof date, "%Y-%m-%d", localtime(&now));
    struct utsname un;
    char plat[160] = "unknown";
    if (uname(&un) == 0) snprintf(plat, sizeof plat, "%s %s", un.sysname, un.machine);

    char* stdin_prompt = 0;
    if (prompt && !strcmp(prompt, "-")) { stdin_prompt = read_all_stdin(); prompt = stdin_prompt; }
    oneshot = prompt != 0;
    tty_in = isatty(0);
    tty_err = isatty(2);

    if (pipe(selfpipe) < 0) { perror("barb: pipe"); return 1; }
    fcntl(selfpipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(selfpipe[1], F_SETFD, FD_CLOEXEC);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGTERM, &sa, 0);
    signal(SIGPIPE, SIG_IGN);

    agent_cfg_t cfg = { str_z(model), 0, str_z(cwd), str_z(plat), str_z(date), yes, max_turns };
    agent = agent_new(&mem, &cfg);
    if (!agent) { fprintf(stderr, "barb: out of memory\n"); return 1; }
    ops_init(deliver, base_copy, key);

    if (!oneshot) {
        fprintf(stderr, "barb %s, model %s, in %s\n/help for commands; Ctrl-D to exit.\n", VERSION, model, cwd);
        need_prompt = 1;
    } else {
        deliver_simple(EV_INPUT, 0, 0, prompt);
        deliver_simple(EV_EOF, 0, 0, 0);
    }
    loop();

    ops_shutdown();
    agent_free(agent);
    free(stdin_prompt);
    free(base_copy);
    return exit_code;
}
