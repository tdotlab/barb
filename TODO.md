# To do

Barb has two versions (`docs/VERSIONS.md`): `rust/` gets new features, `c/` stays the one built
into RingTail and gets fixes and what T asks for there. Both are at v0.2.0 and pass their tests on
Windows, with every mutation caught (the C version's newest work has not run on Linux yet). Each task below stands on its own. Model suggestions
are for Claude and Codex: [Claude: Haiku, Sonnet, Opus] [Codex: Luna, Sol].

## Both

**A live run of the Rust build.** One real task against a live model on Linux and one on Windows,
interactive, with a background task and a Ctrl-C, as the C version had. The tests cover the same
ground, but a live run is what found the C version's last bugs. Sonnet medium, Codex Luna medium.

**The port's fixes, in C.** The Rust port found three things the C version still has: the
plain-http warning matches `http://localhost.evil.com` as local (parse the host), the atomic-write
test looks for leftovers in the wrong directory, and no end-to-end test checks that a foreground
command's leftovers are killed. Sonnet medium.

## Rust

**Streaming replies.** The biggest gap in daily use: a long answer arrives after a minute of the
waiting line. Show text as it is written, and lift the non-streaming limit on long replies.
A server-sent-events parser in the core (host-tested against recorded streams), partial tool input
accumulated as JSON, and the platforms' `http` passing curl's output through in pieces (an `Event::HttpChunk`).
The design is the hard part. Opus high, Codex Sol high.

**Permissions you can set.** A settings file (`.barb/settings.json`) with allow and deny lists, so
`git status` and the test command do not ask every time. The same file is where a compaction point
per model belongs (`"compact_at": {"deepseek-flash": "100k"}`), over `--compact-at` and
`$BARB_COMPACT_AT`, which are one number for every model now. Sonnet high.

**Compaction, live.** It is tested against the fake API only. One long real session that crosses
the point, on Claude and on DeepSeek, to see the summary hold up and the cache survive the summary
request. Sonnet medium.

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

**Saved sessions on RingTail.** The C core saves and resumes, but `c/src/ringtail` finds no files
and sets no path: it needs the session listing, `-c`, `-r`, `--sessions`, `--no-save` and `-v` that
`c/src/posix/main.c` has, over RingTail's calls, and dim thinking in its display. Sonnet medium,
Codex Luna high.

**Linux.** Saved sessions, verbose and compaction (2026-11-28) were tested on Windows only;
`src/posix/main.c` has been syntax-checked and never run. `make test`, then `python3 tools/mutate.py`
on Linux. Sonnet medium.

**RingTail's copy.** `c/tools/to_ringtail.sh PATH/TO/RINGTAIL` copies the core and the RingTail
platform into RingTail's `vendor/barb`. Run it after a change here, then RingTail's `make test`.
Still to do from before: a real HTTPS run from a RingTail machine to the API itself.

## Tests worth adding (both)

**macOS.** Run `python3 tools/test.py` on a Mac. The C version was checked under clang on Linux; the Rust one not at all
there. The loop waits with
select() and the e2e test lists processes with `ps` when there is no `/proc`, but neither has run on
a real Mac. Still unchecked: terminal and `curl` details on macOS.
