#!/usr/bin/env python3
"""Break the code on purpose and check that a test notices.

Each mutation below replaces one exact piece of a source file, rebuilds one test target, runs it,
and expects it to fail. A mutation the tests do not catch is either a gap in the tests or an
equivalent change (one that does not alter behaviour); say which in its note. The source is
always restored, and the target rebuilt from the restored source, even if the run is interrupted.

    python3 tools/mutate.py            all mutations
    python3 tools/mutate.py busy load  only those whose name contains one of the words

On Windows it builds with build.cmd (`build.cmd one NAME`) and runs only the mutations of the
code that builds there: the core and src/win32. Elsewhere it uses make and skips src/win32.
"""
import subprocess, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (name, file, old text, new text, make target, command to run)
AGENT = ("build/test_agent", ["build/test_agent"])
EDIT = ("build/test_edit", ["build/test_edit"])
KEYS = ("build/test_keys", ["build/test_keys"])
MD = ("build/test_md", ["build/test_md"])
SCREEN = ("build/test_screen", ["build/test_screen"])
WAIT = ("build/test_wait", ["build/test_wait"])
E2E = ("build/barb-san", ["python3", "tests/e2e.py", "build/barb-san"])
E2E_WIN = ("build/barb-san", ["python", "tests/e2e.py", "build/barb-san.exe"])
MUTATIONS = [
    ("interrupt-marker", "src/core/agent.c",
     'if (u) tr_add_text(&g->life, u, str_z("[barb] The user interrupted you here, before you replied."));', "(void)u;", *AGENT),
    ("busy-always-0", "src/core/agent.c",
     "int agent_busy(const agent_t* g) { return !g->done && g->state != S_IDLE; }",
     "int agent_busy(const agent_t* g) { (void)g; return 0; }", *AGENT),
    ("load-no-claude-fallback", "src/core/agent.c",
     '            g->ld_phase = LD_CLAUDE;\n            ld_read(g, str_z("CLAUDE.md"));',
     "            ld_finish(g, 1);", *AGENT),
    ("load-empty-counts", "src/core/agent.c",
     "if (ok && str_trim(ev->data).n) {", "if (ok) {", *AGENT),
    ("load-imports-unbounded", "src/core/agent.c",
     "g->ld_nimports < INSTR_IMPORTS", "g->ld_nimports < 9", *AGENT),
    ("load-self-import", "src/core/agent.c",
     "int dup = !path.n || str_eq(path, g->ld_file);", "int dup = !path.n;", *AGENT),
    ("load-no-dedupe", "src/core/agent.c",
     "for (int k = 0; !dup && k < g->ld_nimports; k++) dup = str_eq(g->ld_imports[k], path);", "", *AGENT),
    ("load-no-cap", "src/core/agent.c", "if (r.n > INSTR_MAX) {", "if (0) {", *AGENT),
    ("load-merges-typed-ahead", "src/core/agent.c",
     "    g->nhead = g->ntail = 0;\n    start_turn(g, g->ld_held);",
     "    start_turn(g, g->ld_held);", *AGENT),
    ("load-unterminated-path", "src/core/agent.c",
     "g->ld_imports[g->ld_nimports++] = str_dup(&g->life, path);", "g->ld_imports[g->ld_nimports++] = path;", *AGENT),
    ("load-interrupt-keeps-notes", "src/core/agent.c",
     "        g->nhead = g->ntail = 0;\n        g->ld_held = str_of(0, 0);\n        ld_finish(g, 0);",
     "        ld_finish(g, 0);", *AGENT),
    ("load-every-turn", "src/core/agent.c",
     "if (g->cfg.instructions && g->ld_phase == LD_NONE) {", "if (g->cfg.instructions && g->ld_phase != LD_IMPORTS) {", *AGENT),
    ("timeout-fraction-dropped", "src/core/tools.c",
     "if (t->n % 1000) { sb_putc(&r, '.'); sb_puti(&r, t->n % 1000 / 100); }", "", *AGENT),
    ("timeout-always-plural", "src/core/tools.c",
     'sb_putz(&r, t->n == 1000 ? " second" : " seconds");', 'sb_putz(&r, " seconds");', *AGENT),
    ("oom-in-tool-swallowed", "src/core/agent.c",
     "    if (g->ta.failed) g->life.failed = 1;", "", *AGENT),

    # The Windows prompt's line editor. Its test plays the output on a terminal emulator.
    ("edit-settle-no-cr", "src/ui/edit.c", 'putz(e, " \\r");', 'putz(e, " ");', *EDIT),
    ("edit-settle-never", "src/ui/edit.c", "if (col < e->width) return row;", "if (col <= e->width) return row;", *EDIT),
    ("edit-draw-unsettled", "src/ui/edit.c", "    er = settle(e, er, ec);\n", "", *EDIT),
    ("edit-wrap-early", "src/ui/edit.c", "if (c + w > e->width) { r++; c = 0; }", "if (c + w >= e->width) { r++; c = 0; }", *EDIT),
    ("edit-cursor-full-row", "src/ui/edit.c", "if (*col >= e->width) { (*row)++; *col = 0; }", "", *EDIT),
    ("edit-hide-no-up", "src/ui/edit.c", '    put_up(e, e->row);\n    putz(e, "\\r\\033[J");', '    putz(e, "\\r\\033[J");', *EDIT),
    ("edit-take-blank-row", "src/ui/edit.c", 'if (c < e->width) putz(e, "\\r\\n");', 'putz(e, "\\r\\n");', *EDIT),
    ("edit-mark-fast-path", "src/ui/edit.c", "int at_end = e->cur == e->n && ed_cols(ch) > 0;", "int at_end = e->cur == e->n;", *EDIT),
    ("edit-hidden-history-draws", "src/ui/edit.c",
     "static void redraw(ed_t* e) {\n    if (!e->shown) return;\n", "static void redraw(ed_t* e) {\n", *EDIT),
    ("edit-back-one-byte", "src/ui/edit.c", "cut(e, prev_char(e, e->cur), e->cur);", "cut(e, e->cur - 1, e->cur);", *EDIT),
    ("edit-no-continuation", "src/ui/edit.c", "& 0xC0) == 0x80; }", "& 0xC0) == 0xC0; }", *EDIT),
    ("edit-word-left-spaces", "src/ui/edit.c", "    while (i > 0 && is_space_at(e, prev_char(e, i))) i = prev_char(e, i);\n", "", *EDIT),
    ("edit-no-room-for-nul", "src/ui/edit.c", "if (e->n + k > ED_MAX - 1) return;", "if (e->n + k > ED_MAX) return;", *EDIT),
    ("edit-controls-typed", "src/ui/edit.c", "if (ch < 0x20 || ch == 0x7F || ", "if (", *EDIT),
    ("edit-width-unclamped", "src/ui/edit.c", "e->width = width < 10 ? 10 : width;", "e->width = width;", *EDIT),
    ("edit-history-dups", "src/ui/edit.c", "if (last >= 0 && !strcmp(e->hist + last, e->line)) return;", "(void)last;", *EDIT),
    ("edit-history-no-draft", "src/ui/edit.c", "if (e->back == 0) { memcpy(e->draft, e->line, (size_t)e->n); e->draftn = e->n; }", "", *EDIT),
    ("edit-history-drop-wrong", "src/ui/edit.c", "int drop = (int)strlen(e->hist) + 1;", "int drop = need;", *EDIT),
    ("edit-clear-keeps-history", "src/ui/edit.c", "e->line[0] = 0; e->back = 0; break;", "e->line[0] = 0; break;", *EDIT),

    # The key decoder.
    ("keys-crlf-two-enters", "src/ui/keys.c", "if (b == '\\n' && after_cr) return 0;", "if (b == '\\r' && after_cr) return 0;", *KEYS),
    ("keys-paste-controls-are-keys", "src/ui/keys.c", "    if (d->pasting) return 0;  // a control", "    if (0) return 0;  // a control", *KEYS),
    ("keys-paste-sequences-are-keys", "src/ui/keys.c", "    if (d->pasting) {          // pasted text", "    if (0) {          // pasted text", *KEYS),
    ("keys-shift-moves-by-word", "src/ui/keys.c", "int word = p2 > 1 && ((p2 - 1) & 6);", "int word = p2 > 1;", *KEYS),
    ("keys-params-overflow", "src/ui/keys.c", "if (v[at] < 100000) v[at]", "v[at]", *KEYS),
    ("keys-esc-esc-one-key", "src/ui/keys.c", "        if (d->seqn == 1 && !d->pasting) return edit(k, EK_CLEAR);\n", "", *KEYS),
    ("keys-alt-backspace-lost", "src/ui/keys.c",
     "    if (d->seqn == 1 && (b == 0x7F || b == 0x08)) { d->seq[d->seqn++] = b; return sequence(d, k); }\n", "", *KEYS),
    ("keys-control-breaking-swallowed", "src/ui/keys.c", "{ d->seqn = 0; *again = 1; return 0; }", "{ d->seqn = 0; *again = b == '\\r'; return 0; }", *KEYS),
    ("keys-long-sequence-means-key", "src/ui/keys.c", "        d->seq[1] = 0;                     // (a sequence", "        // (a sequence", *KEYS),
    ("keys-lone-esc-nothing", "src/ui/keys.c", "return lone_esc ? edit(k, EK_CLEAR) : 0;", "return lone_esc && d->pasting ? edit(k, EK_CLEAR) : 0;", *KEYS),
    ("keys-broken-utf8-eats-next", "src/ui/keys.c", "        d->need = 0;           // a broken", "        d->need = 0; return 0;           // a broken", *KEYS),
    ("keys-overlong-accepted", "src/ui/keys.c", "if (c < d->least || c > 0x10FFFF", "if (c > 0x10FFFF", *KEYS),
    ("keys-surrogate-accepted", "src/ui/keys.c", " || (c >= 0xD800 && c <= 0xDFFF) || c < 0xA0)", " || c < 0xA0)", *KEYS),
    ("keys-c1-accepted", "src/ui/keys.c", " || c < 0xA0) return 0;", ") return 0;", *KEYS),
    ("keys-paste-end-ignored", "src/ui/keys.c", '{ d->pasting = 0; return key(k, KD_PASTE_END, 0, 0); }', "{ return key(k, KD_PASTE_END, 0, 0); }", *KEYS),

    # The markdown renderer. Its test plays the output on a terminal emulator.
    ("md-style-no-reset", "src/ui/md.c", "    b[k++] = '0';\n", "", *MD),
    ("md-safe-esc-passes", "src/ui/md.c", "if ((c < 0x20 && c != '\\n' && c != '\\t') || c == 0x7F)",
     "if ((c < 0x20 && c != '\\n' && c != '\\t' && c != 0x1B) || c == 0x7F)", *MD),
    ("md-safe-c1-passes", "src/ui/md.c", "<= 0x9F) {\n            memcpy(rep", "<= 0x9F && 0) {\n            memcpy(rep", *MD),
    ("md-inline-esc-passes", "src/ui/md.c", "else if (c < 0x20 || c == 0x7F) { add1", "else if ((c < 0x20 && c != 0x1B) || c == 0x7F) { add1", *MD),
    ("md-inline-c1-passes", "src/ui/md.c", "<= 0x9F) {\n            add1(m, '\\xEF'", "<= 0x9F && 0) {\n            add1(m, '\\xEF'", *MD),
    ("md-plain-line-unsafe", "src/ui/md.c", "    md_safe(m->write, m->ctx, s, n);\n", "    m->write(m->ctx, s, n);\n", *MD),
    ("md-long-line-cut", "src/ui/md.c", "if (m->n >= MD_LINE) { m->over = 1; return; }", "if (m->n >= MD_LINE) { return; }", *MD),
    ("md-backslash-ignored", "src/ui/md.c",
     "        if (c == '\\\\' && i + 1 < n && is_punct(s[i + 1])) { add(m, s + i + 1, 1, st); i += 2; continue; }\n", "", *MD),
    ("md-code-padding-kept", "src/ui/md.c", "if (len >= 2 && p[0] == ' ' && p[len - 1] == ' ' && blank < len) { p++; len -= 2; }", "", *MD),
    ("md-snake-case-italic", "src/ui/md.c", " && (c != '_' || i == 0 || !is_alnum(s[i - 1])) &&", " &&", *MD),
    ("md-emphasis-without-closer", "src/ui/md.c", "closer_ahead(s, n, i + dl, d, dl)) {", "1) {", *MD),
    ("md-link-url-always", "src/ui/md.c", "if (c == '!' || tl != ul || memcmp(s + a, s + u, tl)) {", "if (1) {", *MD),
    ("md-link-url-dropped", "src/ui/md.c", "if (c == '!' || tl != ul || memcmp(s + a, s + u, tl)) {", "if (0) {", *MD),
    ("md-wrap-early", "src/ui/md.c", "if (!empty && col + (int)nsp + wc > W) {", "if (!empty && col + (int)nsp + wc >= W) {", *MD),
    ("md-long-word-unbroken", "src/ui/md.c", "if (col + cw > W && !empty) { end_line(m); wz(m, p2); col = c2; }", "", *MD),
    ("md-wrap-last-column", "src/ui/md.c", "return m->width - 1 < 20 ? 20 : m->width - 1;", "return m->width < 20 ? 20 : m->width;", *MD),
    ("md-columns-are-bytes", "src/ui/md.c", "        c += ed_cols(dec(s + i, n - i, &len));\n", "        dec(s + i, n - i, &len); c += len;\n", *MD),
    ("md-cr-kept", "src/ui/md.c", "return e > a && s[e - 1] == '\\r' ? e - 1 : e;", "return e;", *MD),
    ("md-fence-short-closes", "src/ui/md.c", "if (fl >= fence && ch == fch", "if (fl && ch == fch", *MD),
    ("md-fence-other-char-closes", "src/ui/md.c", "if (fl >= fence && ch == fch && ", "if (fl >= fence && ", *MD),
    ("md-code-block-styled", "src/ui/md.c", "            } else plain_line(m, L, ln);", '            } else para(m, L, ln, 0, "", 0, "", 0);', *MD),
    ("md-hashtag-heading", "src/ui/md.c", "(run_of(p, pn, 0, '#') == pn || is_space(p[run_of(p, pn, 0, '#')]))", "1", *MD),
    ("md-heading-closing-hashes-kept", "src/ui/md.c", "if (c < b && (c == a || is_space(p[c - 1]))) { b = c;", "if (0) { b = c;", *MD),
    ("md-rule-two", "src/ui/md.c", "return k >= 3;", "return k >= 2;", *MD),
    ("md-quote-one-level", "src/ui/md.c", "        levels++;\n", "        levels = 1;\n", *MD),
    ("md-bullet-plus-not-list", "src/ui/md.c", "(p[0] == '-' || p[0] == '*' || p[0] == '+')", "(p[0] == '-' || p[0] == '*')", *MD),
    ("md-numbered-paren-not-list", "src/ui/md.c", "(p[dl] == '.' || p[dl] == ')')", "p[dl] == '.'", *MD),
    ("md-table-escaped-pipe-splits", "src/ui/md.c", "        if (i < b && s[i] == '\\\\') { i++; continue; }\n", "", *MD),
    ("md-table-code-pipe-splits", "src/ui/md.c", "        if (i < b && s[i] == '`') { code = !code; continue; }\n", "", *MD),
    ("md-table-align-ignored", "src/ui/md.c", "before = align[c] == 2 ? pad : align[c] == 1 ? pad / 2 : 0;", "before = 0;", *MD),
    ("md-table-too-wide-drawn", "src/ui/md.c", "    if (total <= W) {\n", "    if (1) {\n", *MD),
    ("md-table-delim-no-dash", "src/ui/md.c", "if (!dashes || i != l - (size_t)right) return 0;", "if (i != l - (size_t)right) return 0;", *MD),
    ("md-table-header-count-unchecked", "src/ui/md.c", "if (nc && split_cells(L, ln, hoff, hlen, MD_COLS + 1) == nc) {", "if (nc) {", *MD),
    ("md-table-rows-unbounded", "src/ui/md.c", "        if (rows == MD_ROWS) { fits = 0; break; }\n", "", *MD),
    ("md-table-cells-unbounded", "src/ui/md.c", "if (m->over || m->tn + m->n > MD_TABLE) return 0;", "if (m->over) return 0;", *MD),

    # The screen: the record, the activity line, the footer. Its test plays both streams on an emulator.
    ("sc-pipe-styled", "src/ui/screen.c", "return s->cfg.mode != SC_PIPE && vt(s, fd);", "return vt(s, fd);", *SCREEN),
    ("sc-clean-controls-kept", "src/ui/screen.c", '        if (c < 0x20 || c == 0x7F) { src = " "; out = 1; }\n', "", *SCREEN),
    ("sc-clean-c1-kept", "src/ui/screen.c", "if ((c >= 0x80 && c <= 0x9F) || (c == 0xFFFD && len == 1)) {", "if (0) {", *SCREEN),
    ("sc-clean-cut-unmarked", "src/ui/screen.c", "if (k + (size_t)out > cap - 1) { more = 1; break; }", "if (k + (size_t)out > cap - 1) { break; }", *SCREEN),
    ("sc-dec-overlong", "src/ui/screen.c", "if ((k == 2 && c < 0x80) || ", "if (", *SCREEN),
    ("sc-fit-no-ellipsis", "src/ui/screen.c", "    bput(b, p, i);\n    bputz(b, ell);\n}", "    bput(b, p, i);\n}", *SCREEN),
    ("sc-fit-one-too-many", "src/ui/screen.c", "if (c + w > room) break;", "if (c + w > room + 1) break;", *SCREEN),
    ("sc-fit-head-one-too-many", "src/ui/screen.c", "while (i < n && c > room) {", "while (i < n && c > room + 1) {", *SCREEN),
    ("sc-home-prefix-loose", "src/ui/screen.c", "(cwd[hl] == 0 || cwd[hl] == '/' || cwd[hl] == '\\\\')", "1", *SCREEN),
    ("sc-home-trailing-slash", "src/ui/screen.c", "    while (hl > 1 && (home[hl - 1] == '/' || home[hl - 1] == '\\\\')) hl--;\n", "", *SCREEN),
    ("sc-gap-in-pipe", "src/ui/screen.c", "if (want && styled(s, fd)) wrz", "if (want) wrz", *SCREEN),
    ("sc-gap-between-user-lines", "src/ui/screen.c", "want = s->last != G_NONE && s->last != G_USER;", "want = s->last != G_NONE;", *SCREEN),
    ("sc-take-no-gap", "src/ui/screen.c", "    gap(s, 2, G_USER);\n", "", *SCREEN),
    ("sc-reply-raw-on-terminal", "src/ui/screen.c", "if (!s->cfg.vt_out) { wr(s, 1, p, n);", "if (1) { wr(s, 1, p, n);", *SCREEN),
    ("sc-follow-up-unsafe", "src/ui/screen.c", "        safe(s, 2, p + a, e - a);\n", "        wr(s, 2, p + a, e - a);\n", *SCREEN),
    ("sc-tool-name-unsafe", "src/ui/screen.c", "        safe(s, 2, p, w);\n", "        wr(s, 2, p, w);\n", *SCREEN),
    ("sc-tool-args-unsafe", "src/ui/screen.c", "        safe(s, 2, p + w, n - w);\n", "        wr(s, 2, p + w, n - w);\n", *SCREEN),
    ("sc-note-unsafe", "src/ui/screen.c", "    safe(s, 2, p, n);\n    wrz(s, 2, \"\\033[0m\\n\");\n}", "    wr(s, 2, p, n);\n    wrz(s, 2, \"\\033[0m\\n\");\n}", *SCREEN),
    ("sc-lite-text-unsafe", "src/ui/screen.c", "if (vt(s, 2)) safe(s, 2, p, n); else wr(s, 2, p, n);", "wr(s, 2, p, n);", *SCREEN),
    ("sc-question-tool-unsafe", "src/ui/screen.c", "        safe(s, 2, tool, tn);\n", "        wr(s, 2, tool, tn);\n", *SCREEN),
    ("sc-question-desc-unsafe", "src/ui/screen.c", "            safe(s, 2, desc + i, e - i);\n", "            wr(s, 2, desc + i, e - i);\n", *SCREEN),
    ("sc-diff-uncoloured", "src/ui/screen.c", "desc[i] == '+' && desc[i + 1] == ' ' ?", "desc[i] == '+' && desc[i + 1] == 'X' ?", *SCREEN),
    ("sc-question-not-midline", "src/ui/screen.c", "    s->midline = 1;\n    s->last = G_QUESTION;", "    s->last = G_QUESTION;", *SCREEN),
    ("sc-answer-keeps-midline", "src/ui/screen.c", "    s->midline = 0;\n}\n\n// ---- what", "}\n\n// ---- what", *SCREEN),
    ("sc-clear-leaves-block", "src/ui/screen.c", "    if (s->ed.shown) { ed_hide(&s->ed); flush_ed(s); }\n    status_clear(s);", "    status_clear(s);", *SCREEN),
    ("sc-status-stuck-shown", "src/ui/screen.c", "    s->status_shown = 0;\n    s->status[0] = 0;", "    s->status[0] = 0;", *SCREEN),
    ("sc-429-not-retried", "src/ui/screen.c", "st == 429 || ", "", *SCREEN),
    ("sc-retry-never-shown", "src/ui/screen.c", "if (s->http_failed) { s->retry_id", "if (0) { s->retry_id", *SCREEN),
    ("sc-failure-sticks", "src/ui/screen.c", "    s->http_failed = 0;\n    switch (ev->kind) {", "    switch (ev->kind) {", *SCREEN),
    ("sc-forget-wrong-task", "src/ui/screen.c", "if (i >= 0) s->task[i] = s->task[--s->ntasks];", "if (i >= 0) s->ntasks--;", *SCREEN),
    ("sc-timer-latest", "src/ui/screen.c", "if (s->timer_due[i] < due) due", "if (s->timer_due[i] > due) due", *SCREEN),
    ("sc-bg-task-is-foreground", "src/ui/screen.c", "if (a->flag == 1) { if (s->ntasks", "if (0) { if (s->ntasks", *SCREEN),
    ("sc-command-leading-space", "src/ui/screen.c", "            while (k < a->a.n && (a->a.p[k] == ' ' || a->a.p[k] == '\\n')) k++;\n", "", *SCREEN),
    ("sc-usage-printed-in-region", "src/ui/screen.c", "if (s->cfg.mode != SC_REGION) sc_say(s, SAY_INFO", "sc_say(s, SAY_INFO", *SCREEN),
    ("sc-elapsed-unpadded", "src/ui/screen.c", '    if (small < 10) bputz(b, "0");\n', "", *SCREEN),
    ("sc-elapsed-hours-wrong", "src/ui/screen.c", "small = t < 3600 ? t % 60 : t / 60 % 60;", "small = t % 60;", *SCREEN),
    ("sc-elapsed-under-a-second", "src/ui/screen.c", "if (now - since >= 1000) {", "if (1) {", *SCREEN),
    ("sc-retry-rounds-down", "src/ui/screen.c", "elapsed(b, left > 0 ? left + 999 : 0);", "elapsed(b, left > 0 ? left : 0);", *SCREEN),
    ("sc-ascii-spinner-fast", "src/ui/screen.c", "&plain[t / 250 % 4]", "&plain[t / 120 % 4]", *SCREEN),
    ("sc-next-ms-one-period", "src/ui/screen.c", "int64_t period = s->cfg.fancy ? 120 : 250,", "int64_t period = 120,", *SCREEN),
    ("sc-next-ms-always", "src/ui/screen.c", "    if (!s->animating) return -1;\n", "", *SCREEN),
    ("sc-tokens-under-1000-k", "src/ui/screen.c", "if (t < 1000) { bputi(b, t); return; }", "if (t < 1000000) { bputi(b, t); return; }", *SCREEN),
    ("sc-tokens-big-tenths", "src/ui/screen.c", "    if (t / unit >= 100) { bputi(b, t / unit); bputz(b, u); return; }\n", "", *SCREEN),
    ("sc-hint-never-queues", "src/ui/screen.c", 'busy ? (s->ed.n ? "Enter queues it" : "Ctrl-C stops it")', 'busy ? "Ctrl-C stops it"', *SCREEN),
    ("sc-hint-crowds-footer", "src/ui/screen.c", "if (lc + 3 + hc <= width)", "if (1)", *SCREEN),
    ("sc-region-question-line-open", "src/ui/screen.c",
     '        if (s->midline) { wrz(s, 2, "\\n"); s->midline = 0; }\n        char row', "        char row", *SCREEN),
    ("sc-lite-uncut", "src/ui/screen.c", "        if (c + w > width) break;\n", "", *SCREEN),
    ("sc-lite-over-question", "src/ui/screen.c", "!s->animating || s->midline) {", "!s->animating) {", *SCREEN),
    ("sc-lite-in-pipe", "src/ui/screen.c", "if (!status_ok || s->cfg.mode == SC_PIPE || ", "if (!status_ok || ", *SCREEN),
    ("sc-resize-md-width", "src/ui/screen.c", "    s->md.width = width;\n", "", *SCREEN),

    # The POSIX front end, through the end-to-end test.
    ("posix-no-bracketed-paste", "src/posix/main.c", "if (!raw_on) { flush_out(); write_all(2, ", "if (0) { flush_out(); write_all(2, ", *E2E),
    ("posix-paste-enter-sends", "src/posix/main.c", "if ((kd.pasting || more_text_waiting())", "if ((more_text_waiting())", *E2E),
    ("posix-paste-end-not-sent", "src/posix/main.c", "        if (paste_n && !scr.ed.n) deliver_paste();\n", "", *E2E),
    ("posix-paste-ignores-rest-of-read", "src/posix/main.c", "for (int i = rbuf_i + 1; i < rbuf_n; i++)", "for (int i = rbuf_n; i < rbuf_n; i++)", *E2E),
    ("posix-ctrl-c-never-clears", "src/posix/main.c",
     "if (interactive && !awaiting && !agent_busy(agent) && (scr.ed.n || paste_n || kd.pasting)) {", "if (0) {", *E2E),
    ("posix-ctrl-d-ends-typed-line", "src/posix/main.c", "        if (scr.ed.n) sc_key(&scr, EK_DEL, 0);\n        else end_input();",
     "        end_input();", *E2E),
    # No mutation for ask()'s read_waiting_keys(): the loop reads stdin before it services curl, so
    # keys typed while the model works are on the line before a question can exist (that is what
    # test_one_key_typeahead checks); the call only closes a window of microseconds between that
    # read and the question, which no test can time. Removing it survives, and is not equivalent.
    ("posix-esc-never-times-out", "src/posix/main.c", "            if (kd_timeout(&kd, &k)) on_key(&k);", "            (void)kd_timeout(&kd, &k);", *E2E),
    ("posix-esc-wait-unbounded", "src/posix/main.c", "            if (timeout < 0 || timeout > left) timeout = (int)left;\n", "", *E2E),
    ("posix-suspend-ignored", "src/posix/main.c", "        case 'z': suspend(); break;\n", "        case 'z': if (0) suspend(); break;\n", *E2E),
    ("posix-suspend-terminal-raw", "src/posix/main.c", "    term_restore();\n    signal(SIGTSTP", "    signal(SIGTSTP", *E2E),
    ("posix-suspend-default", "src/posix/main.c", "        sigaction(SIGTSTP, &sa, 0);\n        sigaction(SIGCONT", "        sigaction(SIGCONT", *E2E),
    ("posix-wait-never-ready", "src/posix/ops.c", "{ fds[i].revents = POLLIN; ready++; }", "{ ready++; }", *WAIT),
    ("posix-wait-no-timeout", "src/posix/ops.c", "        tp = &tv;\n", "        tp = 0; (void)tv;\n", *WAIT),
    ("posix-wait-misses-last-fd", "src/posix/ops.c", "select(maxfd + 1, &rd", "select(maxfd, &rd", *WAIT),
    ("posix-wait-seconds-as-ms", "src/posix/ops.c", "tv.tv_usec = (timeout_ms % 1000) * 1000;", "tv.tv_usec = (timeout_ms % 1000);", *WAIT),
    ("posix-ctrl-backslash-quits", "src/posix/main.c", "    t.c_cc[VQUIT] = _POSIX_VDISABLE;\n", "", *E2E),

    # The Windows front end, through the end-to-end test.
    ("win-stdin-event-auto-reset", "src/win32/main.c",
     "in_ready = CreateEventW(0, TRUE, FALSE, 0);", "in_ready = CreateEventW(0, FALSE, FALSE, 0);", *E2E_WIN),
    ("win-key-left-in-environment", "src/win32/main.c",
     'SetEnvironmentVariableW(L"ANTHROPIC_API_KEY", 0);', "", *E2E_WIN),
    ("win-paste-never-gathers", "src/win32/main.c",
     "if (more_text_waiting() && paste_add", "if (0 && paste_add", *E2E_WIN),
    ("win-paste-ignores-rest-of-batch", "src/win32/main.c",
     "for (DWORD i = batch_i + 1; i < batch_n; i++) if (text_key(&batch[i])) return 1;", "", *E2E_WIN),
    ("win-paste-never-ends", "src/win32/main.c",
     "    deliver_simple(EV_INPUT, 0, 0, paste);\n    paste_n = 0;\n",
     "    deliver_simple(EV_INPUT, 0, 0, paste);\n", *E2E_WIN),
]

