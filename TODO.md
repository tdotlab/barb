# To do

Where Barb stands: v0.1.0, the base. The core, six tools and the Linux/macOS platform work and are
tested; nothing has been tried on a real task with a real key. Each task below stands on its own.
Model suggestions are for Claude and Codex: [Claude: Haiku, Sonnet, Opus] [Codex: Luna, Sol].

## First, before anything else

**A real session.** Run `barb` with a real key on a small real task (edit a file, run a test,
a background build). Everything so far is tested against fakes and one invalid-key request; this is
where the surprises will be. Not a coding task: T does it, and writes down what felt wrong.

## Next

**Project instructions.** Read `AGENTS.md` (and `CLAUDE.md`) from the working directory into the
system prompt, as T's repos already assume. Small: the platform reads the file (a READ action) before
the first request and the core adds it to the system prompt. Sonnet high, Codex Luna high.

**Streaming replies.** Show text as it is written, and lift the non-streaming limit on long replies.
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

**A mutation runner in the repo.** The one used for the 0.1 checks lives outside it (see AGENTS.md).
Sonnet medium.

**macOS.** Run `make test` on a Mac. Probably only `ops.c` details (`pgrep`, `/proc` in the e2e key
check, which is Linux-only) need changes.
