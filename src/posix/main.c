// barb for Linux and macOS: the command line, and the loop that joins the agent to the system.
//
//   barb                       an interactive session (type a task; /help for commands)
//   barb -p "task"             do one task and exit; the answer goes to standard output
//
// This file is the only place that touches the terminal. The agent (src/core) decides what
// to do; ops.c does it; this loop waits for the next thing to happen (the user typing, a
// process or reply arriving, a timer) and hands it to the agent. When nothing is happening
// it sleeps in poll(), which is what "waiting costs nothing" means in practice.
//
// An interactive session on a terminal puts it in raw mode: bytes arrive as they are typed, the
// key decoder (src/ui/keys.c) turns them into keys, and the screen (src/ui/screen.c) keeps the
// prompt, what is going on and a footer at the bottom, as on Windows. The terminal is asked to
// bracket pastes, so a pasted block of lines is one message. Anything else (a pipe, a file, a
// terminal with no use for escapes) is read as lines.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "../core/agent.h"
#include "../ui/keys.h"
#include "../ui/screen.h"
#include "ops.h"

#define VERSION "0.2.0"
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

// The terminal while a question waits for its one-key answer.
static struct termios cooked;
static int keys_mode;          // the terminal is in one-key mode

// The terminal when it is ours: raw keys, the key decoder, the screen and its line editor.
static int interactive;
static int raw_on;             // the terminal is in raw mode now
static sc_t scr;
static kd_t kd;
static int64_t kd_deadline;    // when a key left part-way is given up (a lone ESC: the Escape key)

static int64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void write_all(int fd, const char* s, size_t n) {
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

// What the screen writes is gathered and written at once, when the loop is about to wait: a frame
// of the bottom block is many small pieces, and a terminal may show each write as it comes.
static char pend[1 << 16];
static size_t pend_n;
static int pend_fd = 2;

static void flush_out(void) {
    if (pend_n) write_all(pend_fd, pend, pend_n);
    pend_n = 0;
}

static void sc_write(void* ctx, int fd, const char* s, size_t n) {
    (void)ctx;
    if (fd != pend_fd || pend_n + n > sizeof pend) { flush_out(); pend_fd = fd; }
    if (n > sizeof pend) { write_all(fd, s, n); return; }
    memcpy(pend + pend_n, s, n);
    pend_n += n;
}

// A time in now_ms's units (the wall clock's), as "14:32".
static void sc_clock(void* ctx, int64_t at, char* hhmm) {
    (void)ctx;
    time_t t = (time_t)(at / 1000);
    struct tm lt;
    hhmm[0] = 0;
    if (localtime_r(&t, &lt)) strftime(hhmm, 16, "%H:%M", &lt);
}

static void keys_on(void) {
    if (keys_mode || !tty_in) return;
    struct termios t = cooked;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcflush(0, TCIFLUSH);   // a line typed ahead must not answer the question
    if (tcsetattr(0, TCSANOW, &t) == 0) keys_mode = 1;
}

static void keys_off(void) {
    if (!keys_mode) return;
    tcsetattr(0, TCSANOW, &cooked);
    keys_mode = 0;
}

// Raw mode: no line discipline, no echo; Ctrl-C and Ctrl-Z stay signals (commands run in process
// groups of their own, so the terminal sends those to barb alone). Output is still processed, so
// "\n" is a new line. Ctrl-\ is turned off: its default is to dump core with the terminal raw.
static void term_raw(void) {
    struct termios t = cooked;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | IEXTEN);
    t.c_iflag &= ~(tcflag_t)(ICRNL | INLCR | IGNCR | IXON);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
#ifdef _POSIX_VDISABLE
    t.c_cc[VQUIT] = _POSIX_VDISABLE;
#endif
    if (tcsetattr(0, TCSANOW, &t) != 0) return;
    if (!raw_on) { flush_out(); write_all(2, "\033[?2004h", 8); }   // bracket pastes
    raw_on = 1;
}

static void term_restore(void) {
    if (!raw_on) return;
    flush_out();
    write_all(2, "\033[?2004l", 8);
    tcsetattr(0, TCSANOW, &cooked);
    raw_on = 0;
}

