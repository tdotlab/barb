// A fake world for the agent: a scripted model, a tiny in-memory file system and
// scripted processes. It carries out the agent's actions the way a platform would,
// feeds the answers back as events, and checks every request the agent would send
// against the rules the real API enforces.
#ifndef BARB_HARNESS_H
#define BARB_HARNESS_H

#include "check.h"
#include "../src/core/agent.h"
#include "../src/core/json.h"
#include "../src/core/compact.h"

#define J(...) #__VA_ARGS__

static int h_quiet;   // silence the notice about requests the script has no reply for

#define MAXQ 512
#define MAXOPS 128

typedef struct { ev_t ev; char* data; size_t len; char* note; } qev_t;
typedef struct { const char* match; const char* out; int code; int flags; } proc_script_t;   // a scripted command

typedef struct {
    int64_t id;
    actk_t kind;
    int bg;
    int64_t n;
    char* a;
    char* c;
    int cancelled;
} op_t;

typedef struct {
    agent_t* g;
    qev_t q[MAXQ];
    int qh, qt;

    char* replies[32];
    int status[32];
    int nrep, irep;
    int unexpected_requests;

    struct { char path[96]; char* data; size_t len; int is_dir; } files[16];
    int nfiles;

    proc_script_t procs[8];
    int nprocs;

    char* says[256];
    int style[256];
    int nsays;
    int64_t usage;             // n of the last SAY_USAGE
    char* bodies[64];
    int nbodies;
    op_t ops[MAXOPS];
    int nops;
    char* asks[16];
    int nasks;

    int ask_answer;   // -1: leave questions unanswered
    int hold_http, hold_fs, hold_proc;
    int done, exit_code;
} world_t;

static void w_push_n(world_t* w, ev_t ev, const char* data, size_t len, const char* note) {
    qev_t* e = &w->q[w->qt++ % MAXQ];
    e->ev = ev;
    e->data = 0;
    if (data) { e->data = malloc(len + 1); memcpy(e->data, data, len); e->data[len] = 0; }
    e->len = len;
    e->note = note ? strdup(note) : 0;
    e->ev.data = data ? str_of(e->data, len) : str_of(0, 0);
    e->ev.note = str_z(e->note);
}
static void w_push(world_t* w, ev_t ev, const char* data, const char* note) {
    w_push_n(w, ev, data, data ? strlen(data) : 0, note);
}

static void w_put_file_n(world_t* w, const char* path, const char* data, size_t len) {
    int i;
    for (i = 0; i < w->nfiles; i++) if (!strcmp(w->files[i].path, path)) break;
    if (i == w->nfiles) { strcpy(w->files[i].path, path); w->nfiles++; }
    else free(w->files[i].data);
    w->files[i].data = 0;
    if (data) { w->files[i].data = malloc(len + 1); memcpy(w->files[i].data, data, len); w->files[i].data[len] = 0; }
    w->files[i].len = len;
    w->files[i].is_dir = !data;
}
static void w_put_file(world_t* w, const char* path, const char* data) { w_put_file_n(w, path, data, data ? strlen(data) : 0); }

static const char* w_file(world_t* w, const char* path) {
    for (int i = 0; i < w->nfiles; i++) if (!strcmp(w->files[i].path, path)) return w->files[i].data;
    return 0;
}

static char* mk_reply(const char* stop, const char* content) {
    char* s = malloc(strlen(content) + 300);
    sprintf(s, "{\"id\":\"msg_x\",\"type\":\"message\",\"role\":\"assistant\",\"content\":%s,"
               "\"stop_reason\":\"%s\",\"usage\":{\"input_tokens\":100,\"output_tokens\":20}}", content, stop);
    return s;
}

