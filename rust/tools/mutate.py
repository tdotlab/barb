#!/usr/bin/env python3
"""Break the Rust code on purpose and check that a test notices.

Each mutation below replaces one exact piece of a source file, runs one test target with cargo,
and expects it to fail. A mutation the tests do not catch is either a gap in the tests or an
equivalent change (one that does not alter behaviour); say which in its note. A mutation that no
longer compiles proves nothing and is reported as such: rewrite it. The source is always restored,
even if the run is interrupted.

    python3 tools/mutate.py            all mutations
    python3 tools/mutate.py busy load  only those whose name starts with one of the words

cargo must be on PATH (or set CARGO to its path). Mutations of barb/src/posix run only on Linux and
macOS, those of barb/src/win32 only on Windows.
"""
import os, shutil, signal, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CARGO = os.environ.get("CARGO") or shutil.which("cargo") or "cargo"

# What to run: cargo test arguments.
AGENT = ["-p", "barb-core", "--test", "agent"]
API = ["-p", "barb-core", "--test", "api"]
EDIT = ["-p", "barb-ui", "--test", "edit"]
KEYS = ["-p", "barb-ui", "--test", "keys"]
MD = ["-p", "barb-ui", "--test", "md"]
SCREEN = ["-p", "barb-ui", "--test", "screen"]
COMMON = ["-p", "barb", "--", "common::"]
WAIT = ["-p", "barb", "--", "posix::sys::"]
OPS = ["-p", "barb", "--", "posix::ops::"]
WOPS = ["-p", "barb", "--", "win32::ops::"]
WMAIN = ["-p", "barb", "--", "win32::tests::"]
E2E = "e2e"    # a release build of barb, then tests/e2e.py against it

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

    # Saved conversations and verbose, in the agent and the API layer.
    ('save-never', 'core/src/agent.rs', 'if self.cfg.session.is_empty() {\n            return;\n        }\n        let data', 'if true {\n            return;\n        }\n        let data', AGENT),
    ('save-not-after-send', 'core/src/agent.rs', '        self.wait_said = false;\n        self.save();\n', '        self.wait_said = false;\n', AGENT),
    ('save-not-after-reply', 'core/src/agent.rs', '            self.tr.msgs.push(r.msg);\n            self.save();', '            self.tr.msgs.push(r.msg);', AGENT),
    ('save-not-after-interrupt', 'core/src/agent.rs', '                self.tr.user_msg().add_text(b"[barb] The user interrupted you here, before you replied.");\n                self.save();\n', '                self.tr.user_msg().add_text(b"[barb] The user interrupted you here, before you replied.");\n', AGENT),
    ('save-not-after-tool-interrupt', 'core/src/agent.rs', '                self.complete_round(false);\n                self.save();\n', '                self.complete_round(false);\n', AGENT),
    ('save-result-not-routed', 'core/src/agent.rs', ' if self.save_ops.contains(&id) => self.on_saved(id, status, note),', ' if false => self.on_saved(id, status, note),', AGENT),
    ('save-failure-silent', 'core/src/agent.rs', 'if status == Fs::Ok || self.save_warned {', 'if true {', AGENT),
    ('save-failure-every-time', 'core/src/agent.rs', '        self.save_warned = true;\n        let mut b', '        let mut b', AGENT),
    ('save-clear-same-file', 'core/src/agent.rs', '                    self.clears += 1; // the old conversation stays in its file\n', '', AGENT),
    ('save-clear-path-off-by-one', 'core/src/agent.rs', 'cat!(stem, "-", self.clears + 1, ".json")', 'cat!(stem, "-", self.clears, ".json")', AGENT),
    ('save-clear-empty-bumps', 'core/src/agent.rs', 'if !self.tr.msgs.is_empty() && !self.cfg.session.is_empty() {\n                    self.clears', 'if !self.cfg.session.is_empty() {\n                    self.clears', AGENT),
    ('resume-no-note', 'core/src/agent.rs', '        self.resume_note = note;\n', '        let _ = note;\n', AGENT),
    ('resume-note-dropped', 'core/src/agent.rs', '        let resumed = core::mem::take(&mut self.resume_note);\n        let u = self.tr.user_msg();\n        u.add_text(&resumed);', '        let u = self.tr.user_msg();', AGENT),
    ('resume-note-survives-clear', 'core/src/agent.rs', '                self.resume_note.clear();\n', '', AGENT),
    ('resume-cwd-unchecked', 'core/src/agent.rs', 'if !s.cwd.is_empty() && s.cwd != self.cfg.cwd {', 'if false {', AGENT),
    ('resume-no-replay', 'core/src/agent.rs', '        if show {\n            self.replay();\n        }\n', '', AGENT),
    ('resume-replay-all', 'core/src/agent.rs', 'let from = n.saturating_sub(REPLAY_MSGS);', 'let from = 0 * n;', AGENT),
    ('resume-replay-shows-notes', 'core/src/agent.rs', '                        if !t.starts_with(b"[barb]") {\n                            self.say(Style::User, t.clone());\n                        }', '                        self.say(Style::User, t.clone());', AGENT),
    ('resume-replay-no-tools', 'core/src/agent.rs', '                        let t = tools::label(name, input);\n                        self.say(Style::Tool, t);', '                        let _ = (name, input);', AGENT),
    ('thinking-always', 'core/src/agent.rs', '        if !self.cfg.verbose {\n            return;\n        }\n        let Some(kind)', '        let Some(kind)', AGENT),
    ('thinking-redacted-unshown', 'core/src/agent.rs', '            self.say(Style::Thinking, "[redacted thinking]");', '', AGENT),
    ('thinking-untrimmed', 'core/src/agent.rs', '                let t = text::trim(&t);\n', '                let t = &t[..];\n', AGENT),
    ('verbose-command-dead', 'core/src/agent.rs', '                self.cfg.verbose = !self.cfg.verbose;\n', '', AGENT),
    ('label-no-path', 'core/src/tools.rs', 'b"read_file" | b"edit_file" | b"write_file" => push!(b; " ", s("path")),', 'b"read_file" | b"edit_file" | b"write_file" => {}', AGENT),
    ('label-no-description', 'core/src/tools.rs', 'push!(b; if desc.is_empty() { text::first_line(&cmd, 100) } else { &desc[..] });', 'push!(b; text::first_line(&cmd, 100));', AGENT),
    ('label-no-background', 'core/src/tools.rs', 'push!(b; " ", if bg { "(background) " } else { "" });', 'push!(b; " ");', AGENT),
    ('session-any-version', 'core/src/api.rs', 'if !(1..=SESSION_VERSION).contains(&v) {', 'if v < 1 {', API),
    ('session-any-role', 'core/src/api.rs', 'if !m.assistant && role != b"user" {', 'if false {', API),
    ('session-no-repair', 'core/src/api.rs', '    s.tr.repair();\n    Some(s)', '    Some(s)', API),
    ('session-no-cwd', 'core/src/api.rs', '    w.key("cwd");\n    w.str(cwd);\n', '', API),
    ('repair-keeps-empty', 'core/src/api.rs', '        self.msgs.retain(|m| m.blocks.iter().any(Block::emittable));\n', '', API),
    ('repair-no-merge', 'core/src/api.rs', 'if self.msgs[i].assistant == self.msgs[i - 1].assistant {', 'if false {', API),
    ('repair-no-new-user-msg', 'core/src/api.rs', "            if !matches!(self.msgs.get(i + 1), Some(m) if !m.assistant) {\n                self.msgs.insert(i + 1, Msg { assistant: false, blocks: Vec::new() });\n            }\n", "            if self.msgs.len() == i + 1 {\n                break;\n            }\n", API),
    ('repair-duplicates-results', 'core/src/api.rs', "let has = next.blocks.iter().any(|b| matches!(b, Block::ToolResult { id: r, .. } if *r == id));", "let has = false;", API),
    ('repair-results-at-end', 'core/src/api.rs', '                    next.blocks.insert(\n                        k,', '                    next.blocks.insert(\n                        next.blocks.len(),', API),

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

    # What both platforms' programs share: the command line, kept output, talking to curl.
    ('common-max-turns-unparsed', 'barb/src/common.rs', 'a.max_turns = atoi(', 'a.max_turns = 1 + 0 * atoi(', COMMON),
    ('common-continue-unparsed', 'barb/src/common.rs', '            a.start = Start::Continue;\n', '', COMMON),
    ('common-name-no-suffix', 'barb/src/common.rs', '    if !taken(&base) {\n        return base;\n    }\n', '    if true {\n        return base;\n    }\n', COMMON),
    ('common-name-seconds-unwrapped', 'barb/src/common.rs', 't.3, t.4, sec % 60);', 't.3, t.4, sec);', COMMON),
    ('common-session-file-path-as-name', 'barb/src/common.rs', "    if arg.contains('/') || (cfg!(windows) && arg.contains('\\\\')) {\n        return arg.to_string();\n    }\n", '', COMMON),
    ('common-session-file-double-json', 'barb/src/common.rs', 'let n = if arg.ends_with(".json") { arg.to_string() } else { format!("{}.json", arg) };', 'let n = format!("{}.json", arg);', COMMON),
    ('e2e-no-save-saves', 'barb/src/common.rs', '            if a.no_save {\n                return Ok(Plan::default());\n            }\n', '', E2E),
    ('e2e-resume-no-save-saves', 'barb/src/common.rs', 'path: if a.no_save { Vec::new() } else { path.into_bytes() },', 'path: path.into_bytes(),', E2E),
    ('e2e-continue-oldest', 'barb/src/common.rs', '    v.sort_by(|a, b| b.cmp(a)); // newest first\n', '    v.sort(); // newest first\n', E2E),
    ('e2e-resume-garbage', 'barb/src/common.rs', '    let s = barb_core::api::load_session(&data).ok_or_else(|| format!("{} is not a saved Barb conversation", path))?;\n    Ok(Plan { path: if a.no_save { Vec::new() } else { path.into_bytes() }, data: Some(data), model: s.model })',
     '    let s = barb_core::api::load_session(&data).unwrap_or(barb_core::api::Session { model: Vec::new(), cwd: Vec::new(), tr: Default::default() });\n    Ok(Plan { path: if a.no_save { Vec::new() } else { path.into_bytes() }, data: Some(data), model: s.model })', E2E),
    ('e2e-no-gitignore', 'barb/src/common.rs', '    if !gi.exists() {\n', '    if false {\n', E2E),
    ('e2e-no-saved-line', 'barb/src/common.rs', 'Some(format!("Saved as {}; barb -r {} takes it up again.\\n", name, name))', 'Some(String::from(&name[..0]))', E2E),
    ('e2e-sessions-no-first-line', 'barb/src/common.rs', 'String::from_utf8_lossy(&first)));', 'String::from_utf8_lossy(&first[..0])));', E2E),
    ('common-output-head-overfills', 'barb/src/common.rs', 'let take = (self.head_keep - self.head.len()).min(p.len());', 'let take = p.len();', COMMON),
    ('common-output-drop-unmarked', 'barb/src/common.rs', '            out.extend_from_slice(format!("\\n[... {} bytes of output dropped here ...]\\n", dropped).as_bytes());\n', '', COMMON),
    ('common-curl-000-kept', 'barb/src/common.rs', '        if s.ends_with(b"000") {\n            s = &s[..s.len() - 3];\n        }\n', '', COMMON),
    ('common-curl-status-in-body', 'barb/src/common.rs', 'out[..cut - 1].to_vec()', 'out[..cut].to_vec()', COMMON),
    ('common-key-not-wiped', 'barb/src/common.rs', '    for c in b.iter_mut() {', '    for c in b.iter_mut().take(0) {', COMMON),

    # The POSIX program, through its own tests and the end-to-end test.
    ('common-plain-http-user-is-host', 'barb/src/common.rs', "rfind('@').map_or(0, |i| i + 1)", "rfind('@').map_or(0, |_| 0)", COMMON),
    ('common-plain-http-port-kept', 'barb/src/common.rs', "&h[..h.find(':').unwrap_or(h.len())]", 'h', COMMON),
    ('posix-wait-never-ready', 'barb/src/posix/sys.rs', '            f.revents = POLLIN;\n            ready += 1;', '            ready += 1;', WAIT),
    ('posix-wait-no-timeout', 'barb/src/posix/sys.rs', 'if timeout_ms >= 0 { &mut tv } else', 'if false { &mut tv } else', WAIT),
    ('posix-wait-misses-last-fd', 'barb/src/posix/sys.rs', 'select(maxfd + 1, &mut rd', 'select(maxfd, &mut rd', WAIT),
    ('posix-wait-seconds-as-ms', 'barb/src/posix/sys.rs', 'usec: ((timeout_ms % 1000) * 1000) as _', 'usec: (timeout_ms % 1000) as _', WAIT),
    ('posix-wait-no-poll-fallback', 'barb/src/posix/sys.rs', '        if f.fd as usize >= FD_SETSIZE {\n            return poll_all(fds, timeout_ms);\n        }\n', '', WAIT),
    ('posix-ops-no-time-limit', 'barb/src/posix/ops.rs', '&& o.deadline != 0 && !o.timed_out && now >= o.deadline {', '&& false {', OPS),
    ('posix-ops-no-grace', 'barb/src/posix/ops.rs', '                o.timed_out = true;\n                o.kill(SIGTERM);', '                o.timed_out = true;\n                o.kill(SIGKILL);', OPS),
    ('posix-ops-cancel-reported', 'barb/src/posix/ops.rs', 'Kind::Proc if !o.cancelled || o.bg => {', 'Kind::Proc => {', OPS),
    ('posix-ops-timeout-is-signal', 'barb/src/posix/ops.rs', '            let exit = if o.timed_out {\n                Exit::TimedOut\n            } else if', '            let exit = if', OPS),
    ('posix-ops-mode-lost', 'barb/src/posix/ops.rs', '.map(|m| m.permissions().mode() & 0o7777).unwrap_or(0o644);', '.map(|_| 0o644).unwrap_or(0o644);', OPS),
    ('posix-ops-symlink-replaced', 'barb/src/posix/ops.rs', 'let target = fs::canonicalize(p).unwrap_or_else(|_| p.to_path_buf());', 'let target = p.to_path_buf();', OPS),
    ('posix-ops-read-unlimited', 'barb/src/posix/ops.rs', '    if md.is_file() && md.len() > READ_LIMIT {', '    if false {', OPS),
    ('posix-ops-leftovers-kept', 'barb/src/posix/ops.rs', '                if !o.bg {\n                    o.kill(SIGKILL);\n                }\n', '', E2E),
    ('posix-ops-tmp-left', 'barb/src/posix/ops.rs', '            let _ = fs::remove_file(&tmp);\n            file_err(id, &e)', '            file_err(id, &e)', OPS),
    ('posix-key-left-in-environment', 'barb/src/posix/mod.rs', '    std::env::remove_var("ANTHROPIC_API_KEY");\n', '', E2E),
    ('posix-no-bracketed-paste', 'barb/src/posix/mod.rs', '        write_all(2, b"\\x1b[?2004h"); // bracket pastes\n        RAW_ON', '        RAW_ON', E2E),
    ('posix-paste-enter-sends', 'barb/src/posix/mod.rs', 'if self.kd.pasting() || self.more_text_waiting() {', 'if self.more_text_waiting() {', E2E),
    ('posix-paste-end-not-sent', 'barb/src/posix/mod.rs', '                if !self.paste.is_empty() && self.scr.ed.line.is_empty() {\n                    self.deliver_paste();\n                }\n', '', E2E),
    ('posix-paste-ignores-rest-of-read', 'barb/src/posix/mod.rs', '&self.rbuf[(self.rbuf_i + 1).min(self.rbuf_n)..self.rbuf_n]', '&self.rbuf[self.rbuf_n..self.rbuf_n]', E2E),
    ('posix-ctrl-c-never-clears', 'barb/src/posix/mod.rs', '            && (!self.scr.ed.line.is_empty() || !self.paste.is_empty() || self.kd.pasting())', '            && false', E2E),
    ('posix-ctrl-d-ends-typed-line', 'barb/src/posix/mod.rs', '                if !self.scr.ed.line.is_empty() {\n                    self.scr.key(Key::Del);\n                } else {\n                    self.end_input();\n                }', '                self.end_input();', E2E),
    # No mutation for ask()'s read_waiting_keys(), for the reason the C table gives: the loop reads
    # stdin before it services curl, so the call only closes a window no test can time.
    ('posix-esc-never-times-out', 'barb/src/posix/mod.rs', '                if let Some(k) = self.kd.timeout() {\n                    self.on_key(k);\n                }', '                let _ = self.kd.timeout();', E2E),
    ('posix-esc-wait-unbounded', 'barb/src/posix/mod.rs', '                if timeout < 0 || timeout > left {\n                    timeout = left;\n                }\n', '                let _ = left;\n', E2E),
    ('posix-suspend-ignored', 'barb/src/posix/mod.rs', "                b'z' => self.suspend(),", "                b'z' => {}", E2E),
    ('posix-suspend-terminal-raw', 'barb/src/posix/mod.rs', '        self.term_restore();\n        sys::on_signal(sys::SIGTSTP', '        sys::on_signal(sys::SIGTSTP', E2E),
    ('posix-suspend-default', 'barb/src/posix/mod.rs', '        catch(sys::SIGTSTP);\n        catch(sys::SIGCONT);', '        catch(sys::SIGCONT);', E2E),
    ('posix-ctrl-backslash-quits', 'barb/src/posix/mod.rs', '    t.cc[sys::VQUIT] = sys::VDISABLE;\n', '', E2E),
    ('posix-resume-not-taken-up', 'barb/src/posix/mod.rs', 'if let Some(d) = &plan.data {', 'if let Some(d) = plan.data.as_ref().filter(|_| false) {', E2E),
    ('posix-saved-model-ignored', 'barb/src/posix/mod.rs', 'if !chosen && !plan.model.is_empty() {', 'if false {', E2E),
    ('posix-question-unasked-allowed', 'barb/src/posix/mod.rs', '            self.scr.say(Style::Error, NOT_ALLOWED);\n            self.answer(id, Answer::No);', '            self.answer(id, Answer::Yes);', E2E),
    # The Windows program: its operations through their own tests, the front end through the
    # end-to-end test.
    ('win-quote-trailing-backslash', 'barb/src/win32/ops.rs', 'b.extend(std::iter::repeat_n(bs_, bs * 2));\n            break;', 'b.extend(std::iter::repeat_n(bs_, bs));\n            break;', WOPS),
    ('win-quote-before-quote', 'barb/src/win32/ops.rs', 'repeat_n(bs_, bs * 2 + 1)', 'repeat_n(bs_, bs + 1)', WOPS),
    ('win-path-drive-kept', 'barb/src/win32/ops.rs', 'let mut t = vec![s[1], b\':\', b\'/\'];', 'let mut t = vec![b\'/\', s[1], b\'/\'];', WOPS),
    ('win-path-tmp-kept', 'barb/src/win32/ops.rs', '} else if n >= 4 && &s[..4] == b"/tmp"', '} else if false && &s[..4] == b"/tmp"', WOPS),
    ('win-path-long-unprefixed', 'barb/src/win32/ops.rs', 'if w.len() >= MAX_PATH - 12 && !w.starts_with(&verbatim) {', 'if false {', WOPS),
    ('win-ops-no-time-limit', 'barb/src/win32/ops.rs', 'if o.kind == Kind::Proc && !o.exited && o.deadline != 0 && !o.timed_out && now >= o.deadline {', 'if false {', WOPS),
    ('win-ops-timeout-is-signal', 'barb/src/win32/ops.rs', '                    let exit = if o.timed_out {\n                        Exit::TimedOut\n                    } else if', '                    let exit = if', WOPS),
    ('win-ops-stderr-lost', 'barb/src/win32/ops.rs', 'si.si.std_error = out;', 'si.si.std_error = 0;', WOPS),
    ('win-ops-pipe-held-open', 'barb/src/win32/ops.rs', '        close(out); // the child has its own', '        let _ = out; // the child has its own', WOPS),
    ('win-ops-read-dir-opened', 'barb/src/win32/ops.rs', '    if attrs & 0x10 != 0 {', '    if false {', WOPS),
    ('win-ops-write-over-dir', 'barb/src/win32/ops.rs', '            return Ev::file(id, Fs::IsDir, "it is a directory");\n', '', WOPS),
    ('win-ops-missing-is-error', 'barb/src/win32/ops.rs', '        2 | 3 | 123 |', '        3 | 123 |', WOPS),
    ('win-ops-leftovers-kept', 'barb/src/win32/ops.rs', '        if !o.bg && o.job != 0 {\n            // SAFETY: the job is ours.\n            unsafe { TerminateJobObject(o.job, 1) };\n        }\n', '', E2E),
    ('win-stdin-event-auto-reset', 'barb/src/win32/mod.rs', 'let ready = unsafe { CreateEventW(null_mut(), 1, 0, null()) };', 'let ready = unsafe { CreateEventW(null_mut(), 0, 0, null()) };', E2E),
    ('win-key-left-in-environment', 'barb/src/win32/mod.rs', '    std::env::remove_var("ANTHROPIC_API_KEY");\n', '', E2E),
    ('win-paste-never-gathers', 'barb/src/win32/mod.rs', '        if self.more_text_waiting() {', '        if false {', E2E),
    ('win-paste-ignores-rest-of-batch', 'barb/src/win32/mod.rs', 'if self.batch.iter().skip(self.batch_i + 1).any(is_text_key) {', 'if false {', E2E),
    ('win-paste-never-ends', 'barb/src/win32/mod.rs', 'let mut p = std::mem::take(&mut self.paste);', 'let mut p = self.paste.clone();', E2E),
    ('win-ctrl-c-never-clears', 'barb/src/win32/mod.rs', 'if !self.agent.busy() && !self.scr.ed.line.is_empty() {', 'if false {', E2E),
    ('win-crlf-two-enters', 'barb/src/win32/mod.rs', 'if ch == 10 && *last == 13 {', 'if false {', WMAIN),
    ('win-resume-not-taken-up', 'barb/src/win32/mod.rs', 'if let Some(d) = &plan.data {', 'if let Some(d) = plan.data.as_ref().filter(|_| false) {', E2E),
    ('win-saved-model-ignored', 'barb/src/win32/mod.rs', 'if !chosen && !plan.model.is_empty() {', 'if false {', E2E),
    ('win-question-unasked-allowed', 'barb/src/win32/mod.rs', '            self.push(Ev::Answer { id, answer: Answer::No });\n            return;', '            self.push(Ev::Answer { id, answer: Answer::Yes });\n            return;', E2E),
]


