// The terminal's screen: see screen.h.
#include "screen.h"
#include <string.h>

enum { G_NONE, G_BANNER, G_USER, G_ASSIST, G_TOOL, G_NOTE, G_QUESTION };

// ---- writing ----

static void wr(sc_t* s, int fd, const char* p, size_t n) { if (n) s->cfg.write(s->cfg.ctx, fd, p, n); }
static void wrz(sc_t* s, int fd, const char* p) { wr(s, fd, p, strlen(p)); }
static void to1(void* ctx, const char* p, size_t n) { wr((sc_t*)ctx, 1, p, n); }
static void to2(void* ctx, const char* p, size_t n) { wr((sc_t*)ctx, 2, p, n); }

// Text from elsewhere (the model, a command, a file name), with its control characters made visible.
static void safe(sc_t* s, int fd, const char* p, size_t n) { md_safe(fd == 1 ? to1 : to2, s, p, n); }

static void flush_ed(sc_t* s) {
    if (s->ed.outn) wr(s, 2, s->ed.out, (size_t)s->ed.outn);
    s->ed.outn = 0;
}

static int vt(const sc_t* s, int fd) { return fd == 1 ? s->cfg.vt_out : s->cfg.vt_err; }
static int styled(const sc_t* s, int fd) { return s->cfg.mode != SC_PIPE && vt(s, fd); }

// ---- a small string builder, cut at its size ----

typedef struct { char* b; size_t n, cap; } buf_t;

static void bput(buf_t* b, const char* p, size_t n) {
    if (b->n + n > b->cap - 1) n = b->cap - 1 - b->n;
    memcpy(b->b + b->n, p, n);
    b->n += n;
    b->b[b->n] = 0;
}
static void bputz(buf_t* b, const char* p) { bput(b, p, strlen(p)); }
static void bputi(buf_t* b, int64_t v) {
    char t[24];
    int k = 0;
    if (v < 0) v = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) bput(b, &t[--k], 1);
}
static void bspaces(buf_t* b, int n) { while (n-- > 0) bput(b, " ", 1); }

// ---- characters ----

static uint32_t dec(const char* p, size_t n, int* len) {
    const unsigned char* u = (const unsigned char*)p;
    uint32_t c;
    int k;
    if (u[0] < 0x80) { *len = 1; return u[0]; }
    if ((u[0] & 0xE0) == 0xC0) { c = u[0] & 0x1Fu; k = 2; }
    else if ((u[0] & 0xF0) == 0xE0) { c = u[0] & 0x0Fu; k = 3; }
    else if ((u[0] & 0xF8) == 0xF0) { c = u[0] & 0x07u; k = 4; }
    else { *len = 1; return 0xFFFD; }
    if ((size_t)k > n) { *len = 1; return 0xFFFD; }
    for (int i = 1; i < k; i++) {
        if ((u[i] & 0xC0) != 0x80) { *len = 1; return 0xFFFD; }
        c = (c << 6) | (u[i] & 0x3Fu);
    }
    if ((k == 2 && c < 0x80) || (k == 3 && c < 0x800) || (k == 4 && (c < 0x10000 || c > 0x10FFFF)) ||
        (c >= 0xD800 && c <= 0xDFFF)) { *len = 1; return 0xFFFD; }
    *len = k;
    return c;
}

// Copy text for a one-line place (the activity line, the footer): control characters become
// spaces, C1 controls and broken UTF-8 '?'. At a newline it stops; 1 if it did (there is more).
static int clean(char* dst, size_t cap, const char* p, size_t n) {
    size_t k = 0;
    int more = 0;
    for (size_t i = 0; i < n;) {
        int len, out;
        uint32_t c = dec(p + i, n - i, &len);
        const char* src = p + i;
        out = len;
        if (c == '\n' || c == '\r') { more = i + 1 < n; break; }
        if (c < 0x20 || c == 0x7F) { src = " "; out = 1; }
        else if ((c >= 0x80 && c <= 0x9F) || (c == 0xFFFD && len == 1)) { src = "?"; out = 1; }
        if (k + (size_t)out > cap - 1) { more = 1; break; }
        memcpy(dst + k, src, (size_t)out);
        k += (size_t)out;
        i += (size_t)len;
    }
    dst[k] = 0;
    return more;
}