static void w_reply(world_t* w, const char* body, int status) {
    w->replies[w->nrep] = strdup(body);
    w->status[w->nrep++] = status;
}
static void w_reply_text(world_t* w, const char* text) {
    char c[2048];
    snprintf(c, sizeof c, "[{\"type\":\"text\",\"text\":\"%s\"}]", text);
    char* r = mk_reply("end_turn", c);
    w_reply(w, r, 200);
    free(r);
}
static void w_reply_tools(world_t* w, const char* blocks) {   // blocks: tool_use objects separated by commas
    char* c = malloc(strlen(blocks) + 3);
    sprintf(c, "[%s]", blocks);
    char* r = mk_reply("tool_use", c);
    free(c);
    w_reply(w, r, 200);
    free(r);
}
#define TOOL(id, name, input) "{\"type\":\"tool_use\",\"id\":\"" id "\",\"name\":\"" name "\",\"input\":" input "}"

// ---- the rules the API enforces ----

static void check_body(const char* body) {
    arena_t a;
    arena_init(&a, &plain_mem);
    str_t b = str_z(body), msgs, v;
    CHECK(json_valid(b));
    CHECK(json_get(b, "model", &v));
    CHECK(json_get(b, "messages", &msgs));
    json_t j;
    json_open(&j, msgs);
    CHECK(json_next(&j) == JT_ARR);
    int idx = 0;
    char pending[16][64];   // tool_use ids the last assistant message left unanswered
    int npending = 0;
    for (;;) {
        jtok_t t = json_next(&j);
        if (t == JT_ARR_END) break;
        str_t m;
        CHECK(t == JT_OBJ);
        if (t != JT_OBJ || !json_capture(&j, t, &m)) break;
        str_t role = str_of(0, 0), content;
        CHECK(json_get(m, "role", &v) && json_as_str(&a, v, &role));
        CHECK(json_get(m, "content", &content));
        int assistant = str_eqz(role, "assistant");
        CHECK(assistant == (idx % 2 == 1));          // roles alternate, user first
        json_t cj;
        json_open(&cj, content);
        CHECK(json_next(&cj) == JT_ARR);
        int nblocks = 0, seen_non_result = 0;
        for (;;) {
            jtok_t bt = json_next(&cj);
            if (bt == JT_ARR_END) break;
            str_t blk, type = str_of(0, 0);
            if (bt != JT_OBJ || !json_capture(&cj, bt, &blk)) { CHECK(0); break; }
            nblocks++;
            CHECK(json_get(blk, "type", &v) && json_as_str(&a, v, &type));
            if (str_eqz(type, "text")) {
                str_t text = str_of(0, 0);
                CHECK(json_get(blk, "text", &v) && json_as_str(&a, v, &text));
                CHECK(text.n > 0);                    // no empty text blocks
                if (!assistant) seen_non_result = 1;
            } else if (str_eqz(type, "tool_use")) {
                str_t id = str_of(0, 0);
                CHECK(assistant);
                CHECK(json_get(blk, "id", &v) && json_as_str(&a, v, &id));
                CHECK(json_get(blk, "input", &v));
                if (npending < 16 && id.n < 64) { memcpy(pending[npending], id.p, id.n); pending[npending][id.n] = 0; npending++; }
            } else if (str_eqz(type, "tool_result")) {
                str_t id = str_of(0, 0);
                CHECK(!assistant);
                CHECK(!seen_non_result);              // results come first in the user message
                CHECK(json_get(blk, "tool_use_id", &v) && json_as_str(&a, v, &id));
                int found = 0;
                for (int i = 0; i < npending; i++)
                    if (strlen(pending[i]) == id.n && !memcmp(pending[i], id.p, id.n)) { found = 1; pending[i][0] = 0; pending[i][1] = 'x'; }
                CHECK(found);
            }
        }
        CHECK(nblocks > 0);                           // no empty messages
        if (!assistant && idx > 0) {
            for (int i = 0; i < npending; i++) CHECK(pending[i][0] == 0 && pending[i][1] == 'x');  // all answered
            npending = 0;
        }
        idx++;
    }
    // A request ends on a user message, or the API would be asked to continue its own turn.
    CHECK(idx % 2 == 1);
    arena_free_all(&a);
}

// ---- carrying out actions ----

