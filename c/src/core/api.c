// The transcript and the Messages API: see api.h.
#include "api.h"
#include <string.h>

block_t* block_new(arena_t* a, bkind_t kind) {
    block_t* b = arena_alloc(a, sizeof *b);
    if (b) b->kind = kind;
    return b;
}

msg_t* tr_new_msg(arena_t* a, transcript_t* t, int assistant) {
    msg_t* m = arena_alloc(a, sizeof *m);
    if (!m) return 0;
    m->assistant = assistant;
    if (t->last) t->last->next = m; else t->first = m;
    t->last = m;
    return m;
}

void msg_add(msg_t* m, block_t* b) {
    b->next = 0;
    if (m->last) m->last->next = b; else m->first = b;
    m->last = b;
}

msg_t* tr_user_msg(arena_t* a, transcript_t* t) {
    if (t->last && !t->last->assistant) return t->last;
    return tr_new_msg(a, t, 0);
}

int tr_add_text(arena_t* a, msg_t* m, str_t text) {
    if (text.n == 0) return 1;
    block_t* b = block_new(a, BK_TEXT);
    if (!b) return 0;
    b->text = str_dup(a, text);
    if (!b->text.p) return 0;
    msg_add(m, b);
    return 1;
}

// ---- the request ----

static int emittable(const block_t* b) {
    switch (b->kind) {
    case BK_TEXT: return b->text.n > 0;
    case BK_TOOL_USE: case BK_TOOL_RESULT: return 1;
    case BK_RAW: return json_valid(b->text);
    }
    return 0;
}

static void cache_mark(jw_t* w) {
    jw_key(w, "cache_control");
    jw_obj(w);
    jw_key(w, "type"); jw_strz(w, "ephemeral");
    jw_obj_end(w);
}

static void emit_block(jw_t* w, const block_t* b, int cache) {
    if (b->kind == BK_RAW) { jw_raw(w, b->text); return; }
    jw_obj(w);
    switch (b->kind) {
    case BK_TEXT:
        jw_key(w, "type"); jw_strz(w, "text");
        jw_key(w, "text"); jw_str(w, b->text);
        if (cache) cache_mark(w);
        break;
    case BK_TOOL_USE:
        jw_key(w, "type"); jw_strz(w, "tool_use");
        jw_key(w, "id"); jw_str(w, b->id);
        jw_key(w, "name"); jw_str(w, b->name);
        jw_key(w, "input"); jw_raw(w, b->input.n ? b->input : str_z("{}"));
        break;
    case BK_TOOL_RESULT:
        jw_key(w, "type"); jw_strz(w, "tool_result");
        jw_key(w, "tool_use_id"); jw_str(w, b->id);
        jw_key(w, "content"); jw_str(w, b->text);
        if (b->is_error) { jw_key(w, "is_error"); jw_bool(w, 1); }
        if (cache) cache_mark(w);
        break;
    case BK_RAW:
        break;
    }
    jw_obj_end(w);
}

static int msg_emittable(const msg_t* m) {
    for (const block_t* b = m->first; b; b = b->next) if (emittable(b)) return 1;
    return 0;
}

