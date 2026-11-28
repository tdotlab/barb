# To do

Barb has two versions (`docs/VERSIONS.md`): `rust/` gets new features, `c/` stays the one built
into RingTail and gets fixes and what T asks for there. Both are at v0.2.0 and pass their tests on
Linux and Windows, with every mutation caught. Each task below stands on its own. Model suggestions
are for Claude and Codex: [Claude: Haiku, Sonnet, Opus] [Codex: Luna, Sol].

## Both

**A live run of the Rust build.** One real task against a live model on Linux and one on Windows,
interactive, with a background task and a Ctrl-C, as the C version had. The tests cover the same
ground, but a live run is what found the C version's last bugs. Sonnet medium, Codex Luna medium.

**Saved sessions, in C.** Rust has them (`-c`, `-r`, `--sessions`, `--no-save`); C should write
and read the same files, as `docs/SESSIONS.md` describes, including the repair of a cut-off file and
the resume note. The Rust tests (`core/tests/agent.rs`, the `test_sessions` part of
`rust/tests/e2e.py`) are the checklist. Sonnet high, Codex Luna high.

**Verbose, in C.** Rust has `-v` and `/verbose`: thinking blocks shown dim and italic, hidden by
default, and shown again on resume. Sonnet medium.

**The port's fixes, in C.** The Rust port found three things the C version still has: the
plain-http warning matches `http://localhost.evil.com` as local (parse the host), the atomic-write
test looks for leftovers in the wrong directory, and no end-to-end test checks that a foreground
command's leftovers are killed. Sonnet medium.

## Rust

**The model and the effort, from the prompt.** `/model NAME` and an effort level (`--effort`,
`/effort`) sent as `output_config.effort`. Sonnet medium.

**Streaming replies.** The biggest gap in daily use: a long answer arrives after a minute of the
waiting line. Show text as it is written, and lift the non-streaming limit on long replies.
A server-sent-events parser in the core (host-tested against recorded streams), partial tool input
accumulated as JSON, and the platforms' `http` passing curl's output through in pieces (an `Event::HttpChunk`).
The design is the hard part. Opus high, Codex Sol high.

**Compaction.** When `usage.input_tokens` nears the window, summarize the old part of the
conversation (a model call with no tools) and replace it, keeping recent turns whole. The transcript
is a plain `Vec` now, so dropping the old part frees it. Opus high, Codex Sol high.

**Permissions you can set.** A settings file (`.barb/settings.json`) with allow and deny lists, so
`git status` and the test command do not ask every time. Sonnet high.

**More tools.** `glob` and `grep` as tools (the model can use bash, but a dedicated tool has bounded,
sorted output), and `list_dir`. Sonnet high, Codex Luna high.

## Rust: platforms

**Windows, what is left.** Barb runs there (barb/src/win32), and the C version did a live task
against DeepSeek from Windows (2026-11-22: found cl through vswhere, worked around MSYS mangling
`/c`, compiled and ran a program, slept through a background task; 13 model calls).

**The prompt, what is left.** Both prompts share `ui`'s editor. A pasted line longer than
the editor's 8,192 bytes is cut there, and each line of a paste goes into the history as its own
entry, so Up after a paste walks back through its lines; a paste should be one entry, and its
lines unlimited. Sonnet medium.

**The screen, what is left.** `ui/src/screen.rs` gives both prompts an activity line, a footer and a
styled record, and replies are rendered as markdown. Still rough: markdown arrives a whole reply
at a time (streaming would need the renderer fed line by line, which its design allows), and
RingTail still has the C version's plainer display. Check
it by eye on a real terminal or two (Windows Terminal, conhost, a Linux console). Sonnet medium.

**Redundant wake-ups.** In that live run the model set timers to "check" a background task whose
completion message had already woken it, which cost four extra model calls. The bash tool's
description should say that a finished background task wakes the model by itself, so a timer to
check on it is wasted. Sonnet medium; check it live.

**RingTail in Rust, as an optional program.** RingTail ships the C version; the Rust one could be
installed on top. `barb-core` and `barb-ui` are `no_std` and need only an allocator; what is missing
is a Rust target for RingTail (a target spec, `core` and `alloc` built for it, a global allocator
over its heap) and a platform module like `barb/src/posix` for its jobs, `fetch` and `/barb.cfg`.
Opus high for the target, Codex Sol high.

## C

**RingTail's copy.** `c/tools/to_ringtail.sh PATH/TO/RINGTAIL` copies the core and the RingTail
platform into RingTail's `vendor/barb`. Run it after a change here, then RingTail's `make test`.
Still to do from before: a real HTTPS run from a RingTail machine to the API itself.

## Tests worth adding (both)

**macOS.** Run `python3 tools/test.py` on a Mac. The C version was checked under clang on Linux; the Rust one not at all
there. The loop waits with
select() and the e2e test lists processes with `ps` when there is no `/proc`, but neither has run on
a real Mac. Still unchecked: terminal and `curl` details on macOS.