static void w_act(world_t* w, const act_t* a) {
    // The contract in agent.h: every string in an action is followed by a NUL.
    CHECK(!a->a.p || a->a.p[a->a.n] == 0);
    CHECK(!a->b.p || a->b.p[a->b.n] == 0);
    CHECK(!a->c.p || a->c.p[a->c.n] == 0);
    switch (a->kind) {
    case ACT_SAY:
        if (w->nsays < 256) { w->says[w->nsays] = dupn(a->a.p ? a->a.p : "", a->a.n); w->style[w->nsays++] = a->flag; }
        if (a->flag == SAY_USAGE) w->usage = a->n;
        break;
    case ACT_HTTP: {
        char* body = dupn(a->b.p, a->b.n);
        if (w->nbodies < 64) w->bodies[w->nbodies++] = body;
        check_body(body);
        CHECK(str_eqz(a->a, "/v1/messages"));
        op_t* o = &w->ops[w->nops++];
        memset(o, 0, sizeof *o);
        o->id = a->id; o->kind = ACT_HTTP;
        if (!w->hold_http) {
            if (w->irep >= w->nrep) { w->unexpected_requests++; if (!h_quiet) printf("unexpected request #%d\n", w->nbodies); break; }
            ev_t e = { EV_HTTP, a->id, w->status[w->irep], 0, str_of(0, 0), str_of(0, 0), 0 };
            w_push(w, e, w->replies[w->irep], w->status[w->irep] == 0 ? "could not connect" : 0);
            w->irep++;
        }
        break;
    }
    case ACT_READ:
    case ACT_WRITE: {
        char path[128];
        snprintf(path, sizeof path, "%.*s", (int)a->a.n, a->a.p);
        op_t* o = &w->ops[w->nops++];
        memset(o, 0, sizeof *o);
        o->id = a->id; o->kind = a->kind; o->a = strdup(path);
        if (a->kind == ACT_WRITE) {
            o->c = dupn(a->b.p, a->b.n);
            // A saved conversation must load again, and carries no cache marks.
            if (a->b.n > 15 && !memcmp(a->b.p, "{\"barb_session\"", 15)) {
                arena_t sa;
                arena_init(&sa, &plain_mem);
                session_t s;
                CHECK(api_load_session(&sa, a->b, &s));
                CHECK(str_find(a->b, str_z("cache_control")) == STR_NONE);
                arena_free_all(&sa);
            }
        }
        if (w->hold_fs) break;
        ev_t e = { EV_FILE, a->id, FS_OK, 0, str_of(0, 0), str_of(0, 0), 0 };
        if (a->kind == ACT_WRITE) {
            w_put_file_n(w, path, a->b.p ? a->b.p : "", a->b.n);
            w_push(w, e, 0, 0);
        } else {
            const char* d = w_file(w, path);
            size_t dlen = 0;
            int dir = 0;
            for (int i = 0; i < w->nfiles; i++) if (!strcmp(w->files[i].path, path)) { dlen = w->files[i].len; dir = w->files[i].is_dir; }
            if (dir) { e.status = FS_ISDIR; w_push(w, e, 0, 0); }
            else if (!d) { e.status = FS_NOTFOUND; w_push(w, e, 0, 0); }
            else w_push_n(w, e, d, dlen, 0);
        }
        break;
    }
    case ACT_SPAWN: {
        op_t* o = &w->ops[w->nops++];
        memset(o, 0, sizeof *o);
        o->id = a->id; o->kind = ACT_SPAWN; o->bg = a->flag; o->n = a->n;
        o->a = dupn(a->a.p, a->a.n);
        o->c = dupn(a->c.p ? a->c.p : "", a->c.n);
        if (!a->flag && !w->hold_proc) {
            ev_t e = { EV_PROC, a->id, 0, 0, str_of(0, 0), str_of(0, 0), 10 };
            const char* out = "";
            for (int i = 0; i < w->nprocs; i++)
                if (strstr(o->a, w->procs[i].match)) { out = w->procs[i].out; e.status = w->procs[i].code; e.flags = w->procs[i].flags; break; }
            w_push(w, e, out, (e.flags & PROC_FAILED) ? "no such shell" : 0);
        }
        break;
    }
    case ACT_TIMER: {
        op_t* o = &w->ops[w->nops++];
        memset(o, 0, sizeof *o);
        o->id = a->id; o->kind = ACT_TIMER; o->n = a->n;
        break;
    }
    case ACT_CANCEL:
        for (int i = 0; i < w->nops; i++) if (w->ops[i].id == a->id) w->ops[i].cancelled = 1;
        break;
    case ACT_ASK: {
        if (w->nasks < 16) w->asks[w->nasks++] = dupn(a->b.p, a->b.n);
        op_t* o = &w->ops[w->nops++];
        memset(o, 0, sizeof *o);
        o->id = a->id; o->kind = ACT_ASK;
        if (w->ask_answer >= 0) {
            ev_t e = { EV_ANSWER, a->id, w->ask_answer, 0, str_of(0, 0), str_of(0, 0), 0 };
            w_push(w, e, 0, 0);
        }
        break;
    }
    case ACT_DONE:
        w->done = 1;
        w->exit_code = (int)a->n;
        break;
    }
}