static int term_width(void) {
    struct winsize w;
    if (ioctl(2, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) return w.ws_col;
    return 80;
}

static void on_signal(int sig) {
    char c = sig == SIGINT ? 'i' : sig == SIGWINCH ? 'w' : sig == SIGTSTP ? 'z' : sig == SIGCONT ? 'c' : 't';
    int saved = errno;
    ssize_t r = write(selfpipe[1], &c, 1);
    (void)r;
    errno = saved;
}

// ---- carrying out actions ----

static int stdin_open = 1;
static void read_waiting_keys(void);

static const char not_allowed[] =
    "(not allowed: there is no one to ask. Run with --yes to let barb write files and run commands unasked.)";

static void ask(const act_t* a) {
    if (oneshot || !tty_in) {
        sc_say(&scr, SAY_ERROR, not_allowed, sizeof not_allowed - 1);
        deliver_simple(EV_ANSWER, a->id, ANS_NO, 0);
        return;
    }
    if (interactive) {
        read_waiting_keys();   // keys typed before the question go to the line, so they cannot answer it
        if (!stdin_open) { deliver_simple(EV_ANSWER, a->id, ANS_NO, 0); return; }
    } else {
        keys_on();   // before the question shows, so the flush cannot eat a quick answer
    }
    sc_question(&scr, a->a.p, a->a.n, a->b.p, a->b.n);
    awaiting = a->id;
}

// Every action is shown to the screen first (ACT_SAY is only that), then carried out.
static void exec(const act_t* a) {
    sc_action(&scr, a, now_ms());
    switch (a->kind) {
    case ACT_SAY: need_prompt = 1; break;
    case ACT_HTTP: ops_http(a->id, a->a, a->b); break;
    case ACT_READ: ops_read(a->id, a->a); break;
    case ACT_WRITE: ops_write(a->id, a->a, a->b); break;
    case ACT_SPAWN: ops_spawn(a->id, a->a, a->n, a->flag, a->c); break;
    case ACT_TIMER: ops_timer(a->id, a->n); break;
    case ACT_CANCEL:
        if (a->id == awaiting) { awaiting = 0; keys_off(); sc_hide(&scr); }
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

// ---- the terminal ----

static char inbuf[65536];
static size_t inlen;

static void handle_line(char* line, size_t n) {
    while (n && (line[n - 1] == '\r')) n--;
    line[n] = 0;
    if (awaiting) {   // answered on a line of its own: the terminal echoed it
        int64_t id = awaiting;
        awaiting = 0;
        scr.midline = 0;
        int ans = ANS_NO;
        if (!strcasecmp(line, "y") || !strcasecmp(line, "yes")) ans = ANS_YES;
        else if (!strcasecmp(line, "a") || !strcasecmp(line, "always")) ans = ANS_ALWAYS;
        deliver_simple(EV_ANSWER, id, ans, 0);
        return;
    }
    deliver_simple(EV_INPUT, 0, 0, line);
}

// A key pressed while a question waits in one-key mode: y, n or a answers it (Enter, Escape and
// Ctrl-D say no); anything else is ignored. 1 if it answered.
static int answer_key(char c) {
    int ans;
    switch (c) {
    case 'y': case 'Y': ans = ANS_YES; break;
    case 'a': case 'A': ans = ANS_ALWAYS; break;
    case 'n': case 'N': case '\r': case '\n': case 27: case 4: ans = ANS_NO; break;
    default: return 0;
    }
    int64_t id = awaiting;
    awaiting = 0;
    keys_off();
    sc_answer(&scr, ans);
    deliver_simple(EV_ANSWER, id, ans, 0);
    return 1;
}

// A pasted block of lines is one message. The terminal brackets a paste, so each Enter inside one
// adds its line here; a terminal that does not is caught the way Windows catches it, by lines
// that are followed at once by more text. Delivered with the line that ends it, or, when the paste
// itself ended with an Enter, when it ends.
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

static void deliver_paste(void) {
    deliver_simple(EV_INPUT, 0, 0, paste);
    paste_n = 0;
}

// The bytes being read, and the one being handled.
static unsigned char rbuf[4096];
static int rbuf_n, rbuf_i;

// Is typed text already waiting behind the Enter being handled? Part-way through a paste the next
// chunk may still be on its way, so it is given a moment.
static int more_text_waiting(void) {
    kd_t probe = kd;
    kd_key_t k;
    for (int i = rbuf_i + 1; i < rbuf_n; i++)
        if (kd_feed(&probe, rbuf[i], &k) && k.kind == KD_EDIT && k.ed == EK_CHAR) return 1;
    struct pollfd p = { 0, POLLIN, 0 };
    return poll(&p, 1, paste_n ? 40 : 0) > 0 && (p.revents & POLLIN);
}

static void enter(void) {
    int n;   // a paste while a question waits goes to the line, which is not on the screen then
    const char* line = awaiting ? ed_take(&scr.ed, &n) : sc_take(&scr, &n);
    if ((kd.pasting || more_text_waiting()) && paste_add(line, (size_t)n) && paste_add("\n", 1)) return;
    if (!paste_n) { deliver_simple(EV_INPUT, 0, 0, line); return; }
    if (n) paste_add(line, (size_t)n);
    deliver_paste();
}

// Ctrl-D on an empty line, or the terminal gone: the end of input.
static void end_input(void) {
    sc_hide(&scr);
    stdin_open = 0;
    if (paste_n) deliver_paste();
    deliver_simple(EV_EOF, 0, 0, 0);
}

// Ctrl-C: stop the work; at an idle prompt, throw away what was typed (and pasted) first.
static void interrupt(void) {
    if (interactive && !awaiting && !agent_busy(agent) && (scr.ed.n || paste_n || kd.pasting)) {
        sc_key(&scr, EK_CLEAR, 0);
        paste_n = 0;
        kd_init(&kd);
        return;
    }
    deliver_simple(EV_INTERRUPT, 0, 0, 0);
}

static void on_key(const kd_key_t* k) {
    // One key answers a question. Pasted text does not: it goes to the line, which waits.
    if (awaiting && !kd.pasting && k->kind != KD_PASTE_END) {
        switch (k->kind) {
        case KD_ENTER: answer_key('\r'); break;
        case KD_EOF: answer_key(4); break;
        case KD_INTR: interrupt(); break;
        case KD_EDIT:
            if (k->ed == EK_CLEAR) answer_key(27);
            else if (k->ed == EK_CHAR && k->ch < 0x80) answer_key((char)k->ch);
            break;
        }
        return;
    }
    switch (k->kind) {
    case KD_EDIT: sc_key(&scr, k->ed, k->ch); break;
    case KD_ENTER: enter(); break;
    case KD_EOF:   // on an empty line, the end; otherwise delete forward, as in a shell
        if (scr.ed.n) sc_key(&scr, EK_DEL, 0);
        else end_input();
        break;
    case KD_INTR: interrupt(); break;
    case KD_PASTE_END:   // a paste that ended with an Enter is sent; otherwise its last line waits
        if (paste_n && !scr.ed.n) deliver_paste();
        break;
    }
}

static void read_keys(void) {
    ssize_t r = read(0, rbuf, sizeof rbuf);
    if (r < 0 && (errno == EINTR || errno == EAGAIN)) return;
    if (r <= 0) {              // the terminal has gone
        if (awaiting) answer_key(4);
        end_input();
        return;
    }
    rbuf_n = (int)r;
    for (rbuf_i = 0; rbuf_i < rbuf_n && stdin_open && !done; rbuf_i++) {
        kd_key_t k;
        if (kd_feed(&kd, rbuf[rbuf_i], &k)) on_key(&k);
    }
    rbuf_n = rbuf_i = 0;
    // A sequence cut off mid-paste is most likely the end marker split across reads: wait longer.
    if (kd_pending(&kd)) kd_deadline = now_ms() + (kd.pasting ? 1000 : 30);
}

// Whatever the terminal has for us, without waiting.
static void read_waiting_keys(void) {
    struct pollfd p = { 0, POLLIN, 0 };
    while (stdin_open && !done && poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) read_keys();
}

static void resize(void) { sc_resize(&scr, term_width()); }

// Ctrl-Z: give the terminal back as it was, stop, and take it again when continued.
static void suspend(void) {
    sc_hide(&scr);
    term_restore();
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTSTP, &sa, 0);
    term_raw();
    resize();
}

static void read_stdin(void) {
    if (interactive) { read_keys(); return; }
    if (keys_mode && awaiting) {
        char c;
        ssize_t k = read(0, &c, 1);
        if (k == 1) answer_key(c);
        else if (k == 0 || (k < 0 && errno != EINTR && errno != EAGAIN)) answer_key('n');
        return;
    }
    ssize_t r = read(0, inbuf + inlen, sizeof inbuf - inlen - 1);
    if (r < 0 && (errno == EINTR || errno == EAGAIN)) return;
    if (r <= 0) {
        stdin_open = 0;
        if (inlen) { inbuf[inlen] = 0; handle_line(inbuf, inlen); inlen = 0; }
        if (awaiting) { int64_t id = awaiting; awaiting = 0; deliver_simple(EV_ANSWER, id, ANS_NO, 0); }
        deliver_simple(EV_EOF, 0, 0, 0);
        if (tty_in) sc_raw(&scr, 2, "\n");   // after the ^D the terminal echoed
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
        switch (buf[i]) {
        case 'i': interrupt(); break;
        case 'w': resize(); break;
        case 'z': suspend(); break;
        case 'c': term_raw(); break;        // continued after a stop barb did not see coming
        default: done = 1, exit_code = 143; // SIGTERM
        }
    }
}

// The bottom of the screen: the block with the prompt whenever the user can type, otherwise
// (a question waits, input has ended, the terminal is not ours) at most a status line.
static void draw(void) {
    int prompt_ok = interactive && !awaiting && stdin_open && !done;
    sc_draw(&scr, now_ms(), agent_busy(agent), prompt_ok, !awaiting && !done);
}

static void loop(void) {
    for (;;) {
        if (interactive && kd_pending(&kd) && now_ms() >= kd_deadline) {
            kd_key_t k;
            if (kd_timeout(&kd, &k)) on_key(&k);
        }
        drain_queue();
        if (done) return;
        // A terminal in line mode gets a plain prompt when the agent is ready for the next line.
        if (!interactive && need_prompt && tty_in && !oneshot && stdin_open && !awaiting && !agent_busy(agent)) {
            sc_raw(&scr, 2, "> ");
            need_prompt = 0;
        }
        draw();
        flush_out();
        int nops = ops_nfds();
        struct pollfd* fds = calloc((size_t)nops + 2, sizeof *fds);
        if (!fds) { exit_code = 1; return; }
        int n = 0;
        fds[n].fd = selfpipe[0]; fds[n].events = POLLIN; n++;
        int stdin_slot = -1;
        if (stdin_open && !oneshot) { stdin_slot = n; fds[n].fd = 0; fds[n].events = POLLIN; n++; }
        ops_fill(fds + n);
        int timeout = ops_timeout_ms(), anim = sc_next_ms(&scr, now_ms());   // the spinner's next frame
        if (anim >= 0 && (timeout < 0 || timeout > anim)) timeout = anim;
        if (interactive && kd_pending(&kd)) {
            int64_t left = kd_deadline - now_ms();
            if (left < 0) left = 0;
            if (timeout < 0 || timeout > left) timeout = (int)left;
        }
        int r = poll(fds, (nfds_t)(n + nops), timeout);
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
        "      --no-instructions  do not read AGENTS.md or CLAUDE.md from the working directory\n"
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
    int yes = 0, max_turns = 0, no_instructions = 0;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        int has_val = i + 1 < argc;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("barb %s\n", VERSION); return 0; }
        else if (!strcmp(a, "-y") || !strcmp(a, "--yes")) yes = 1;
        else if (!strcmp(a, "--no-instructions")) no_instructions = 1;
        else if ((!strcmp(a, "-p") || !strcmp(a, "--prompt")) && has_val) prompt = argv[++i];
        else if ((!strcmp(a, "-m") || !strcmp(a, "--model")) && has_val) model = argv[++i];
        else if ((!strcmp(a, "-C") || !strcmp(a, "--cwd")) && has_val) cwd_arg = argv[++i];
        else if (!strcmp(a, "--max-turns") && has_val) max_turns = atoi(argv[++i]);
        else { fprintf(stderr, "barb: unknown or incomplete option %s\n\n", a); usage(stderr); return 2; }
    }
    if (!model || !*model) model = DEFAULT_MODEL;

    // Take the key out of the environment: every command the model runs inherits it otherwise,
    // and `echo $ANTHROPIC_API_KEY` would show it. Only the platform layer holds it from here.
    const char* key_env = getenv("ANTHROPIC_API_KEY");
    char* key = key_env ? strdup(key_env) : 0;
    unsetenv("ANTHROPIC_API_KEY");
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
    if (tty_in && tcgetattr(0, &cooked) != 0) tty_in = 0;
    const char* term = getenv("TERM");
    if (!oneshot && tty_in && tty_err && !(term && !strcmp(term, "dumb"))) {
        term_raw();
        interactive = raw_on;
    }

    if (pipe(selfpipe) < 0) { perror("barb: pipe"); return 1; }
    fcntl(selfpipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(selfpipe[1], F_SETFD, FD_CLOEXEC);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGTERM, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    // The screen: the whole bottom block for an interactive session, a status line on a terminal
    // that is not ours to edit, plain text for pipes, files and dumb terminals. The Linux console's
    // font has no braille or bullets, and a terminal not set up for UTF-8 may show none of it.
    int dumb = term && !strcmp(term, "dumb");
    const char* loc = getenv("LC_ALL");
    if (!loc || !*loc) loc = getenv("LC_CTYPE");
    if (!loc || !*loc) loc = getenv("LANG");
    int utf8 = !loc || !*loc || strstr(loc, "UTF-8") || strstr(loc, "utf-8") || strstr(loc, "UTF8") || strstr(loc, "utf8") ||
               !strcmp(loc, "C") || !strcmp(loc, "POSIX");
    sc_cfg_t sc;
    memset(&sc, 0, sizeof sc);
    sc.vt_out = isatty(1) && !dumb;
    sc.vt_err = tty_err && !dumb;
    sc.mode = interactive ? SC_REGION : sc.vt_err ? SC_LITE : SC_PIPE;
    sc.ascii = !utf8 || (term && !strncmp(term, "linux", 5));
    sc.fancy = !sc.ascii;
    sc.width = tty_err ? term_width() : 80;
    sc.model = model;
    sc.cwd = cwd;
    sc.home = getenv("HOME");
    sc.write = sc_write;
    sc.clock = sc_clock;
    sc_init(&scr, &sc);
    if (interactive) {
        atexit(term_restore);
        sigaction(SIGWINCH, &sa, 0);
        sigaction(SIGTSTP, &sa, 0);
        sigaction(SIGCONT, &sa, 0);
    }

    agent_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.model = str_z(model);
    cfg.cwd = str_z(cwd);
    cfg.platform = str_z(plat);
    cfg.date = str_z(date);
    cfg.auto_approve = yes;
    cfg.max_turns = max_turns;
    cfg.instructions = !no_instructions;
    agent = agent_new(&mem, &cfg);
    if (!agent) { fprintf(stderr, "barb: out of memory\n"); return 1; }
    ops_init(deliver, base_copy, key);

    if (!oneshot) {
        sc_banner(&scr, VERSION);
        need_prompt = 1;
    } else {
        deliver_simple(EV_INPUT, 0, 0, prompt);
        deliver_simple(EV_EOF, 0, 0, 0);
    }
    loop();
    keys_off();
    sc_hide(&scr);
    flush_out();
    term_restore();

    ops_shutdown();
    agent_free(agent);
    free(stdin_prompt);
    free(base_copy);
    free(paste);
    return exit_code;
}