static int cols_of(const char* p) { return ed_text_cols(p, strlen(p)); }

// Put p (clean) in at most `maxc` columns: cut at the end with an ellipsis if it does not fit, or
// if `cut` says it was already cut. `head`: cut at the start instead, keeping the end.
static void fit(buf_t* b, const char* p, int maxc, int cut, int head, const char* ell) {
    int total = cols_of(p), ec = cols_of(ell);
    size_t n = strlen(p);
    if (maxc <= 0) return;
    if (total <= maxc && !cut) { bput(b, p, n); return; }
    if (maxc <= ec) { bputz(b, ell); return; }
    int room = maxc - ec;
    if (head) {                // the last `room` columns
        size_t i = 0;
        int c = total;
        while (i < n && c > room) {
            int len;
            c -= ed_cols(dec(p + i, n - i, &len));
            i += (size_t)len;
        }
        bputz(b, ell);
        bput(b, p + i, n - i);
        return;
    }
    size_t i = 0;
    int c = 0;
    while (i < n) {
        int len;
        int w = ed_cols(dec(p + i, n - i, &len));
        if (c + w > room) break;
        c += w;
        i += (size_t)len;
    }
    bput(b, p, i);
    bputz(b, ell);
}

static const char* ell(const sc_t* s) { return s->cfg.ascii ? "..." : "\xE2\x80\xA6"; }
static const char* dot(const sc_t* s) { return s->cfg.ascii ? " - " : " \xC2\xB7 "; }

// ---- making room ----

static void status_clear(sc_t* s) {
    if (!s->status_shown) return;
    wrz(s, 2, "\r\033[K");
    s->status_shown = 0;
    s->status[0] = 0;
}

// Before anything is written: the block and the status line go, and a question's line ends.
static void clear(sc_t* s) {
    if (s->ed.shown) { ed_hide(&s->ed); flush_ed(s); }
    status_clear(s);
    if (s->midline) { wrz(s, 2, "\n"); s->midline = 0; }
}

void sc_hide(sc_t* s) { clear(s); }

// A blank line between things of different kinds, on a terminal: before every reply, tool and
// question, before the user's line unless it follows another, and before a note that follows
// one of those. Never in PIPE mode, so what scripts read is as it was.
static void gap(sc_t* s, int fd, int what) {
    int want;
    switch (what) {
    case G_NOTE: want = s->last == G_ASSIST || s->last == G_TOOL || s->last == G_QUESTION; break;
    case G_USER: want = s->last != G_NONE && s->last != G_USER; break;
    default: want = s->last != G_NONE;
    }
    if (want && styled(s, fd)) wrz(s, fd, "\n");
    s->last = what;
}

// ---- the record ----

void sc_init(sc_t* s, const sc_cfg_t* cfg) {
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    if (s->cfg.width < 10) s->cfg.width = 80;
    const char* model = cfg->model ? cfg->model : "";
    clean(s->model, sizeof s->model, model, strlen(model));
    const char* cwd = cfg->cwd ? cfg->cwd : "";
    const char* home = cfg->home;
    size_t hl = home ? strlen(home) : 0;
    while (hl > 1 && (home[hl - 1] == '/' || home[hl - 1] == '\\')) hl--;
    if (hl && !strncmp(cwd, home, hl) && (cwd[hl] == 0 || cwd[hl] == '/' || cwd[hl] == '\\')) {
        s->cwd[0] = '~';
        clean(s->cwd + 1, sizeof s->cwd - 1, cwd + hl, strlen(cwd + hl));
    } else {
        clean(s->cwd, sizeof s->cwd, cwd, strlen(cwd));
    }
    s->cfg.model = s->cfg.cwd = s->cfg.home = 0;   // not kept: they may not outlive this call
    ed_init(&s->ed, s->cfg.width);
    if (s->cfg.mode == SC_REGION) ed_frame(&s->ed, 2, 1);
    s->ed.outn = 0;
    md_init(&s->md, to1, s, s->cfg.width, s->cfg.ascii);
}

void sc_raw(sc_t* s, int fd, const char* text) {
    clear(s);
    wrz(s, fd, text);
}