static void w_step(world_t* w, ev_t ev) {
    size_t n;
    const act_t* acts = agent_step(w->g, &ev, &n);
    for (size_t i = 0; i < n; i++) w_act(w, &acts[i]);
}

// Process queued events until nothing is left (held operations stay unanswered).
static void w_run(world_t* w) {
    int guard = 0;
    while (w->qh != w->qt && guard++ < 2000) {
        qev_t* e = &w->q[w->qh++ % MAXQ];
        w_step(w, e->ev);
        free(e->data); free(e->note);
    }
    CHECK(guard < 2000);
}

static void w_event(world_t* w, evk_t kind, int64_t id, int status, int flags, const char* data) {
    ev_t e = { kind, id, status, flags, str_of(0, 0), str_of(0, 0), 5000 };
    w_push(w, e, data, 0);
    w_run(w);
}
static void w_input(world_t* w, const char* line) { w_event(w, EV_INPUT, 0, 0, 0, line); }

static int64_t w_last_op(world_t* w, actk_t kind) {
    for (int i = w->nops - 1; i >= 0; i--) if (w->ops[i].kind == kind) return w->ops[i].id;
    return 0;
}
static op_t* w_op(world_t* w, int64_t id) {
    for (int i = 0; i < w->nops; i++) if (w->ops[i].id == id) return &w->ops[i];
    return 0;
}

typedef struct {
    int auto_approve, max_turns, instructions, verbose;
    const char* session;    // 0: not saved
    int64_t compact_at;
} w_opts_t;

static world_t* w_new_opts(w_opts_t o) {
    world_t* w = calloc(1, sizeof *w);
    agent_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.model = str_z("claude-test");
    cfg.cwd = str_z("/work");
    cfg.platform = str_z("TestOS");
    cfg.date = str_z("2026-11-21");
    cfg.auto_approve = o.auto_approve;
    cfg.max_turns = o.max_turns;
    cfg.instructions = o.instructions;
    cfg.verbose = o.verbose;
    cfg.session = str_z(o.session ? o.session : "");
    cfg.compact_at = o.compact_at;
    w->g = agent_new(&test_mem, &cfg);
    w->ask_answer = -1;
    return w;
}
static world_t* w_new_cfg(int auto_approve, int max_turns, int instructions) {
    w_opts_t o = { auto_approve, max_turns, instructions, 0, 0, 0 };
    return w_new_opts(o);
}

// Carry out what agent_resume returned, then what follows from it.
static int w_resume(world_t* w, const char* data, int show) {
    const act_t* acts;
    size_t n;
    int ok = agent_resume(w->g, str_z(data), show, &acts, &n);
    for (size_t i = 0; i < n; i++) w_act(w, &acts[i]);
    w_run(w);
    return ok;
}
static world_t* w_new_turns(int auto_approve, int max_turns) { return w_new_cfg(auto_approve, max_turns, 0); }
static world_t* w_new(int auto_approve) { return w_new_turns(auto_approve, 0); }

