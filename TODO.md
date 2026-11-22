# To do

Where Barb stands: v0.2.0. The core, six tools and the Linux/macOS platform work, and have done real
tasks against a live model (DeepSeek through its Anthropic-compatible route): multi-file fixes, a
refactor, CRLF edits, a 7,000-line file, background builds and servers, timers, and interactive
sessions. The Windows platform passes the same tests and more. Each task below stands on its own. Model suggestions are for Claude and Codex:
[Claude: Haiku, Sonnet, Opus] [Codex: Luna, Sol].

## Next

**Streaming replies.** The biggest gap in daily use: a long answer arrives after a minute of the
waiting line. Show text as it is written, and lift the non-streaming limit on long replies.
A server-sent-events parser in the core (host-tested against recorded streams), partial tool input
accumulated as JSON, and `ops_http` passing curl's output through in pieces (`EV_HTTP_CHUNK`).
The design is the hard part. Opus high, Codex Sol high.

**Compaction.** When `usage.input_tokens` nears the window, summarize the old part of the
conversation (a model call with no tools) and replace it, keeping recent turns whole. Needs the
transcript rebuilt in a fresh arena so the old one can be freed. Opus high, Codex Sol high.

**Saved sessions.** Write the transcript to `.barb/sessions/` and resume it (`--resume`). Sonnet high.

**Permissions you can set.** A settings file (`.barb/settings.json`) with allow and deny lists, so
`git status` and the test command do not ask every time. Sonnet high.

**More tools.** `glob` and `grep` as tools (the model can use bash, but a dedicated tool has bounded,
sorted output), and `list_dir`. Sonnet high, Codex Luna high.

**Extended thinking.** The request parameter and showing the thinking; the blocks are already kept
and sent back intact. Sonnet medium.

## Platforms

**Windows, what is left.** Barb runs there (src/win32, `build.cmd test`). Still to do: a live task
against the real API from Windows (the tests use the fake one; it needs a key, so it waits for
T's); and the line editor could move to the POSIX prompt too, which has only the terminal's line
discipline and so still sends a multi-line paste as one message per line. Sonnet high for the live
run, Opus medium for moving the editor.

**RingTail, what is left.** Barb runs there (src/ringtail, tested in RingTail's QEMU suite, and
it has done a live task). Still to do: a real HTTPS run from a RingTail machine to the API itself
(the live run so far went through a plain-HTTP relay on the test host, because the cloud container
reaches the internet only through a proxy QEMU's network cannot use); the waiting line and colours
over serial are rough; and MicroPython on RingTail, so the agent can run Python there. Sonnet high
for the HTTPS run, Opus high for MicroPython.

## Tests worth adding

**macOS.** Run `make test` on a Mac. Probably only `ops.c` details (`pgrep`, `/proc` in the e2e key
check, which is Linux-only) need changes.
