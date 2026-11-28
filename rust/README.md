# Barb in Rust

The Rust version of Barb, the full one (the C version, the one built into RingTail, is in
`../c`; `../docs/VERSIONS.md` says how they differ). A coding agent for the terminal. You give it a task; it reads and edits files and runs
commands in your working directory, asking before it changes anything. It is built the way
RingTail's programs are: a core that does no I/O, tested on the host, and a thin layer for each
platform. It runs on Linux, on Windows and on macOS (untested).

Barb is written from scratch with no dependencies, and that is the point of the project: the agent,
the JSON and markdown handling, the line editor and the screen are all its own code, with no crate
behind any of them. Building it takes only a Rust toolchain; the few system calls the standard
library does not wrap are declared by hand. The one outside program it calls is `curl`, for HTTPS.
It is built to be portable and tested down to the mutation, not to be small.

Status: v0.2.0. It talks to the Anthropic Messages API (and anything that speaks it) and has six
tools. It has done real work against a live model: multi-file bug fixes, a refactor across a
project, edits that keep CRLF and tabs, paging through a 7,000-line file, background builds and
servers, timers, and interactive sessions with Ctrl-C. (That record is the C version's, which this
one was ported from behaviour for behaviour in November 2026.)

## Use

```
export ANTHROPIC_API_KEY=...
cargo build --release
target/release/barb                      # an interactive session
target/release/barb -p "fix the failing test in tests/foo.c" --yes
```

Options: `-p TASK` (do one task and exit; `-` reads it from standard input), `-m MODEL` (default
`claude-sonnet-5-5`, or `$BARB_MODEL`), `-y/--yes` (do not ask before writing files or running
commands), `-C DIR`, `--max-turns N` (model calls per message without you typing; default 60).
`ANTHROPIC_BASE_URL` points it at another endpoint (the tests use a fake one on localhost).
DeepSeek works through its Anthropic-compatible route: `ANTHROPIC_BASE_URL=https://api.deepseek.com/anthropic`
with `-m deepseek-flash` (checked live: tools, caching, thinking blocks and background wake-ups all behave).
OpenRouter works too: `ANTHROPIC_BASE_URL=https://openrouter.ai/api` with an OpenRouter key and a model
like `-m deepseek/deepseek-chat` (it accepts the `x-api-key` header Barb sends; checked live).

In a session, `/clear` forgets the conversation, `/exit` or Ctrl-D quits, and Ctrl-C stops what
is running (twice at the prompt quits); the model is told it was interrupted. Type while it works
and your message is queued.

On a terminal the prompt sits in a small block at the bottom: a line above it says what is going
on (a spinner while the model thinks or a command runs, with the seconds; the retry countdown after
a failed request; the background tasks and wakeups it is idle on), a rule, the prompt, and a footer
with the model, the directory, the tokens the conversation takes, and a hint for the key that
matters now. The record scrolls above it: each tool call a line with its name in bold and its
result hung under it, errors in red, questions with a diff's added and removed lines coloured. Replies
are rendered from markdown: headings, bold, italics, code, links, lists, quotes, fenced code, rules,
and tables (aligned, or a record per row when too wide), wrapped at words. Text from the model or a
command is never sent to the terminal raw: its control characters are shown as `^[` and the like.
In a locale that is not UTF-8, or on the Linux console, it all falls back to ASCII. When output is
not a terminal, nothing changes: what scripts read is as it was.

The prompt has its own line editor (`ui/src/edit.rs`, on Linux, macOS and Windows): arrows,
Home/End, Ctrl- or Alt-arrows by word, Backspace and Delete, the readline keys (Ctrl-A, E, B, F, K,
U, W, P, N, and Alt-B, Alt-F), Escape to clear the line, Up and Down through the history, and wide
and combining characters wrapped the way the terminal draws them. Ctrl-C at a prompt with text on
it clears the line; on Linux and macOS, Ctrl-Z suspends Barb with the terminal put back as it was. A paste of several
lines is one message, with its newlines: on a terminal with bracketed paste (nearly all of them)
the paste is marked, and otherwise Enters that arrive with more text right behind them are
gathered. A paste that ends in a newline is sent; one that does not waits for you to finish the
line. Lines you type one at a time are separate messages, as ever.
`barb -p` exits 1 if the API call failed, so a script can tell.

Project instructions: before the first request Barb reads `AGENTS.md` from the working directory
(or `CLAUDE.md` if there is none) into the system prompt. A line that is only `@path` is replaced by
that file, so a `CLAUDE.md` of just `@AGENTS.md` works. `--no-instructions` skips this.

It needs `curl` on the path (it makes the HTTPS requests) and a POSIX shell for the bash tool.

## What it does

Six tools: `read_file` (numbered lines, paged), `write_file`, `edit_file` (an exact string,
which must be unique, in a file it has read), `bash`, `task_stop` and `schedule_wakeup`.
Writing, editing and commands ask first: one key, `y`, `n`, or `a` for always this session (Enter
or Escape is no). Anything typed before the question appeared is thrown away, so a line typed ahead
cannot answer it.

The part it was written for: anything slow runs as a background task, and the agent goes to sleep.
`bash` with `run_in_background` returns a task number at once and writes the output to
`.barb/tasks/N.log`. While it waits, Barb makes no model calls and uses no CPU (it sleeps in
`select()`). When the task exits, or a `schedule_wakeup` timer fires, the model is woken with a message.
Add `.barb/` to your `.gitignore`.

