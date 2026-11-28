# Barb architecture

This is the map: what the parts are, how they talk, and the rules that keep the conversation
valid. It does not repeat the code; each source file opens with a comment saying what it is for
and the rules it keeps. When this map and a file disagree, the file is right; fix the map.

## The idea: a core that does no I/O

```
  user ──┐                                    ┌── terminal
         │   events                  actions  │   files, processes, HTTP, timers
         ▼                                    ▼
  ┌──────────────────────────┐        ┌───────────────────────┐
  │ platform (barb/src/posix,│ ─ev──▶ │ core (core/src)       │
  │  barb/src/win32)         │ ◀─act─ │ agent.rs  the state   │
  │ mod.rs  the wait loop    │        │           machine     │
  │ ops.rs  does the work    │        │ tools.rs  the tools   │
  │ sys.rs  the system calls │        │ api.rs    transcript, │
  └──────────────────────────┘        │           request,    │
             │ every action and event │           reply       │
             ▼                        │ json.rs  text.rs      │
  ┌──────────────────────────┐        └───────────────────────┘
  │ screen (ui/src)          │
  │ screen.rs edit.rs        │
  │ keys.rs   md.rs          │
  └──────────────────────────┘
```

The core is a function: `Agent::step(&mut self, &Event) -> Vec<Action>`. It makes no system
calls. The platform feeds it events (a line the user typed, an HTTP reply, a file's contents, a
process that exited, a timer that fired, Ctrl-C) and carries out the actions it returns (show
this, POST that, read this file, run this command, wake me in 60 seconds, ask the user). Between
events the core is not running, so the platform can sleep until the next one. That is the whole
mechanism behind sleeping on a long wait: a sleeping agent gets no events and makes no model calls.

Every operation the core starts (HTTP request, file read or write, process, timer, question) has an
id the core chose, and the event that answers it carries the same id. An event with an id the core
is not waiting for (a late reply to something it cancelled) is ignored. `core/src/agent.rs` has the
full set of events and actions. `Agent::busy()` says whether the agent is in the middle of a turn;
the platform draws its prompt only when it is not. The platform never calls the agent while it is
carrying out a step's actions: what they produce waits in a queue (`common::Ev`, an event that owns
its bytes) and goes in afterwards, one event at a time.

This is also why the same core can run on Windows, macOS and, one day, RingTail: a new platform is
a new module that does the same nine things.

## The crates

`barb-core` and `barb-ui` are `#![no_std]` with `alloc`, and `#![forbid(unsafe_code)]`: they need
an allocator and nothing else, so a kernel with no libc can host them. `barb` is the program and
the only crate that touches the system. It uses std, plus hand-written declarations of the calls
std does not make (`posix/sys.rs`, `win32/sys.rs`); every `unsafe` block in it says why it is
sound. There are no dependencies: `Cargo.lock` lists only the three crates.

Memory: the core uses the global allocator like any Rust code, and an allocation that fails aborts
the program. (The C version had arenas, a sticky out-of-memory state and a test that refused every
allocation in turn; Rust's collections give no way to recover, so that test is gone. Input sizes
are bounded instead: reads are capped at 8 MiB, command output is kept head and tail, JSON nesting
is limited.)

## Files