// "messages": the transcript, with the cache breakpoint on block `cache_at` (none if 0).
static void write_messages(jw_t* w, const transcript_t* t, const block_t* cache_at) {
    jw_key(w, "messages");
    jw_arr(w);
    for (const msg_t* m = t->first; m; m = m->next) {
        if (!msg_emittable(m)) continue;
        jw_obj(w);
        jw_key(w, "role"); jw_strz(w, m->assistant ? "assistant" : "user");
        jw_key(w, "content");
        jw_arr(w);
        for (const block_t* b = m->first; b; b = b->next) {
            if (!emittable(b)) continue;
            emit_block(w, b, b == cache_at && (b->kind == BK_TEXT || b->kind == BK_TOOL_RESULT));
        }
        jw_arr_end(w);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

int api_request(sb_t* out, const api_cfg_t* cfg, const transcript_t* t) {
    // The last block of the last message that has any gets the cache breakpoint.
    const block_t* last_block = 0;
    for (const msg_t* m = t->first; m; m = m->next)
        for (const block_t* b = m->first; b; b = b->next)
            if (emittable(b)) last_block = b;

    jw_t w;
    jw_init(&w, out);
    jw_obj(&w);
    jw_key(&w, "model"); jw_str(&w, cfg->model);
    jw_key(&w, "max_tokens"); jw_int(&w, cfg->max_tokens);
    if (cfg->thinking) {
        jw_key(&w, "thinking");
        jw_obj(&w);
        jw_key(&w, "type"); jw_strz(&w, "adaptive");
        jw_key(&w, "display"); jw_strz(&w, "summarized");
        jw_obj_end(&w);
    }
    if (cfg->system.n) {
        jw_key(&w, "system");
        jw_arr(&w);
        jw_obj(&w);
        jw_key(&w, "type"); jw_strz(&w, "text");
        jw_key(&w, "text"); jw_str(&w, cfg->system);
        cache_mark(&w);
        jw_obj_end(&w);
        jw_arr_end(&w);
    }
    if (cfg->tools.n) { jw_key(&w, "tools"); jw_raw(&w, cfg->tools); }
    write_messages(&w, t, last_block);
    jw_obj_end(&w);
    return !out->failed && !w.bad;
}

// ---- copies and repairs ----

int tr_copy(arena_t* a, const transcript_t* in, transcript_t* out) {
    out->first = out->last = 0;
    for (const msg_t* m = in->first; m; m = m->next) {
        msg_t* c = tr_new_msg(a, out, m->assistant);
        if (!c) return 0;
        for (const block_t* b = m->first; b; b = b->next) {
            block_t* d = block_new(a, b->kind);
            if (!d) return 0;
            *d = *b;
            msg_add(c, d);
        }
    }
    return 1;
}

size_t tr_count(const transcript_t* t) {
    size_t n = 0;
    for (const msg_t* m = t->first; m; m = m->next) n++;
    return n;
}

// A result kept as it came (its content is not a string) answers its call too. The id is
// compared as JSON text, which is how it is written for any id a model makes.
static int raw_result_for(const block_t* b, str_t id) {
    str_t v, rid;
    if (!json_get(b->text, "type", &v) || !str_eqz(v, "\"tool_result\"")) return 0;
    if (!json_get(b->text, "tool_use_id", &rid) || rid.n != id.n + 2) return 0;
    return rid.p[0] == '"' && !memcmp(rid.p + 1, id.p, id.n) && rid.p[id.n + 1] == '"';
}

static int has_result(const msg_t* m, str_t id) {
    for (const block_t* b = m->first; b; b = b->next) {
        if (b->kind == BK_TOOL_RESULT && str_eq(b->id, id)) return 1;
        if (b->kind == BK_RAW && raw_result_for(b, id)) return 1;
    }
    return 0;
}

int tr_repair(arena_t* a, transcript_t* t) {
    // Empty messages go, and two messages of one side in a row (one between them was empty)
    // become one.
    msg_t* prev = 0;
    msg_t* m = t->first;
    t->first = t->last = 0;
    while (m) {
        msg_t* next = m->next;
        m->next = 0;
        if (!msg_emittable(m)) {
            // dropped
        } else if (prev && prev->assistant == m->assistant) {
            prev->last->next = m->first;
            prev->last = m->last;
        } else {
            if (prev) prev->next = m; else t->first = m;
            prev = m;
        }
        m = next;
    }
    t->last = prev;

    int added = 0;
    for (msg_t* q = t->first; q; q = q->next) {
        if (!q->assistant) continue;
        int any = 0;
        for (const block_t* b = q->first; b; b = b->next) if (b->kind == BK_TOOL_USE) any = 1;
        if (!any) continue;
        msg_t* u = q->next;
        if (!u || u->assistant) {
            u = arena_alloc(a, sizeof *u);
            if (!u) return -1;
            u->next = q->next;
            q->next = u;
            if (t->last == q) t->last = u;
        }
        // The missing results go first, in the order of the calls.
        block_t* after = 0;
        for (const block_t* b = q->first; b; b = b->next) {
            if (b->kind != BK_TOOL_USE || has_result(u, b->id)) continue;
            block_t* r = block_new(a, BK_TOOL_RESULT);
            if (!r) return -1;
            r->id = b->id;
            r->text = str_z("Not run: Barb was closed before this tool call finished.");
            r->is_error = 1;
            if (after) { r->next = after->next; after->next = r; }
            else { r->next = u->first; u->first = r; }
            if (!r->next) u->last = r;
            after = r;
            added++;
        }
        q = u;  // the next assistant message comes after it
    }
    return added;
}

// ---- the reply ----

static int get_str(arena_t* a, str_t obj, const char* key, str_t* out) {
    str_t v;
    if (!json_get(obj, key, &v)) return 0;
    return json_as_str(a, v, out);
}

static int64_t get_int(str_t obj, const char* key) {
    str_t v;
    int64_t n = 0;
    if (json_get(obj, key, &v) && json_as_int(v, &n)) return n;
    return 0;
}

static int parse_content(arena_t* a, str_t content, msg_t* m) {
    json_t j;
    json_open(&j, content);
    if (json_next(&j) != JT_ARR) return 0;
    for (;;) {
        jtok_t t = json_next(&j);
        if (t == JT_ARR_END) return 1;
        str_t el;
        if (t != JT_OBJ || !json_capture(&j, t, &el)) return 0;

        str_t type = str_of(0, 0), v, s;
        get_str(a, el, "type", &type);
        if (str_eqz(type, "text")) {
            str_t text;
            if (!get_str(a, el, "text", &text)) return 0;
            if (!tr_add_text(a, m, text)) return 0;
        } else if (str_eqz(type, "tool_use")) {
            block_t* b = block_new(a, BK_TOOL_USE);
            str_t input;
            if (!b) return 0;
            if (!get_str(a, el, "id", &b->id) || !get_str(a, el, "name", &b->name)) return 0;
            if (!json_get(el, "input", &input)) input = str_z("{}");
            b->input = str_dup(a, input);
            if (!b->input.p) return 0;
            msg_add(m, b);
        } else if (str_eqz(type, "tool_result") && json_get(el, "content", &v) && json_as_str(a, v, &s)) {
            // A result whose content is a string; any other kind is kept as it came.
            block_t* b = block_new(a, BK_TOOL_RESULT);
            if (!b) return 0;
            if (!get_str(a, el, "tool_use_id", &b->id)) return 0;
            b->text = s;
            int e = 0;
            if (json_get(el, "is_error", &v) && json_as_bool(v, &e)) b->is_error = e;
            msg_add(m, b);
        } else {
            block_t* b = block_new(a, BK_RAW);
            if (!b) return 0;
            b->text = str_dup(a, el);
            if (!b->text.p) return 0;
            msg_add(m, b);
        }
    }
}

// ---- saved sessions ----

int api_session(sb_t* out, str_t model, str_t cwd, const transcript_t* t) {
    jw_t w;
    jw_init(&w, out);
    jw_obj(&w);
    jw_key(&w, "barb_session"); jw_int(&w, SESSION_VERSION);
    jw_key(&w, "model"); jw_str(&w, model);
    jw_key(&w, "cwd"); jw_str(&w, cwd);
    write_messages(&w, t, 0);
    jw_obj_end(&w);
    return !out->failed && !w.bad;
}

int api_load_session(arena_t* a, str_t data, session_t* s) {
    memset(s, 0, sizeof *s);
    if (!json_valid(data)) return 0;
    str_t v, msgs;
    int64_t ver;
    if (!json_get(data, "barb_session", &v) || !json_as_int(v, &ver)) return 0;
    if (ver < 1 || ver > SESSION_VERSION) return 0;
    get_str(a, data, "model", &s->model);
    get_str(a, data, "cwd", &s->cwd);
    if (!json_get(data, "messages", &msgs)) return 0;
    json_t j;
    json_open(&j, msgs);
    if (json_next(&j) != JT_ARR) return 0;
    for (;;) {
        jtok_t t = json_next(&j);
        if (t == JT_ARR_END) break;
        str_t el, role, content;
        if (t != JT_OBJ || !json_capture(&j, t, &el)) return 0;
        if (!get_str(a, el, "role", &role)) return 0;
        int assistant = str_eqz(role, "assistant");
        if (!assistant && !str_eqz(role, "user")) return 0;
        if (!json_get(el, "content", &content)) return 0;
        msg_t* m = tr_new_msg(a, &s->tr, assistant);
        if (!m || !parse_content(a, content, m)) return 0;
    }
    return tr_repair(a, &s->tr) >= 0 && !a->failed;
}

static char* put2(char* o, int v) {
    o[0] = (char)('0' + v / 10 % 10);
    o[1] = (char)('0' + v % 10);
    return o + 2;
}

int session_new_name(char* out, const int t[6], int try_) {
    if (try_ < 0 || try_ > 'z' - 'b' + 1) return 0;
    char* o = out;
    o = put2(o, t[0] / 100);
    o = put2(o, t[0]);
    o = put2(o, t[1]);
    o = put2(o, t[2]);
    *o++ = '-';
    o = put2(o, t[3]);
    o = put2(o, t[4]);
    o = put2(o, t[5] % 60);
    if (try_) { *o++ = '-'; *o++ = (char)('b' + try_ - 1); }
    *o = 0;
    return 1;
}

int session_file(char* out, size_t cap, const char* arg, int backslash) {
    size_t n = strlen(arg);
    int path = memchr(arg, '/', n) || (backslash && memchr(arg, '\\', n));
    int ext = n >= 5 && !memcmp(arg + n - 5, ".json", 5);
    const char* dir = path ? "" : SESSIONS_DIR "/";
    const char* tail = path || ext ? "" : ".json";
    size_t need = strlen(dir) + n + strlen(tail) + 1;
    if (need > cap) return 0;
    memcpy(out, dir, strlen(dir));
    memcpy(out + strlen(dir), arg, n);
    memcpy(out + strlen(dir) + n, tail, strlen(tail) + 1);
    return 1;
}

int session_summary(arena_t* a, str_t data, size_t* nmsgs, str_t* first) {
    session_t s;
    *nmsgs = 0;
    *first = str_of(0, 0);
    if (!api_load_session(a, data, &s)) return 0;
    *nmsgs = tr_count(&s.tr);
    for (const msg_t* m = s.tr.first; m; m = m->next) {
        if (m->assistant) continue;
        for (const block_t* b = m->first; b; b = b->next) {
            if (b->kind != BK_TEXT || str_starts(b->text, str_z("[barb]"))) continue;
            size_t k = 0;
            while (k < b->text.n && b->text.p[k] != '\n' && k < 60) k++;
            // not cut inside a character
            while (k && k < b->text.n && ((unsigned char)b->text.p[k] & 0xC0) == 0x80) k--;
            *first = str_of(b->text.p, k);
            return 1;
        }
    }
    return 1;
}

int api_parse(arena_t* a, str_t body, api_reply_t* r) {
    memset(r, 0, sizeof *r);
    if (!json_valid(body)) return 0;
    str_t type;
    if (!get_str(a, body, "type", &type)) return 0;

    if (str_eqz(type, "error")) {
        str_t e;
        if (!json_get(body, "error", &e)) return 0;
        get_str(a, e, "type", &r->err_type);
        get_str(a, e, "message", &r->err_msg);
        return 1;
    }
    if (!str_eqz(type, "message")) return 0;

    str_t content;
    if (!json_get(body, "content", &content)) return 0;
    msg_t* m = arena_alloc(a, sizeof *m);
    if (!m) return 0;
    m->assistant = 1;
    if (!parse_content(a, content, m)) return 0;
    r->msg = m;
    r->is_message = 1;
    get_str(a, body, "stop_reason", &r->stop_reason);  // null (an unfinished stream) leaves it empty
    str_t u;
    if (json_get(body, "usage", &u)) {
        r->in_tok = get_int(u, "input_tokens");
        r->out_tok = get_int(u, "output_tokens");
        r->cache_read = get_int(u, "cache_read_input_tokens");
        r->cache_write = get_int(u, "cache_creation_input_tokens");
    }
    return !a->failed;
}
