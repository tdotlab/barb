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
    ("edit-settle-no-cr", "src/win32/edit.c", 'putz(e, " \\r");', 'putz(e, " ");', *EDIT),
    ("edit-settle-never", "src/win32/edit.c", "if (col < e->width) return row;", "if (col <= e->width) return row;", *EDIT),
    ("edit-draw-unsettled", "src/win32/edit.c", "    er = settle(e, er, ec);\n", "", *EDIT),
    ("edit-wrap-early", "src/win32/edit.c", "if (c + w > e->width) { r++; c = 0; }", "if (c + w >= e->width) { r++; c = 0; }", *EDIT),
    ("edit-cursor-full-row", "src/win32/edit.c", "if (cc >= e->width) { cr++; cc = 0; }", "", *EDIT),
    ("edit-hide-no-up", "src/win32/edit.c", "if (e->row) put_csi(e, e->row, 'A');", "", *EDIT),
    ("edit-take-blank-row", "src/win32/edit.c", 'if (c < e->width) putz(e, "\\r\\n");', 'putz(e, "\\r\\n");', *EDIT),
    ("edit-mark-fast-path", "src/win32/edit.c", "int at_end = e->cur == e->n && ed_cols(ch) > 0;", "int at_end = e->cur == e->n;", *EDIT),
    ("edit-hidden-history-draws", "src/win32/edit.c",
     "    if (e->shown) redraw(e);\n}\n\n// ---- editing", "    redraw(e);\n}\n\n// ---- editing", *EDIT),
    ("edit-back-one-byte", "src/win32/edit.c", "cut(e, prev_char(e, e->cur), e->cur);", "cut(e, e->cur - 1, e->cur);", *EDIT),
    ("edit-no-continuation", "src/win32/edit.c", "& 0xC0) == 0x80; }", "& 0xC0) == 0xC0; }", *EDIT),
    ("edit-word-left-spaces", "src/win32/edit.c", "    while (i > 0 && is_space_at(e, prev_char(e, i))) i = prev_char(e, i);\n", "", *EDIT),
    ("edit-no-room-for-nul", "src/win32/edit.c", "if (e->n + k > ED_MAX - 1) return;", "if (e->n + k > ED_MAX) return;", *EDIT),
    ("edit-controls-typed", "src/win32/edit.c", "if (ch < 0x20 || ch == 0x7F || ", "if (", *EDIT),
    ("edit-width-unclamped", "src/win32/edit.c", "e->width = width < 10 ? 10 : width;", "e->width = width;", *EDIT),
    ("edit-history-dups", "src/win32/edit.c", "if (last >= 0 && !strcmp(e->hist + last, e->line)) return;", "(void)last;", *EDIT),
    ("edit-history-no-draft", "src/win32/edit.c", "if (e->back == 0) { memcpy(e->draft, e->line, (size_t)e->n); e->draftn = e->n; }", "", *EDIT),
    ("edit-history-drop-wrong", "src/win32/edit.c", "int drop = (int)strlen(e->hist) + 1;", "int drop = need;", *EDIT),
    ("edit-clear-keeps-history", "src/win32/edit.c", "e->line[0] = 0; e->back = 0; break;", "e->line[0] = 0; break;", *EDIT),

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
