#!/usr/bin/env python3
"""Break the code on purpose and check that a test notices.

Each mutation below replaces one exact piece of a source file, rebuilds one test target, runs it,
and expects it to fail. A mutation the tests do not catch is either a gap in the tests or an
equivalent change (one that does not alter behaviour); say which in its note. The source is
always restored, and the target rebuilt from the restored source, even if the run is interrupted.

    python3 tools/mutate.py            all mutations
    python3 tools/mutate.py busy load  only those whose name contains one of the words
"""
import subprocess, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (name, file, old text, new text, make target, command to run)
AGENT = ("build/test_agent", ["build/test_agent"])
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
    # bash display: the command when nobody is asked, a capped and cleaned preview of the output
    ("show-no-cap", "src/core/tools.c", "SHOW_LINES = 20", "SHOW_LINES = 200", *AGENT),
    ("show-no-footer", "src/core/tools.c", "    if (total > shown) {", "    if (0) {", *AGENT),
    ("show-footer-plural", "src/core/tools.c", 'total - shown == 1 ? " more line" : " more lines"', '" more lines"', *AGENT),
    ("show-no-column-cut", "src/core/tools.c", "SHOW_COLS = 120", "SHOW_COLS = 1200", *AGENT),
    ("show-raw-controls", "src/core/tools.c", "(len == 1 && (ch == '\\t' || (ch >= 32 && ch < 127)))", "1", *AGENT),
    ("show-allows-c1", "src/core/tools.c", "(s[0] == 0xC2 && s[1] < 0xA0) ? 0 : 2", "2", *AGENT),
    ("show-allows-overlong3", "src/core/tools.c", "if (s[0] == 0xE0 && s[1] < 0xA0) return 0;   // overlong", "", *AGENT),
    ("show-allows-surrogate", "src/core/tools.c", "if (s[0] == 0xED && s[1] >= 0xA0) return 0;  // surrogate", "", *AGENT),
    ("show-allows-overlong4", "src/core/tools.c", "if (s[0] == 0xF0 && s[1] < 0x90) return 0;   // overlong", "", *AGENT),
    ("show-allows-past-max", "src/core/tools.c", "if (s[0] == 0xF4 && s[1] >= 0x90) return 0;  // past U+10FFFF", "", *AGENT),
    ("show-no-utf8", "src/core/tools.c", "if (len > 1) sb_put(&b, out.p + k, len);", "if (0) sb_put(&b, out.p + k, len);", *AGENT),
    ("show-no-command-echo", "src/core/tools.c", "        if (!needs_ask(g, BIT_BASH)) {\n            // Nobody", "        if (0) {\n            // Nobody", *AGENT),
    ("show-command-when-asked", "src/core/tools.c", "        if (!needs_ask(g, BIT_BASH)) {\n            // Nobody", "        if (1) {\n            // Nobody", *AGENT),
    ("show-no-output", "src/core/tools.c", "    say_output(g, out);\n", "    if (0) say_output(g, out);\n", *AGENT),
]


def sh(cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True)


def main():
    words = sys.argv[1:]
    todo = [m for m in MUTATIONS if not words or any(w in m[0] for w in words)]
    survived = []
    for name, path, old, new, target, run in todo:
        full = os.path.join(ROOT, path)
        src = open(full, "rb").read()
        if src.count(old.encode()) != 1:
            print(f"{name}: the text to replace is not in {path} exactly once; update this table")
            survived.append(name)
            continue
        try:
            open(full, "wb").write(src.replace(old.encode(), new.encode()))
            b = sh(["make", target])
            if b.returncode != 0:
                print(f"{name}: caught (does not build)")
                continue
            r = sh(run)
            caught = r.returncode != 0
            print(f"{name}: {'caught' if caught else 'SURVIVED'}")
            if not caught: survived.append(name)
        finally:
            open(full, "wb").write(src)
            sh(["make", target])
    print(f"{len(todo) - len(survived)} of {len(todo)} caught")
    sys.exit(1 if survived else 0)


if __name__ == "__main__":
    main()