| File | What it is | Tested by |
|---|---|---|
| `core/src/text.rs` | byte strings and the `cat!` macro | through everything else |
| `core/src/json.rs` | validating pull reader, lookups, UTF-8-safe writer | `core/tests/json.rs` (corpus, UTF-8 cases, fuzz mutations) |
| `core/src/api.rs` | transcript, request body, reply parsing, the saved-conversation format and its repair | `core/tests/api.rs` |
| `core/src/agent.rs` | the state machine: turns, retries, interrupts, background tasks, instructions, saving and resuming | `core/tests/agent.rs` (scenarios, chaos) |
| `core/src/tools.rs` | the six tools and their definitions | `core/tests/agent.rs` |
| `ui/src/edit.rs` | the prompt's line editor: no I/O, it returns the bytes to draw | `ui/tests/edit.rs` (a terminal emulator, both wrapping modes, random keys) |
| `ui/src/keys.rs` | turns a terminal's bytes into keys: escape sequences, UTF-8, bracketed paste; the caller times out a lone Escape | `ui/tests/keys.rs` |
| `ui/src/md.rs` | markdown to styled, wrapped terminal text, one reply at a time; `safe` makes any text safe to print | `ui/tests/md.rs` (the emulator, every construct, hostile bytes, random input) |
| `ui/src/screen.rs` | everything the program shows: the record, the activity line, the footer, the questions. It sees every action and event and keeps time only through the `now` it is given | `ui/tests/screen.rs` (both streams on the emulator, all three modes, random runs) |
| `ui/tests/common/mod.rs` | a small terminal emulator (cursor, wrapping both ways, erases, tabs, the escapes Barb writes) that the ui tests play their output on | |
| `barb/src/common.rs` | what both platforms share: the command line, which saved conversation to take up and where to save, the event queue, a command's kept output (head and tail), curl's arguments and config, its replies | unit tests there |
| `barb/src/posix/sys.rs` | the C library calls std does not wrap, both Linux's and macOS's layouts; `wait`, the loop's select(2) behind poll's interface (Apple's poll() cannot watch a terminal) | unit tests there |
| `barb/src/posix/ops.rs` | processes in their own process groups, curl, files written atomically, timers | unit tests there, `tests/e2e.py` |
| `barb/src/posix/mod.rs` | arguments, the wait loop, the terminal in raw mode with bracketed paste, signals (Ctrl-C, Ctrl-Z, resize) through a self-pipe | `tests/e2e.py` (pipes, a pseudo-terminal, one under a job-control parent for Ctrl-Z) |
| `barb/src/win32/sys.rs` | the Win32 declarations: console input, overlapped named pipes, processes and jobs | through the others |
| `barb/src/win32/ops.rs` | the same on Windows: Git Bash in job objects, overlapped pipes, `curl.exe`, long paths, atomic writes | unit tests there, `tests/e2e.py` |
| `barb/src/win32/mod.rs` | arguments, the `WaitForMultipleObjects` loop, the console in raw mode, Ctrl-C | unit tests there, `tests/e2e.py` (pipes, a hidden console, a ConPTY) |
| `tests/e2e.py` | the release binary against a fake API on localhost | run by `tools/test.py` |
| `tools/mutate.py` | breaks the code in listed ways and checks a test catches each | run by hand after a change; it runs the rows for the system it is on |

## The screen

`ui/src/screen.rs` owns the terminal; the platforms only feed it and write out what it gives
them. They pass it every action before carrying it out (`Screen::action`) and every event before
the agent sees it (`Screen::event`), so it knows what is in flight: the model call, a foreground
command, background tasks, timers, and a retry after a failed request. Before each wait the loop
calls `draw`, and it waits at most `next_ms` so the spinner turns; when nothing is animating that
is -1 and the loop sleeps as before. It has three modes, chosen at startup:

- `Mode::Region` (interactive): the line editor's block is the activity row, a rule, the prompt
  and the footer. Anything printed hides the block first and redraws it after, so output never
  lands on the prompt.
- `Mode::Lite` (stderr is a terminal but this is not a session, as with `-p`): the styled record,
  and the activity on one line that is cleared before anything else is written.
- `Mode::Pipe`: the plain formats, byte for byte; `tests/e2e.py` reads them.

Everything shown that came from outside (the model, a command, a path) goes through `md::safe` or,
for one-line places, a cleaner that turns control characters into spaces; `ui/tests/screen.rs`
checks that every escape reaching the terminal is one of the few kinds it writes itself (colours,
erases, cursor moves), whatever the text held. The token count arrives as a `Say` with
`Style::Usage`; Region puts it in the footer and the other modes print it.

## The rules the conversation must keep

The API refuses a conversation that breaks these, and then refuses every later request too, so
the fake world in `core/tests/agent.rs` checks them on every request the agent builds:

- After an assistant message with tool calls, the next user message starts with a `tool_result` for
  every one of them. This holds when a tool fails, when the user interrupts, when the reply was cut
  off at the token limit.
- No empty text blocks and no empty messages.
- Text that arrives while a step is running (the user typing, a background task ending) is queued
  and joins the next user message after the results. The agent adds to the last user message
  instead of starting a second one.
- Blocks the core does not know (thinking, and whatever comes next) are stored as raw JSON and sent
  back unchanged.
- Bytes that are not valid UTF-8 (a binary file read by a tool) become U+FFFD when written, so one
  such file cannot poison the conversation.

## Project instructions

If the platform sets `instructions` in the config, the first message the user sends does not go
out at once. The agent reads `AGENTS.md` (or else `CLAUDE.md`) with ordinary `Read` actions, then
each file named on a line of its own as `@path` (eight at most, one level deep), and puts the text
in the system prompt; then it sends the message. Anything typed meanwhile is queued as it would be
behind any request, so the turns come out the same with or without instructions. Ctrl-C during the
reads goes on without them. Because it is all actions and events, every platform gets it for free.

## Interrupts

Ctrl-C during a model call cancels it and adds "[barb] The user interrupted you here, before you
replied." to the unanswered message, so the model does not take up the old request when the user
types next. During tool calls, the running call and the ones after it get results that say so.

## Saving and resuming

The format is in `../docs/SESSIONS.md`, because the C version reads and writes the same files. The
core saves with ordinary `Write` actions (atomic, parent directories made): when a request goes out
(not a retry), when a reply is added, and after an interrupt changes the transcript. It keeps the
ids of its own writes, so their `File` events go to `on_saved` and not to a tool; a failure is said
once. The platform decides the path (`common::plan`: a new name, `-c`, `-r`) and hands it over in
`Config::session`; after `/clear` the agent moves to the `-2`, `-3`, ... sibling itself.

`Agent::resume(data, show)` comes before any event. It loads and repairs the transcript
(`Transcript::repair`: a `tool_result` for every call that never got one), shows the last messages
again if asked, and leaves a note that goes first in the next user message. The chaos test resumes
whatever its random run last saved and checks that the next request keeps the rules above.

Thinking blocks stay raw JSON in the transcript; `say_thinking` shows them as `Style::Thinking`
only when `Config::verbose` is on (`-v`, or `/verbose`).

## The model, the effort and thinking

`Config::model` and `Config::effort` are only read when a request is built (`post_request`), so
`/model NAME` and `/effort LEVEL` take effect with the next request, even one built while a turn
is under way. `/model` says `Style::Model` with the bare name, and the screen puts it in the footer.
The effort is one of `api::EFFORTS` (low to max), sent as `output_config.effort`; empty means
the key is left out and the model's default holds. The platform checks `--effort` and
`$BARB_EFFORT` with `common::effort` and refuses a bad level at startup.

`thinking: {type: adaptive, display: summarized}` is sent when verbose is on or an effort is set,
and left out otherwise. With verbose, it is there because Claude models since Opus 4.7 return
thinking blocks with no text unless `display` asks for it, so verbose would show nothing. With an
effort, it is there because some models refuse the higher levels when thinking is off. The cost:
on a model where thinking is optional (DeepSeek takes `adaptive` as "enabled"), `-v` turns it on;
and switching it mid-conversation spoils the message cache for one request. Changing the model
mid-conversation is safe on the Claude API (thinking blocks a model cannot read are dropped, not
refused); other providers' blocks carry no Claude signature, so going from one to the other is
untested.

## Sleeping and waking

`bash` with `run_in_background` starts a process whose output goes to `.barb/tasks/N.log` and
returns at once. When the model ends its turn, the agent is idle with a task pending: it prints one
line saying it is waiting and the platform blocks in `select()` (or `WaitForMultipleObjects`).
When the task exits, the platform delivers an exit event, the agent queues a message about it and
starts a model call. `schedule_wakeup` is the same with a timer. In one-shot mode (`-p`) the agent
finishes once the user will type no more and nothing is pending. A model-call limit per user
message (default 60) stops a loop of wake-ups from spending without end.

## Adding a tool

Add its JSON definition to the tool definitions in `tools.rs`, write it as a small state machine
there (a phase number; keep what it needs between steps in the tool call's state), and add
scenarios to `core/tests/agent.rs`. Write its errors for a model to read: what happened and what to
do next.

## Adding a platform

Carry out the nine actions of `agent.rs` (`Say`, `Http`, `Read`, `Write`, `Spawn`, `Timer`,
`Cancel`, `Ask`, `Done`), turn their results into events, and never call the agent re-entrantly.
`barb/src/posix` is the template. `barb/src/win32` is a second shape: no fork and no signals, so a
command is a suspended process put in a job object, every pipe is overlapped I/O, and the loop
waits in one `WaitForMultipleObjects` on the console, the pipes, the processes and an event the
Ctrl-C handler sets. The C version's `src/ringtail` (now in RingTail's `vendor/barb`) showed how
far the shape stretches on a system with no fork, no environment and no SIGCHLD: commands are jobs
stopped with `kill(-job)`, their end is found by asking whether the task is still there, and HTTP
is a child program reading the key from a pipe. The core wants the clock only through timers the
platform runs, and nothing else. Show the prompt only when `busy()` is false, and set
`instructions` in the config unless the user turned it off.