WIN = os.name == "nt"


def sh(cmd, limit):
    """The command's exit code and output. One that runs past `limit` seconds has hung, which a
    test run would notice too, so it counts as a failure (code -1). Output goes to a file, not
    a pipe, and a hung command is killed with everything it started: a test binary left
    holding a pipe would otherwise keep the wait going forever."""
    with tempfile.TemporaryFile() as out:
        if WIN:
            p = subprocess.Popen(cmd, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT)
        else:
            p = subprocess.Popen(cmd, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = p.wait(timeout=limit)
        except subprocess.TimeoutExpired:
            if WIN:
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], capture_output=True)
            else:
                os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            code = None
        out.seek(0)
        text = out.read().decode("utf-8", "replace")
    if code is None:
        return -1, text + f"\n(hung: stopped after {limit} s)\n"
    return code, text


def run(test):
    if test == E2E:
        code, out = sh([CARGO, "build", "-q", "--release", "-p", "barb"], 1200)
        if code == 0:
            target = os.environ.get("CARGO_TARGET_DIR") or os.path.join(ROOT, "target")
            barb = os.path.join(target, "release", "barb.exe" if WIN else "barb")
            code, more = sh([sys.executable, "tests/e2e.py", barb], 600)
            return ("caught" if code else "SURVIVED"), out + more
    else:
        code, out = sh([CARGO, "test", "-q", *test], 600)
    if "error[" in out or "error: could not compile" in out:
        return "DOES NOT COMPILE", out
    return ("caught" if code else "SURVIVED"), out


def runs_here(path):
    return not path.startswith("barb/src/posix") if WIN else not path.startswith("barb/src/win32")


def main():
    words = sys.argv[1:]
    bad = 0
    for m in MUTATIONS:
        name, path, old, new, test = m[:5]
        nth = m[5] if len(m) > 5 else 1
        if (words and not any(name.startswith(w) for w in words)) or not runs_here(path):
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