void sc_banner(sc_t* s, const char* version) {
    clear(s);
    if (!styled(s, 2)) {
        wrz(s, 2, "barb ");
        wrz(s, 2, version);
        wrz(s, 2, ", model ");
        wrz(s, 2, s->model);
        wrz(s, 2, ", in ");
        wrz(s, 2, s->cwd);
        wrz(s, 2, "\n/help for commands; Ctrl-D to exit.\n");
    } else {
        wrz(s, 2, "\033[1mbarb\033[0m\033[2m ");
        wrz(s, 2, version);
        wrz(s, 2, dot(s));
        wrz(s, 2, s->model);
        wrz(s, 2, dot(s));
        wrz(s, 2, s->cwd);
        wrz(s, 2, "\033[0m\n\033[2m/help for commands");
        wrz(s, 2, dot(s));
        wrz(s, 2, "Ctrl-D to exit\033[0m\n");
    }
    s->last = G_BANNER;
}

// A reply: markdown on a terminal, as it came anywhere else.
static void assistant(sc_t* s, const char* p, size_t n) {
    gap(s, 1, G_ASSIST);
    if (!s->cfg.vt_out) { wr(s, 1, p, n); wr(s, 1, "\n", 1); return; }
    while (n && (p[n - 1] == '\n' || p[n - 1] == '\r')) n--;
    md_render(&s->md, p, n);
}

// A tool's follow-up ("  exit 0, 3 lines", "  error: ..."): under the tool line, hung off a corner.
static void follow_up(sc_t* s, int style, const char* p, size_t n) {
    for (size_t i = 0; i <= n;) {
        size_t e = i;
        while (e < n && p[e] != '\n') e++;
        size_t a = i;
        while (a < e && p[a] == ' ') a++;
        wrz(s, 2, i ? "    " : (s->cfg.ascii ? "\033[2m  - " : "\033[2m  \xE2\x94\x94 "));
        if (!i) wrz(s, 2, style == SAY_ERROR ? "\033[0m\033[31m" : "");
        else wrz(s, 2, style == SAY_ERROR ? "\033[31m" : "\033[2m");
        safe(s, 2, p + a, e - a);
        wrz(s, 2, "\033[0m\n");
        if (e >= n) break;
        i = e + 1;
    }
}

void sc_say(sc_t* s, int style, const char* p, size_t n) {
    clear(s);
    if (style == SAY_ASSISTANT) { assistant(s, p, n); return; }
    int follow = n && p[0] == ' ';
    if (!styled(s, 2)) {
        if (!follow) s->last = style == SAY_TOOL ? G_TOOL : G_NOTE;
        if (style == SAY_TOOL && !follow) wrz(s, 2, "> ");
        if (vt(s, 2)) safe(s, 2, p, n); else wr(s, 2, p, n);
        wrz(s, 2, "\n");
        return;
    }
    if (follow) { follow_up(s, style, p, n); return; }
    if (style == SAY_TOOL) {   // "bash ls src": a dot, the tool's name in bold, what it does
        gap(s, 2, G_TOOL);
        size_t w = 0;
        while (w < n && p[w] != ' ' && p[w] != '\n') w++;
        wrz(s, 2, s->cfg.ascii ? "\033[32m*\033[0m \033[1m" : "\033[32m\xE2\x97\x8F\033[0m \033[1m");
        safe(s, 2, p, w);
        wrz(s, 2, "\033[0m");
        safe(s, 2, p + w, n - w);
        wrz(s, 2, "\n");
        return;
    }
    gap(s, 2, G_NOTE);
    wrz(s, 2, style == SAY_ERROR ? "\033[31m" : "\033[2m");
    safe(s, 2, p, n);
    wrz(s, 2, "\033[0m\n");
}