It retries overloaded and failed requests with growing delays, caches the system prompt and the
conversation so a long session pays for what is new, and keeps thinking blocks and any block type
it does not know intact.

## On Windows

```
set ANTHROPIC_API_KEY=...
cargo build --release
target\release\barb.exe
```

Cargo builds with the MSVC toolchain (Visual Studio or its Build Tools, with the C++ workload),
which it finds by itself from any prompt. Barb runs commands in Git Bash, so it needs Git for
Windows; it finds the installation through the registry, `git.exe` on the path, or the usual
folders, and `BARB_SHELL` names another `bash.exe`. The model is told it is on Windows and that
paths in its commands are written `/c/Users/...`; the file tools take either spelling. HTTP goes
through the `curl.exe` in System32 (Windows 10 and later). Everything else is the same as on
Linux: the tools, the questions, sleeping on background tasks without a CPU cycle (the loop waits
in `WaitForMultipleObjects`), Ctrl-C.

The console gets the same line editor as Linux; text goes to it as UTF-16, so it shows right
whatever the code page. A paste there is told apart only by its Enters arriving with more text
right behind them.

Differences from Linux: there is no SIGTERM, so a command that is stopped or times out is killed
at once, with everything it started (a job object).

## On RingTail

RingTail ships the C version (`../c`), which needs nothing but RingTail itself. The Rust core and
screen (`barb-core`, `barb-ui`) are `no_std` and need only an allocator, so this version could run
there as an optional program one day; what is missing is a Rust target for RingTail and a platform
module like `barb/src/posix`.

## Tests

`python3 tools/test.py` runs everything, on Linux, macOS or Windows: `cargo test --workspace`,
then the release binary end to end.

- `core/tests/json.rs`: the reader and writer (valid and invalid corpus, UTF-8 edge cases, the
  nesting limit, fuzz mutations);
- `core/tests/api.rs`: the request body and reply parsing;
- `core/tests/agent.rs`: scenarios for every tool and error path, interrupts in every state, and a
  randomized chaos test of event orders. A fake model and a fake world run the whole agent, and
  every request it builds is checked against the rules the API enforces (see ARCHITECTURE.md);
- `ui/tests/edit.rs`: the line editor, played on a terminal emulator in both of the console's
  wrapping modes, with a randomized run of keys;
- `ui/tests/keys.rs`: the decoder that turns a terminal's bytes into keys: escape sequences, UTF-8
  (overlong forms and other non-characters dropped), bracketed paste, and random bytes;
- `ui/tests/md.rs`: the markdown renderer, played on the emulator: every construct, wrapping with
  wide characters, hostile bytes, and random input;
- `ui/tests/screen.rs`: the screen (the record, the activity line, the footer) in each of its three
  modes, with both output streams played on the emulator and a randomized run of events, keys and
  resizes;
- unit tests in `barb/src`: the loop's wait (select behind poll's interface, because Apple's poll()
  cannot watch a terminal), processes and their time limits, files, argument quoting and paths on
  Windows, curl's replies, and the key wiped after use;
- `tests/e2e.py`: the real binary against a fake API on localhost, including a pseudo-terminal
  session (a ConPTY on Windows) for the permission prompts, line editing, pastes, Ctrl-C and
  Ctrl-Z, timeouts, what a command leaves running, retries, and checks that the API key is never on
  a command line and never in the environment of a command the model runs.

`tools/mutate.py` breaks the code on purpose in listed ways and checks that a test fails each
time; every mutation in its table is caught, on Linux and on Windows (it runs the ones for the
system it is on). Not tested: macOS, and sessions long enough to fill the context window.

## Limits

- Replies are not streamed: you see the answer when it is complete.
- No compaction: when the conversation outgrows the model's context window, the API says so and
  you `/clear`.
- The conversation is not saved between runs.
- A command that is not run in the background is killed, with its children, when it ends or after
  its timeout (default 2 minutes, at most 10). Background tasks are killed when Barb exits.
- `edit_file` and `write_file` follow symbolic links and write atomically, but do not keep file
  ownership.
- The request body, which includes your files' contents, is written to a mode-0600 temporary file
  for curl and removed afterwards. The API key goes to curl on standard input, not the command line,
  and it is removed from Barb's own environment once read, so the commands the model runs do not
  inherit it. If `ANTHROPIC_BASE_URL` is plain http (other than localhost), Barb warns that the key travels in
  the clear.

## Layout

A cargo workspace of three crates. `core` (`barb-core`) is the agent, the tools, the API layer and
JSON: `no_std`, no `unsafe`, no system calls. `ui` (`barb-ui`) is the line editor, the key
decoder, the markdown renderer and the screen, as portable as the core. `barb` is the program:
`barb/src/posix` for Linux and macOS, `barb/src/win32` for Windows, and `common.rs` for what they
share; it is the only crate that touches the system, and its `sys.rs` files are the hand-written
declarations of the system calls std does not wrap. `tools` has the test runner and the mutation
runner. `tests` has the end-to-end test. `ARCHITECTURE.md` is the map; `../TODO.md` is what
comes next.