WIN = os.name == "nt"


def sh(cmd):
    if cmd[0].startswith("build/"):     # a program of ours, by its full path (and .exe on Windows)
        cmd = [os.path.join(ROOT, cmd[0]) + (".exe" if WIN else "")] + cmd[1:]
    return subprocess.run(cmd, cwd=ROOT, capture_output=True)


def build(target):
    if WIN:
        return subprocess.run(["cmd", "/c", os.path.join(ROOT, "build.cmd"), "one", os.path.basename(target)], cwd=ROOT, capture_output=True)
    return sh(["make", target])


def builds_here(path):
    return not path.startswith("src/posix") if WIN else not path.startswith("src/win32")


def main():
    words = sys.argv[1:]
    todo = [m for m in MUTATIONS if builds_here(m[1]) and (not words or any(w in m[0] for w in words))]
    survived = []
    for name, path, old, new, target, run in todo:
        full = os.path.join(ROOT, path)
        src = open(full, "rb").read()
        if b"\r\n" in src:                # a checkout with Windows line endings
            old, new = old.replace("\n", "\r\n"), new.replace("\n", "\r\n")
        if src.count(old.encode()) != 1:
            print(f"{name}: the text to replace is not in {path} exactly once; update this table")
            survived.append(name)
            continue
        try:
            open(full, "wb").write(src.replace(old.encode(), new.encode()))
            b = build(target)
            if b.returncode != 0:
                print(f"{name}: caught (does not build)")
                continue
            r = sh(run)
            caught = r.returncode != 0
            print(f"{name}: {'caught' if caught else 'SURVIVED'}")
            if not caught: survived.append(name)
        finally:
            open(full, "wb").write(src)
            build(target)
    print(f"{len(todo) - len(survived)} of {len(todo)} caught")
    sys.exit(1 if survived else 0)


if __name__ == "__main__":
    main()