void sc_question(sc_t* s, const char* tool, size_t tn, const char* desc, size_t dn) {
    clear(s);
    if (!styled(s, 2)) {
        wrz(s, 2, "\n");
        wr(s, 2, tool, tn);
        wrz(s, 2, " wants to:\n");
        if (vt(s, 2)) safe(s, 2, desc, dn); else wr(s, 2, desc, dn);
        wrz(s, 2, "\nAllow? [y]es / [n]o / [a]lways this session: ");
    } else {
        gap(s, 2, G_QUESTION);
        wrz(s, 2, s->cfg.ascii ? "\033[33m*\033[0m \033[1m" : "\033[33m\xE2\x97\x8F\033[0m \033[1m");
        safe(s, 2, tool, tn);
        wrz(s, 2, " wants to:\033[0m\n");
        for (size_t i = 0; i < dn;) {   // each line indented; a diff's added and removed lines coloured
            size_t e = i;
            while (e < dn && desc[e] != '\n') e++;
            const char* on = e - i >= 2 && desc[i] == '+' && desc[i + 1] == ' ' ? "\033[32m"
                           : e - i >= 2 && desc[i] == '-' && desc[i + 1] == ' ' ? "\033[31m" : "";
            wrz(s, 2, "  ");
            wrz(s, 2, on);
            safe(s, 2, desc + i, e - i);
            wrz(s, 2, *on ? "\033[0m\n" : "\n");
            i = e + 1;
        }
        wrz(s, 2, "\033[1mAllow?\033[0m [y]es / [n]o / [a]lways this session: ");
    }
    s->midline = 1;
    s->last = G_QUESTION;
}

void sc_answer(sc_t* s, int ans) {
    const char* word = ans == ANS_YES ? "yes" : ans == ANS_ALWAYS ? "always" : "no";
    if (styled(s, 2)) wrz(s, 2, ans == ANS_NO ? "\033[31m" : "\033[32m");
    wrz(s, 2, word);
    wrz(s, 2, styled(s, 2) ? "\033[0m\n" : "\n");
    s->midline = 0;
}

// ---- what is going on ----

static int find(const int64_t* v, int n, int64_t id) {
    for (int i = 0; i < n; i++) if (v[i] == id) return i;
    return -1;
}

static void forget(sc_t* s, int64_t id) {
    if (!id) return;
    if (s->http_id == id) s->http_id = 0;
    if (s->fg_id == id) s->fg_id = 0;
    if (s->retry_id == id) s->retry_id = 0;
    int i = find(s->task, s->ntasks, id);
    if (i >= 0) s->task[i] = s->task[--s->ntasks];
    i = find(s->timer, s->ntimers, id);
    if (i >= 0) { s->ntimers--; s->timer[i] = s->timer[s->ntimers]; s->timer_due[i] = s->timer_due[s->ntimers]; }
}

void sc_event(sc_t* s, const ev_t* ev) {
    s->http_failed = 0;
    switch (ev->kind) {
    case EV_HTTP:
        if (ev->id && ev->id == s->http_id) {
            int st = ev->status;
            s->http_failed = st == 0 || st == 408 || st == 429 || (st >= 500 && st <= 599);
        }
        forget(s, ev->id);
        break;
    case EV_PROC: case EV_TIMER: forget(s, ev->id); break;
    default: break;
    }
}

void sc_action(sc_t* s, const act_t* a, int64_t now) {
    switch (a->kind) {
    case ACT_SAY:
        if (a->flag == SAY_USAGE) {   // REGION shows it in the footer
            s->tokens = a->n;
            if (s->cfg.mode != SC_REGION) sc_say(s, SAY_INFO, a->a.p, a->a.n);
            return;
        }
        sc_say(s, a->flag, a->a.p, a->a.n);
        break;
    case ACT_HTTP: s->http_id = a->id; s->http_since = now; break;
    case ACT_SPAWN:
        if (a->flag == 1) { if (s->ntasks < SC_OPS) s->task[s->ntasks++] = a->id; break; }
        s->fg_id = a->id;
        s->fg_since = now;
        {
            size_t k = 0;
            while (k < a->a.n && (a->a.p[k] == ' ' || a->a.p[k] == '\n')) k++;
            s->fg_more = clean(s->fg_cmd, sizeof s->fg_cmd, a->a.p + k, a->a.n - k);
        }
        break;
    case ACT_TIMER:
        if (s->http_failed) { s->retry_id = a->id; s->retry_due = now + a->n; s->http_failed = 0; break; }
        if (s->ntimers < SC_OPS) { s->timer[s->ntimers] = a->id; s->timer_due[s->ntimers] = now + a->n; s->ntimers++; }
        break;
    case ACT_CANCEL: forget(s, a->id); break;
    default: break;
    }
}

