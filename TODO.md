# To do

Where Barb stands: v0.2.0. The core, six tools and the Linux/macOS platform work, and have done real
tasks against a live model (DeepSeek through its Anthropic-compatible route): multi-file fixes, a
refactor, CRLF edits, a 7,000-line file, background builds and servers, timers, and interactive
sessions. Each task below stands on its own. Model suggestions are for Claude and Codex:
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

**Windows.** `ops.c` again with CreateProcess and a job object (the process group), `curl.exe` or
WinHTTP for HTTP, and the console. The core builds as is. Opus high, Codex Sol high: the process
and signal semantics are where the quiet mistakes are.

**RingTail.** The core needs a libc-free build (it already is: `string.h` only). The platform needs
what RingTail lacks today: an HTTPS POST with headers and a body (`fetch` is GET-only; a small program
or a `fetch -d`), a way to wait on several things at once (child exit, a timer, input), and file
operations over its own system calls. Read `docs/ARCHITECTURE.md`, "Adding a platform". Sonnet high
for `fetch -d`, Opus high for the platform. Later: MicroPython on RingTail, so the agent can run
Python there.

## Tests worth adding

**macOS.** Run `make test` on a Mac. Probably only `ops.c` details (`pgrep`, `/proc` in the e2e key
check, which is Linux-only) need changes.
