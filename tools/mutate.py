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
    ("edit-cursor-full-row", "src/ui/edit.c", "if (cc >= e->width) { cr++; cc = 0; }", "", *EDIT),
    ("edit-hide-no-up", "src/ui/edit.c", "if (e->row) put_csi(e, e->row, 'A');", "", *EDIT),
    ("edit-take-blank-row", "src/ui/edit.c", 'if (c < e->width) putz(e, "\\r\\n");', 'putz(e, "\\r\\n");', *EDIT),
    ("edit-mark-fast-path", "src/ui/edit.c", "int at_end = e->cur == e->n && ed_cols(ch) > 0;", "int at_end = e->cur == e->n;", *EDIT),
    ("edit-hidden-history-draws", "src/ui/edit.c",
     "    if (e->shown) redraw(e);\n}\n\n// ---- editing", "    redraw(e);\n}\n\n// ---- editing", *EDIT),
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

    # The POSIX front end, through the end-to-end test.
    ("posix-no-bracketed-paste", "src/posix/main.c", 'if (!raw_on) fputs("\\033[?2004h", stderr);', "", *E2E),
    ("posix-paste-enter-sends", "src/posix/main.c", "if ((kd.pasting || more_text_waiting())", "if ((more_text_waiting())", *E2E),
    ("posix-paste-end-not-sent", "src/posix/main.c", "        if (paste_n && !ed.n) deliver_paste();\n", "", *E2E),
    ("posix-paste-ignores-rest-of-read", "src/posix/main.c", "for (int i = rbuf_i + 1; i < rbuf_n; i++)", "for (int i = rbuf_n; i < rbuf_n; i++)", *E2E),
    ("posix-ctrl-c-never-clears", "src/posix/main.c",
     "if (interactive && !awaiting && !agent_busy(agent) && (ed.n || paste_n || kd.pasting)) {", "if (0) {", *E2E),
    ("posix-ctrl-d-ends-typed-line", "src/posix/main.c", "        if (ed.n) { ed_key(&ed, EK_DEL, 0); ed_flush(); }\n        else end_input();",
     "        end_input();", *E2E),
    # No mutation for ask()'s read_waiting_keys(): the loop reads stdin before it services curl, so
    # keys typed while the model works are on the line before a question can exist (that is what
    # test_one_key_typeahead checks); the call only closes a window of microseconds between that
    # read and the question, which no test can time. Removing it survives, and is not equivalent.
    ("posix-esc-never-times-out", "src/posix/main.c", "            if (kd_timeout(&kd, &k)) on_key(&k);", "            (void)kd_timeout(&kd, &k);", *E2E),
    ("posix-esc-wait-unbounded", "src/posix/main.c", "            if (timeout < 0 || timeout > left) timeout = (int)left;\n", "", *E2E),
    ("posix-suspend-ignored", "src/posix/main.c", "        case 'z': suspend(); break;\n", "        case 'z': if (0) suspend(); break;\n", *E2E),
    ("posix-suspend-terminal-raw", "src/posix/main.c", "    term_restore();\n    fflush(stdout);\n    signal(SIGTSTP", "    fflush(stdout);\n    signal(SIGTSTP", *E2E),
    ("posix-suspend-default", "src/posix/main.c", "        sigaction(SIGTSTP, &sa, 0);\n        sigaction(SIGCONT", "        sigaction(SIGCONT", *E2E),
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
