#!/usr/bin/env python3
"""Break the Rust code on purpose and check that a test notices.

Each mutation below replaces one exact piece of a source file, runs one test target with cargo,
and expects it to fail. A mutation the tests do not catch is either a gap in the tests or an
equivalent change (one that does not alter behaviour); say which in its note. A mutation that no
longer compiles proves nothing and is reported as such: rewrite it. The source is always restored,
even if the run is interrupted.

    python3 tools/mutate_rs.py            all mutations
    python3 tools/mutate_rs.py busy load  only those whose name starts with one of the words

cargo must be on PATH (or set CARGO to its path).
"""
import os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CARGO = os.environ.get("CARGO") or shutil.which("cargo") or "cargo"

# What to run: cargo test arguments.
AGENT = ["-p", "barb-core", "--test", "agent"]
EDIT = ["-p", "barb-ui", "--test", "edit"]
KEYS = ["-p", "barb-ui", "--test", "keys"]
MD = ["-p", "barb-ui", "--test", "md"]
SCREEN = ["-p", "barb-ui", "--test", "screen"]

# (name, file, old text, new text, test) or with a sixth element: which occurrence (1 is the first).
MUTATIONS = [
    # The agent: its scenarios and the chaos test. The C table's load-unterminated-path and
    # oom-in-tool-swallowed have no Rust counterpart: paths are owned, and there is no arena to fail.
    ('interrupt-marker', 'core/src/agent.rs', '                self.tr.user_msg().add_text(b"[barb] The user interrupted you here, before you replied.");\n', '', AGENT),
    ('busy-always-0', 'core/src/agent.rs', '!self.done && self.state != State::Idle', 'false', AGENT),
    ('load-no-claude-fallback', 'core/src/agent.rs', '                    self.ld.phase = LdPhase::Claude;\n                    self.ld_read(b"CLAUDE.md".to_vec());', '                    self.ld_finish(true);', AGENT),
    ('load-empty-counts', 'core/src/agent.rs', 'if ok && !text::trim(data).is_empty() {', 'if ok {', AGENT),
    ('load-imports-unbounded', 'core/src/agent.rs', 'self.ld.imports.len() < INSTR_IMPORTS', 'self.ld.imports.len() < 9', AGENT),
    ('load-self-import', 'core/src/agent.rs', 'path == self.ld.file.as_bytes() || ', '', AGENT),
    ('load-no-dedupe', 'core/src/agent.rs', ' || self.ld.imports.iter().any(|p| p == path)', '', AGENT),
    ('load-no-cap', 'core/src/agent.rs', 'if b.len() > INSTR_MAX {', 'if false {', AGENT),
    ('load-merges-typed-ahead', 'core/src/agent.rs', '        let typed = core::mem::take(&mut self.notes);\n        let held = core::mem::take(&mut self.ld.held);\n        self.start_turn(&held);\n        self.notes.extend(typed);', '        let held = core::mem::take(&mut self.ld.held);\n        self.start_turn(&held);', AGENT),
    ('load-interrupt-keeps-notes', 'core/src/agent.rs', '                self.notes.clear();\n                self.ld.held.clear();\n', '', AGENT),
    ('load-every-turn', 'core/src/agent.rs', 'if self.cfg.instructions && self.ld.phase == LdPhase::None {', 'if self.cfg.instructions && self.ld.phase != LdPhase::Imports {', AGENT),
    ('timeout-fraction-dropped', 'core/src/tools.rs', '            if n % 1000 != 0 {\n                push!(r; ".", n % 1000 / 100);\n            }\n', '', AGENT),
    ('timeout-always-plural', 'core/src/tools.rs', 'if n == 1000 { " second" } else { " seconds" }', '" seconds"', AGENT),

    # The prompt's line editor. Its test plays the output on a terminal emulator.
    ("edit-settle-no-cr", "ui/src/edit.rs", 'extend_from_slice(b" \\r");', 'extend_from_slice(b" ");', EDIT),
    ("edit-settle-never", "ui/src/edit.rs", "if col < self.width {\n            return row;", "if col <= self.width {\n            return row;", EDIT),
    ("edit-draw-unsettled", "ui/src/edit.rs", "let mut er = self.settle(er, ec);", "let mut er = er + 0 * ec;", EDIT),
    ("edit-wrap-early", "ui/src/edit.rs", "if c + w > self.width {\n                r += 1;", "if c + w >= self.width {\n                r += 1;", EDIT),
    ("edit-cursor-full-row", "ui/src/edit.rs", "if c >= self.width { (r + 1, 0) } else { (r, c) }", "(r, c)", EDIT),
    ("edit-hide-no-up", "ui/src/edit.rs",
     "put_up(&mut self.out, self.row);\n        self.out.extend_from_slice(b\"\\r\\x1b[J\");",
     "self.out.extend_from_slice(b\"\\r\\x1b[J\");", EDIT),
    ("edit-take-blank-row", "ui/src/edit.rs", "if c < self.width {\n                // a full", "if true {\n                // a full", EDIT),
    ("edit-mark-fast-path", "ui/src/edit.rs", "self.cur == self.line.len() && cols(ch) > 0;", "self.cur == self.line.len();", EDIT),
    ("edit-hidden-history-draws", "ui/src/edit.rs",
     "fn redraw(&mut self) {\n        if !self.shown {\n            return;\n        }\n", "fn redraw(&mut self) {\n", EDIT),
    ("edit-back-one-byte", "ui/src/edit.rs", "self.cut(self.prev_char(self.cur), self.cur);", "self.cut(self.cur - 1, self.cur);", EDIT),
    ("edit-no-continuation", "ui/src/edit.rs", "c & 0xC0 == 0x80", "c & 0xC0 == 0xC0", EDIT),
    ("edit-word-left-spaces", "ui/src/edit.rs",
     "        while i > 0 && self.is_space_at(self.prev_char(i)) {\n            i = self.prev_char(i);\n        }\n", "", EDIT),
    ("edit-line-limit", "ui/src/edit.rs", "if self.line.len() + k > MAX - 1 {", "if self.line.len() + k > MAX {", EDIT),
    ("edit-controls-typed", "ui/src/edit.rs", "if ch < 0x20 || ch == 0x7F || (0xD800", "if (0xD800", EDIT),
    ("edit-width-unclamped", "ui/src/edit.rs", "self.width = width.max(10);", "self.width = width;", EDIT),
    ("edit-history-dups", "ui/src/edit.rs", "if self.hist.back() == Some(&self.line) {", "if false {", EDIT),
    ("edit-history-no-draft", "ui/src/edit.rs", "            self.draft.clone_from(&self.line);\n", "", EDIT),
    ("edit-history-drop-wrong", "ui/src/edit.rs", "self.hist_bytes -= old.len() + 1;", "self.hist_bytes -= need;", EDIT),
    ("edit-history-oldest-stuck", "ui/src/edit.rs", "if self.back == self.hist.len() {", "if self.back + 1 == self.hist.len() {", EDIT),
    ("edit-clear-keeps-history", "ui/src/edit.rs", "self.cur = 0;\n                self.back = 0;", "self.cur = 0;", EDIT),
    ("edit-decor-cut-midchar", "ui/src/edit.rs", "while n > 0 && is_cont(s[n]) {", "while false && is_cont(s[n]) {", EDIT),
    ("edit-prompt-cols-unchecked", "ui/src/edit.rs", "let cols = if cols > 20 { 2 } else { cols };", "", EDIT),
    ("edit-resize-rows-unwrapped", "ui/src/edit.rs", "if c > width { c.div_ceil(width) } else { 1 }", "1", EDIT),
    ("edit-above-not-cleared", "ui/src/edit.rs", 'self.out.extend_from_slice(b"\\x1b[K");', "", EDIT),
    ("edit-decor-last-column", "ui/src/edit.rs", "if n + w > width - 1 {", "if n + w > width {", EDIT),
    ("edit-decor-colour-leaks", "ui/src/edit.rs", "    if styled {\n", "    if false {\n", EDIT),
    ("edit-footer-row-change", "ui/src/edit.rs", "(r != r0 || c >= self.width)", "(r != r0)", EDIT),
    ("edit-footer-fast-path", "ui/src/edit.rs", "if at_end && self.has_below &&", "if at_end && false &&", EDIT),
    ("edit-end-skips-footer", "ui/src/edit.rs", "if self.cur == self.line.len() && !self.has_below {", "if self.cur == self.line.len() {", EDIT),
    ("edit-refresh-always", "ui/src/edit.rs", "if self.dirty & 1 != 0 {", "if true {", EDIT),
    ("edit-esc-one-byte", "ui/src/edit.rs", "    if s[1] != b'[' {\n        return 2;", "    if s[1] != b'[' {\n        return 1;", EDIT),
    ("edit-unchanged-redrawn", "ui/src/edit.rs", "    if dst[..] == s[..n] {\n        return false;\n    }\n", "", EDIT),
    ("edit-moves-redraw", "ui/src/edit.rs", "if moves && self.cur == was {", "if false && self.cur == was {", EDIT),

    # The key decoder.
    ("keys-crlf-two-enters", "ui/src/keys.rs", "if b == b'\\n' && after_cr {", "if b == b'\\r' && after_cr {", KEYS),
    ("keys-paste-controls-are-keys", "ui/src/keys.rs",
     "if self.pasting {\n            return None; // a control", "if false {\n            return None; // a control", KEYS),
    ("keys-paste-sequences-are-keys", "ui/src/keys.rs",
     "if self.pasting {\n            // pasted text", "if false {\n            // pasted text", KEYS),
    ("keys-paste-never-begins", "ui/src/keys.rs", "                        self.pasting = true;\n", "", KEYS),
    ("keys-paste-end-ignored", "ui/src/keys.rs",
     "self.pasting = false;\n                return Some(Input::PasteEnd);", "return Some(Input::PasteEnd);", KEYS),
    ("keys-shift-moves-by-word", "ui/src/keys.rs", "let word = p2 > 1 && (p2 - 1) & 6 != 0;", "let word = p2 > 1;", KEYS),
    ("keys-params-overflow", "ui/src/keys.rs", "if v[at] < 100000 {", "if true {", KEYS),
    ("keys-esc-esc-one-key", "ui/src/keys.rs",
     "            if self.seqn == 1 && !self.pasting {\n                return (edit(Key::Clear), false);\n            }\n", "", KEYS),
    ("keys-alt-backspace-lost", "ui/src/keys.rs",
     "        if self.seqn == 1 && (b == 0x7F || b == 0x08) {\n            self.seq[1] = b;\n"
     "            self.seqn = 2;\n            return (self.sequence(), false);\n        }\n", "", KEYS),
    ("keys-control-breaking-swallowed", "ui/src/keys.rs", "return (None, true);", "return (None, b == b'\\r');", KEYS),
    ("keys-long-sequence-means-key", "ui/src/keys.rs", "self.seq[1] = 0; // (a sequence", "// (a sequence", KEYS),
    ("keys-lone-esc-nothing", "ui/src/keys.rs",
     "let lone_esc = self.seqn == 1 && !self.pasting;", "let lone_esc = self.seqn == 1 && self.pasting;", KEYS),
    ("keys-broken-utf8-eats-next", "ui/src/keys.rs",
     "self.need = 0; // a broken", "self.need = 0;\n            return None; // a broken", KEYS),
    ("keys-overlong-accepted", "ui/src/keys.rs", "if c < self.least || c > 0x10FFFF", "if c > 0x10FFFF", KEYS),
    ("keys-surrogate-accepted", "ui/src/keys.rs", " || (0xD800..=0xDFFF).contains(&c) || c < 0xA0", " || c < 0xA0", KEYS),
    ("keys-c1-accepted", "ui/src/keys.rs", " || c < 0xA0 {", " {", KEYS),
    ("keys-tab-dropped", "ui/src/keys.rs", "return edit(Key::Char(b'\\t' as u32));", "return None;", KEYS),

    # The markdown renderer.
    ("md-style-no-reset", "ui/src/md.rs", 'self.out.extend_from_slice(b"\\x1b[0");', 'self.out.extend_from_slice(b"\\x1b[");', MD),
    ("md-safe-esc-passes", "ui/src/md.rs", "} else if (c < 0x20 && c != b'\\n' && c != b'\\t') || c == 0x7F {",
     "} else if (c < 0x20 && c != b'\\n' && c != b'\\t' && c != 0x1B) || c == 0x7F {", MD),
    ("md-safe-c1-passes", "ui/src/md.rs", "(0x80..=0x9F).contains(&s[i + 1]) {\n            out.extend",
     "(0x80..=0x9F).contains(&s[i + 1]) && false {\n            out.extend", MD),
    ("md-inline-esc-passes", "ui/src/md.rs", "} else if c < 0x20 || c == 0x7F {", "} else if (c < 0x20 && c != 0x1B) || c == 0x7F {", MD),
    ("md-inline-c1-passes", "ui/src/md.rs", "(0x80..=0x9F).contains(&s[i + 1]) {\n                for &x",
     "(0x80..=0x9F).contains(&s[i + 1]) && false {\n                for &x", MD),
    ("md-plain-line-unsafe", "ui/src/md.rs", "        self.style(0);\n        safe(s, &mut self.out);\n",
     "        self.style(0);\n        self.out.extend_from_slice(s);\n", MD),
    ("md-long-line-cut", "ui/src/md.rs", "if self.b.len() >= LINE {\n            self.over = true;\n", "if self.b.len() >= LINE {\n", MD),
    ("md-backslash-ignored", "ui/src/md.rs", "if c == b'\\\\' && i + 1 < n && is_punct", "if false && i + 1 < n && is_punct", MD),
    ("md-code-padding-kept", "ui/src/md.rs",
     "if p.len() >= 2 && p[0] == b' ' && p[p.len() - 1] == b' ' && p.iter().any(|&x| x != b' ') {", "if false {", MD),
    ("md-snake-case-italic", "ui/src/md.rs", "                    && (c != b'_' || i == 0 || !is_alnum(s[i - 1]))\n", "", MD),
    ("md-emphasis-without-closer", "ui/src/md.rs", "&& closer_ahead(s, i + dl, &d[..dl])", "&& true", MD),
    ("md-link-url-always", "ui/src/md.rs", "if c == b'!' || text != url {", "if true {", MD),
    ("md-link-url-dropped", "ui/src/md.rs", "if c == b'!' || text != url {", "if false {", MD),
    ("md-wrap-early", "ui/src/md.rs", "if !empty && col + nsp + wc > wid {", "if !empty && col + nsp + wc >= wid {", MD),
    ("md-long-word-unbroken", "ui/src/md.rs", "if col + cw > wid && !empty {", "if false {", MD),
    ("md-wrap-last-column", "ui/src/md.rs", "self.width.saturating_sub(1).max(20)", "self.width.max(20)", MD),
    ("md-columns-are-bytes", "ui/src/md.rs", "        c += edit::cols(ch);\n", "        c += len + 0 * ch as usize;\n", MD),
    ("md-cr-kept", "ui/src/md.rs", "if e > a && s[e - 1] == b'\\r' { e - 1 } else { e }", "let _ = (s, a);\n    e", MD),
    ("md-fence-short-closes", "ui/src/md.rs", "if fl >= fence && ch == fch", "if fl > 0 && ch == fch", MD),
    ("md-fence-other-char-closes", "ui/src/md.rs", "if fl >= fence && ch == fch && ", "if fl >= fence && ", MD),
    ("md-code-block-styled", "ui/src/md.rs", "self.plain_line(l);", 'self.para(l, 0, b"", 0, b"", 0);', MD),
    ("md-hashtag-heading", "ui/src/md.rs", " && (run_of(p, 0, b'#') == pn || is_space(p[run_of(p, 0, b'#')]))", "", MD),
    ("md-heading-closing-hashes-kept", "ui/src/md.rs", "if c < b && (c == a || is_space(p[c - 1])) {", "if false {", MD),
    ("md-rule-two", "ui/src/md.rs", "    k >= 3\n", "    k >= 2\n", MD),
    ("md-quote-one-level", "ui/src/md.rs", "levels += 1;", "levels = 1;", MD),
    ("md-bullet-plus-not-list", "ui/src/md.rs", "matches!(p[0], b'-' | b'*' | b'+')", "matches!(p[0], b'-' | b'*')", MD),
    ("md-numbered-paren-not-list", "ui/src/md.rs", "(p[dl] == b'.' || p[dl] == b')')", "p[dl] == b'.'", MD),
    ("md-table-escaped-pipe-splits", "ui/src/md.rs",
     "        if i < b && s[i] == b'\\\\' {\n            i += 2;\n            continue;\n        }\n", "", MD),
    ("md-table-code-pipe-splits", "ui/src/md.rs",
     "        if i < b && s[i] == b'`' {\n            code = !code;\n            i += 1;\n            continue;\n        }\n", "", MD),
    ("md-table-align-ignored", "ui/src/md.rs", "                        2 => pad,\n                        1 => pad / 2,\n", "", MD),
    ("md-table-too-wide-drawn", "ui/src/md.rs", "if total <= wid {", "if true {", MD),
    ("md-table-delim-no-dash", "ui/src/md.rs", "if dashes == 0 || i != end {", "if i != end {", MD),
    ("md-table-header-count-unchecked", "ui/src/md.rs", "if !too_many && head.len() == align.len() {", "if true {", MD),
    ("md-table-rows-unbounded", "ui/src/md.rs",
     "            if rows.len() == ROWS {\n                fits = false;\n                break;\n            }\n", "", MD),
    ("md-table-cells-unbounded", "ui/src/md.rs", "if self.over || self.tb.len() + self.b.len() > TABLE {", "if self.over {", MD),

    # The screen: the record, the activity line, the footer. Its test plays both streams on an emulator.
    ('sc-pipe-styled', 'ui/src/screen.rs', 'self.mode != Mode::Pipe && self.vt(fd)', 'self.vt(fd)', SCREEN),
    ('sc-clean-controls-kept', 'ui/src/screen.rs', '} else if c < 0x20 || c == 0x7F {\n            b" "\n        } else if (0x80', '} else if (0x80', SCREEN),
    ('sc-clean-c1-kept', 'ui/src/screen.rs', '} else if (0x80..=0x9F).contains(&c) || (c == 0xFFFD && len == 1) {', '} else if false {', SCREEN),
    ('sc-clean-cut-unmarked', 'ui/src/screen.rs', 'if d.len() + src.len() > cap - 1 {\n            more = true;\n            break;', 'if d.len() + src.len() > cap - 1 {\n            break;', SCREEN),
    ('sc-dec-overlong', 'ui/src/screen.rs', 'if (k == 2 && c < 0x80)\n        || (k == 3', 'if (k == 3', SCREEN),
    ('sc-fit-no-ellipsis', 'ui/src/screen.rs', '    b.put(&p[..i]);\n    b.put(ell);\n}', '    b.put(&p[..i]);\n}', SCREEN),
    ('sc-fit-one-too-many', 'ui/src/screen.rs', 'if c + w > room {', 'if c + w > room + 1 {', SCREEN),
    ('sc-fit-head-one-too-many', 'ui/src/screen.rs', 'while i < n && c > room {', 'while i < n && c > room + 1 {', SCREEN),
    ('sc-home-prefix-loose', 'ui/src/screen.rs', "&& (cwd.len() == hl || cwd[hl] == b'/' || cwd[hl] == b'\\\\')", '&& true', SCREEN),
    ('sc-home-trailing-slash', 'ui/src/screen.rs', "        while hl > 1 && (home[hl - 1] == b'/' || home[hl - 1] == b'\\\\') {\n            hl -= 1;\n        }\n", '', SCREEN),
    ('sc-gap-in-pipe', 'ui/src/screen.rs', 'if want && self.styled(fd) {', 'if want {', SCREEN),
    ('sc-gap-between-user-lines', 'ui/src/screen.rs', 'Gap::User => l != Gap::None && l != Gap::User,', 'Gap::User => l != Gap::None,', SCREEN),
    ('sc-take-no-gap', 'ui/src/screen.rs', '        self.gap(2, Gap::User);\n', '', SCREEN),
    ('sc-reply-raw-on-terminal', 'ui/src/screen.rs', '        if !self.vt_out {\n            self.wr(1, p);', '        if true {\n            self.wr(1, p);', SCREEN),
    ('sc-follow-up-unsafe', 'ui/src/screen.rs', 'self.safe(2, &p[a..e]);', 'self.wr(2, &p[a..e]);', SCREEN),
    ('sc-tool-name-unsafe', 'ui/src/screen.rs', 'self.safe(2, &p[..w]);', 'self.wr(2, &p[..w]);', SCREEN),
    ('sc-tool-args-unsafe', 'ui/src/screen.rs', 'self.safe(2, &p[w..]);', 'self.wr(2, &p[w..]);', SCREEN),
    ('sc-note-unsafe', 'ui/src/screen.rs', '        self.safe(2, p);\n        self.wr(2, b"\\x1b[0m\\n");\n    }\n\n    pub fn question', '        self.wr(2, p);\n        self.wr(2, b"\\x1b[0m\\n");\n    }\n\n    pub fn question', SCREEN),
    ('sc-lite-text-unsafe', 'ui/src/screen.rs', '            if self.vt(2) {\n                self.safe(2, p);\n            } else {\n                self.wr(2, p);\n            }\n', '            self.wr(2, p);\n', SCREEN),
    ('sc-question-tool-unsafe', 'ui/src/screen.rs', 'self.safe(2, tool);', 'self.wr(2, tool);', SCREEN),
    ('sc-question-desc-unsafe', 'ui/src/screen.rs', 'self.safe(2, &desc[i..e]);', 'self.wr(2, &desc[i..e]);', SCREEN),
    ('sc-diff-uncoloured', 'ui/src/screen.rs', "desc[i] == b'+' && desc[i + 1] == b' '", "desc[i] == b'+' && desc[i + 1] == b'X'", SCREEN),
    ('sc-question-not-midline', 'ui/src/screen.rs', '        self.midline = true;\n        self.last = Gap::Question;', '        self.last = Gap::Question;', SCREEN),
    ('sc-answer-keeps-midline', 'ui/src/screen.rs', '        self.midline = false;\n    }\n\n    // ---- what', '    }\n\n    // ---- what', SCREEN),
    ('sc-clear-leaves-block', 'ui/src/screen.rs', '        if self.ed.shown {\n            self.ed.hide();\n            self.flush_ed();\n        }\n        self.status_clear();', '        self.status_clear();', SCREEN),
    ('sc-status-stuck-shown', 'ui/src/screen.rs', '        self.status_shown = false;\n        self.status.clear();', '        self.status.clear();', SCREEN),
    ('sc-429-not-retried', 'ui/src/screen.rs', 'st == 429 || ', '', SCREEN),
    ('sc-retry-never-shown', 'ui/src/screen.rs', 'if self.http_failed {\n                    self.retry_id', 'if false {\n                    self.retry_id', SCREEN),
    ('sc-failure-sticks', 'ui/src/screen.rs', '        self.http_failed = false;\n        match *ev {', '        match *ev {', SCREEN),
    ('sc-forget-wrong-task', 'ui/src/screen.rs', 'self.task.swap_remove(i);', 'self.task.pop();', SCREEN),
    ('sc-timer-latest', 'ui/src/screen.rs', '.map(|&(_, d)| d).min()', '.map(|&(_, d)| d).max()', SCREEN),
    ('sc-bg-task-is-foreground', 'ui/src/screen.rs', 'if *background {', 'if false {', SCREEN),
    ('sc-command-leading-space', 'ui/src/screen.rs', "                while k < cmd.len() && (cmd[k] == b' ' || cmd[k] == b'\\n') {\n                    k += 1;\n                }\n", '', SCREEN),
    ('sc-usage-printed-in-region', 'ui/src/screen.rs', 'if self.mode != Mode::Region {\n                        self.say(Style::Info, text);\n                    }', 'self.say(Style::Info, text);', SCREEN),
    ('sc-elapsed-unpadded', 'ui/src/screen.rs', '    if small < 10 {\n        b.put(b"0");\n    }\n', '', SCREEN),
    ('sc-elapsed-hours-wrong', 'ui/src/screen.rs', '(t / 3600, t / 60 % 60)', '(t / 3600, t % 60)', SCREEN),
    ('sc-elapsed-under-a-second', 'ui/src/screen.rs', 'if now - since >= 1000 {', 'if true {', SCREEN),
    ('sc-retry-rounds-down', 'ui/src/screen.rs', 'if left > 0 { left + 999 } else { 0 }', 'if left > 0 { left } else { 0 }', SCREEN),
    ('sc-ascii-spinner-fast', 'ui/src/screen.rs', 'let k = (t / 250 % 4) as usize;', 'let k = (t / 120 % 4) as usize;', SCREEN),
    ('sc-next-ms-one-period', 'ui/src/screen.rs', 'let period = if self.fancy { 120 } else { 250 };', 'let period = 120;', SCREEN),
    ('sc-next-ms-always', 'ui/src/screen.rs', '        if !self.animating {\n            return -1;\n        }\n        let period', '        let period', SCREEN),
    ('sc-tokens-under-1000-k', 'ui/src/screen.rs', 'if t < 1000 {\n        b.int(t);', 'if t < 1_000_000 {\n        b.int(t);', SCREEN),
    ('sc-tokens-big-tenths', 'ui/src/screen.rs', '    if t / unit >= 100 {\n        b.int(t / unit);\n        b.put(u);\n        return;\n    }\n', '', SCREEN),
    ('sc-hint-never-queues', 'ui/src/screen.rs', 'if !self.ed.line.is_empty() { b"Enter queues it" } else { b"Ctrl-C stops it" }', 'b"Ctrl-C stops it"', SCREEN),
    ('sc-hint-crowds-footer', 'ui/src/screen.rs', 'if lc + 3 + hc <= width {', 'if true {', SCREEN),
    ('sc-region-question-line-open', 'ui/src/screen.rs', '            if self.midline {\n                self.wr(2, b"\\n");\n                self.midline = false;\n            }\n            self.ed.set_prompt', '            self.ed.set_prompt', SCREEN),
    ('sc-lite-uncut', 'ui/src/screen.rs', '            if c + w > width {\n                break;\n            }\n', '', SCREEN),
    ('sc-lite-over-question', 'ui/src/screen.rs', '|| !self.animating || self.midline {', '|| !self.animating {', SCREEN),
    ('sc-lite-in-pipe', 'ui/src/screen.rs', 'if !status_ok || self.mode == Mode::Pipe || ', 'if !status_ok || ', SCREEN),
    ('sc-resize-md-width', 'ui/src/screen.rs', '        self.md.width = width;\n', '', SCREEN),
]