// "12s", "1m 05s", "1h 02m".
static void elapsed(buf_t* b, int64_t ms) {
    int64_t t = ms < 0 ? 0 : ms / 1000;
    if (t < 60) { bputi(b, t); bputz(b, "s"); return; }
    int64_t big = t < 3600 ? t / 60 : t / 3600, small = t < 3600 ? t % 60 : t / 60 % 60;
    bputi(b, big);
    bputz(b, t < 3600 ? "m " : "h ");
    if (small < 10) bputz(b, "0");
    bputi(b, small);
    bputz(b, t < 3600 ? "s" : "m");
}

static void spinner(const sc_t* s, buf_t* b, int64_t now, const char* colour) {
    static const char* const braille[10] = {
        "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC",
        "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F" };
    static const char plain[] = "-\\|/";
    int64_t t = now < 0 ? 0 : now;
    bputz(b, colour);
    if (s->cfg.fancy) bputz(b, braille[t / 120 % 10]);
    else bput(b, &plain[t / 250 % 4], 1);
    bputz(b, "\033[0m ");
}

// The activity line. 1 if it has a spinner (it changes with time).
static int activity(sc_t* s, buf_t* b, int64_t now, int busy) {
    int width = s->ed.width - 1;
    if (s->retry_id) {
        spinner(s, b, now, "\033[33m");
        bputz(b, "the request failed; retrying in ");
        int64_t left = s->retry_due - now;
        elapsed(b, left > 0 ? left + 999 : 0);
        return 1;
    }
    if (s->http_id || s->fg_id) {
        int64_t since = s->http_id ? s->http_since : s->fg_since;
        char t[32] = "";
        buf_t tb = { t, 0, sizeof t };
        if (now - since >= 1000) { bputz(&tb, dot(s)); elapsed(&tb, now - since); }
        spinner(s, b, now, "\033[35m");
        if (s->http_id) bputz(b, "waiting for the model");
        else {
            bputz(b, "running ");
            fit(b, s->fg_cmd, width - 10 - cols_of(t), s->fg_more, 0, ell(s));
        }
        bputz(b, "\033[2m");
        bputz(b, t);
        bputz(b, "\033[0m");
        return 1;
    }
    if (busy) { spinner(s, b, now, "\033[35m"); bputz(b, "working"); return 1; }
    if (!s->ntasks && !s->ntimers) return 0;
    bputz(b, s->cfg.ascii ? "\033[2mo " : "\033[2m\xE2\x97\x8B ");
    if (s->ntasks == 1) { bputz(b, "task "); bputi(b, s->task[0]); bputz(b, " running"); }
    else if (s->ntasks) { bputi(b, s->ntasks); bputz(b, " tasks running"); }
    if (s->ntimers) {
        int64_t due = s->timer_due[0];
        for (int i = 1; i < s->ntimers; i++) if (s->timer_due[i] < due) due = s->timer_due[i];
        char hm[16] = "";
        if (s->cfg.clock) s->cfg.clock(s->cfg.ctx, due, hm);
        hm[sizeof hm - 1] = 0;
        if (s->ntasks) bputz(b, ", ");
        bputz(b, s->ntimers == 1 ? "a wakeup" : "wakeups");
        if (*hm) { bputz(b, s->ntimers == 1 ? " at " : ", the next at "); bputz(b, hm); }
    }
    bputz(b, dot(s));
    bputz(b, s->ntasks + s->ntimers == 1 ? (s->ntimers ? "idle until then" : "idle until it ends") : "idle until one of them");
    bputz(b, "\033[0m");
    return 0;
}

static void tokens(buf_t* b, int64_t t) {
    if (t < 1000) { bputi(b, t); return; }
    int64_t unit = t < 1000000 ? 1000 : 1000000;
    const char* u = unit == 1000 ? "k" : "M";
    if (t / unit >= 100) { bputi(b, t / unit); bputz(b, u); return; }
    int64_t tenths = t * 10 / unit;
    bputi(b, tenths / 10);
    bputz(b, ".");
    bputi(b, tenths % 10);
    bputz(b, u);
}