static void w_free(world_t* w) {
    if (w->g) agent_free(w->g);
    for (int i = 0; i < w->nrep; i++) free(w->replies[i]);
    for (int i = 0; i < w->nfiles; i++) free(w->files[i].data);
    for (int i = 0; i < w->nsays; i++) free(w->says[i]);
    for (int i = 0; i < w->nbodies; i++) free(w->bodies[i]);
    for (int i = 0; i < w->nops; i++) { free(w->ops[i].a); free(w->ops[i].c); }
    for (int i = 0; i < w->nasks; i++) free(w->asks[i]);
    while (w->qh != w->qt) { qev_t* e = &w->q[w->qh++ % MAXQ]; free(e->data); free(e->note); }
    free(w);
}

// ---- looking at what happened ----

static int w_said(world_t* w, const char* needle) {
    for (int i = 0; i < w->nsays; i++) if (strstr(w->says[i], needle)) return 1;
    return 0;
}

// The text of the tool_result for `id` in the most recent request, or 0.
static char* w_result(world_t* w, const char* id, int* is_error) {
    if (!w->nbodies) return 0;
    arena_t a;
    arena_init(&a, &plain_mem);
    str_t body = str_z(w->bodies[w->nbodies - 1]), msgs, v;
    char* out = 0;
    if (json_get(body, "messages", &msgs)) {
        json_t j;
        json_open(&j, msgs);
        json_next(&j);
        for (;;) {
            jtok_t t = json_next(&j);
            str_t m, content;
            if (t != JT_OBJ || !json_capture(&j, t, &m)) break;
            if (!json_get(m, "content", &content)) continue;
            json_t cj;
            json_open(&cj, content);
            json_next(&cj);
            for (;;) {
                jtok_t bt = json_next(&cj);
                str_t blk, rid, text;
                if (bt != JT_OBJ || !json_capture(&cj, bt, &blk)) break;
                if (!json_get(blk, "tool_use_id", &v) || !json_as_str(&a, v, &rid)) continue;
                if (!(rid.n == strlen(id) && !memcmp(rid.p, id, rid.n))) continue;
                if (json_get(blk, "content", &v) && json_as_str(&a, v, &text)) out = dupn(text.p, text.n);
                if (is_error) *is_error = json_get(blk, "is_error", &v);
            }
        }
    }
    arena_free_all(&a);
    return out;
}

static char* w_last_user_text(world_t* w) {   // all the text blocks of the last user message, joined by '|'
    arena_t a;
    arena_init(&a, &plain_mem);
    str_t body = str_z(w->bodies[w->nbodies - 1]), msgs, v;
    char* out = strdup("");
    if (json_get(body, "messages", &msgs)) {
        json_t j;
        json_open(&j, msgs);
        json_next(&j);
        str_t last = str_of(0, 0);
        for (;;) {
            jtok_t t = json_next(&j);
            str_t m;
            if (t != JT_OBJ || !json_capture(&j, t, &m)) break;
            last = m;
        }
        str_t content;
        if (last.p && json_get(last, "content", &content)) {
            json_t cj;
            json_open(&cj, content);
            json_next(&cj);
            for (;;) {
                jtok_t bt = json_next(&cj);
                str_t blk, type, text;
                if (bt != JT_OBJ || !json_capture(&cj, bt, &blk)) break;
                if (json_get(blk, "type", &v) && json_as_str(&a, v, &type) && str_eqz(type, "text") &&
                    json_get(blk, "text", &v) && json_as_str(&a, v, &text)) {
                    out = realloc(out, strlen(out) + text.n + 2);
                    if (*out) strcat(out, "|");
                    strncat(out, text.p, text.n);
                }
            }
        }
    }
    arena_free_all(&a);
    return out;
}

#define CHECK_RESULT(w, id, want) do { char* r_ = w_result((w), (id), 0); CHECK_STR(str_z(r_ ? r_ : "(none)"), (want)); free(r_); } while (0)
#define CHECK_RESULT_HAS(w, id, needle) do { char* r_ = w_result((w), (id), 0); \
    checks_run++; if (!r_ || !strstr(r_, (needle))) { checks_failed++; printf("FAIL %s:%d: result of %s lacks \"%s\": %s\n", __FILE__, __LINE__, (id), (needle), r_ ? r_ : "(none)"); } free(r_); } while (0)

#endif