def run(test):
    r = subprocess.run([CARGO, "test", "-q", *test], cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=1200)
    out = r.stdout + r.stderr
    if "error[" in out or "error: could not compile" in out:
        return "DOES NOT COMPILE", out
    return ("caught" if r.returncode else "SURVIVED"), out


def main():
    words = sys.argv[1:]
    bad = 0
    for m in MUTATIONS:
        name, path, old, new, test = m[:5]
        nth = m[5] if len(m) > 5 else 1
        if words and not any(name.startswith(w) for w in words):
            continue
        full = os.path.join(ROOT, path)
        with open(full, encoding="utf-8", newline="") as f:
            src = f.read()
        at = -1
        for _ in range(nth):
            at = src.find(old, at + 1)
            if at < 0:
                break
        if at < 0:
            print(f"{name}: NOT FOUND in {path}", flush=True)
            bad += 1
            continue
        try:
            with open(full, "w", encoding="utf-8", newline="") as f:
                f.write(src[:at] + new + src[at + len(old):])
            status, out = run(test)
        finally:
            with open(full, "w", encoding="utf-8", newline="") as f:
                f.write(src)
        print(f"{name}: {status}", flush=True)
        if status != "caught":
            bad += 1
            if status == "DOES NOT COMPILE":
                print("\n".join(l for l in out.splitlines() if l.startswith("error"))[:600])
    print("all caught" if not bad else f"{bad} not caught", flush=True)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