static void footer(sc_t* s, buf_t* b, int busy) {
    char left[ED_DECOR];
    buf_t l = { left, 0, sizeof left };
    bputz(&l, "  ");
    bputz(&l, s->model);
    if (*s->cwd) { bputz(&l, dot(s)); fit(&l, s->cwd, 32, 0, 1, ell(s)); }
    if (s->tokens) { bputz(&l, dot(s)); tokens(&l, s->tokens); bputz(&l, " tokens"); }
    const char* hint = busy ? (s->ed.n ? "Enter queues it" : "Ctrl-C stops it") : "/help for commands";
    int lc = cols_of(left), hc = cols_of(hint), width = s->ed.width - 1;
    bputz(b, "\033[2m");
    bputz(b, left);
    if (lc + 3 + hc <= width) { bspaces(b, width - lc - hc); bputz(b, hint); }
}

static void rule(sc_t* s, buf_t* b) {
    int n = s->ed.width - 1;
    if (n > 160) n = 160;
    bputz(b, "\033[2m");
    for (int i = 0; i < n; i++) bputz(b, s->cfg.ascii ? "-" : "\xE2\x94\x80");
}

void sc_draw(sc_t* s, int64_t now, int busy, int prompt_ok, int status_ok) {
    char act[ED_DECOR];
    buf_t ab = { act, 0, sizeof act };
    act[0] = 0;
    s->animating = activity(s, &ab, now, busy);
    if (s->cfg.mode == SC_REGION && prompt_ok) {
        status_clear(s);
        if (s->midline) { wrz(s, 2, "\n"); s->midline = 0; }
        char row[ED_DECOR];
        buf_t rb = { row, 0, sizeof row };
        ed_set_prompt(&s->ed, busy ? "\033[2m> \033[0m" : "\033[1;35m> \033[0m", 2);
        ed_set_above(&s->ed, 0, act);
        rule(s, &rb);
        ed_set_above(&s->ed, 1, row);
        rb.n = 0;
        row[0] = 0;
        footer(s, &rb, busy);
        ed_set_below(&s->ed, row);
        if (s->ed.shown) ed_refresh(&s->ed); else ed_show(&s->ed);
        flush_ed(s);
        return;
    }
    if (s->ed.shown) { ed_hide(&s->ed); flush_ed(s); }
    if (!status_ok || s->cfg.mode == SC_PIPE || !s->cfg.vt_err || !s->animating || s->midline) {
        status_clear(s);
        s->animating = 0;      // nothing on the screen changes with time
        return;
    }
    // LITE: the activity alone, on the row the cursor is on, cut to fit.
    char line[ED_DECOR];
    buf_t lb = { line, 0, sizeof line };
    int c = 0, width = s->cfg.width - 1;
    bputz(&lb, "\r");
    for (size_t i = 0, n = strlen(act); i < n;) {
        if (act[i] == 0x1B) {
            size_t e = i + 1;
            while (e < n && !(act[e] >= 0x40 && act[e] <= 0x7E && e > i + 1)) e++;
            if (e >= n) break;
            bput(&lb, act + i, e + 1 - i);
            i = e + 1;
            continue;
        }
        int len;
        int w = ed_cols(dec(act + i, n - i, &len));
        if (c + w > width) break;
        bput(&lb, act + i, (size_t)len);
        c += w;
        i += (size_t)len;
    }
    bputz(&lb, "\033[0m\033[K");
    if (s->status_shown && !strcmp(line, s->status)) return;
    wrz(s, 2, line);
    memcpy(s->status, line, lb.n + 1);
    s->status_shown = 1;
}

int sc_next_ms(const sc_t* s, int64_t now) {
    if (!s->animating) return -1;
    int64_t period = s->cfg.fancy ? 120 : 250, t = now < 0 ? 0 : now;
    return (int)(period - t % period);
}

// ---- the line ----

void sc_key(sc_t* s, int key, uint32_t ch) {
    ed_key(&s->ed, key, ch);
    flush_ed(s);
}

const char* sc_take(sc_t* s, int* n) {
    // The prompt and the line stay as the record, with a blank line above them.
    clear(s);
    gap(s, 2, G_USER);
    ed_show(&s->ed);
    flush_ed(s);
    const char* line = ed_take(&s->ed, n);
    flush_ed(s);
    return line;
}

void sc_resize(sc_t* s, int width) {
    status_clear(s);
    if (width < 10) width = 10;
    ed_resize(&s->ed, width);
    flush_ed(s);
    s->cfg.width = width;
    s->md.width = width;
}
