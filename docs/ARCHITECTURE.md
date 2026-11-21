# Barb architecture

This is the map: what the parts are, how they talk, and the rules that keep the conversation
valid. It does not repeat the code; each source file opens with a comment saying what it is for
and the rules it keeps. When this map and a file disagree, the file is right; fix the map.

## The idea: a core that does no I/O

```
  user ──┐                                   ┌── terminal
         │   events                 actions  │   files, processes, HTTP, timers
         ▼                                   ▼
  ┌─────────────────────────┐        ┌───────────────────────┐
  │ platform (src/posix)    │ ─ev──▶ │ core (src/core)       │
  │ main.c  the poll loop   │ ◀─act─ │ agent.c  the state    │
  │ ops.c   does the work   │        │          machine      │
  └─────────────────────────┘        │ tools.c  the tools    │
                                     │ api.c    transcript,  │
                                     │          request,reply│
                                     │ json.c str.c arena.c  │
                                     └───────────────────────┘
```

The core is a function: `agent_step(agent, event) -> actions`. It makes no system calls. The
platform feeds it events (a line the user typed, an HTTP reply, a file's contents, a process that
exited, a timer that fired, Ctrl-C) and carries out the actions it returns (show this, POST that,
read this file, run this command, wake me in 60 seconds, ask the user). Between events the core is
not running, so the platform can sleep until the next one. That is the whole mechanism behind
sleeping on a long wait: a sleeping agent gets no events and makes no model calls.

Every operation the core starts (HTTP request, file read or write, process, timer, question) has an
id the core chose, and the event that answers it carries the same id. An event with an id the core
is not waiting for (a late reply to something it cancelled) is ignored. `agent.h` has the full table
of events and actions.

This is also why the same core can run on Windows, macOS and RingTail: a new platform is a new
folder that does the same eight things.

## Memory

The core never calls malloc. The platform gives it a `mem_t`, and everything comes out of arenas:
`life` (the conversation and anything that lasts), `tmp` (one step's work, emptied at the start of
the next step; the actions returned by a step live in it) and `ta` (one tool call's work, emptied
when the call ends). A failed allocation is sticky and fatal: the agent says so and stops, and the
out-of-memory test tries it at every allocation point of a long scenario.

## Files

| File | What it is | Tested by |
|---|---|---|
| `src/core/arena.c` | chunked bump allocator | `test_core` |
| `src/core/str.c` | slices and a growing buffer | `test_core` |
| `src/core/json.c` | validating pull reader, lookups, UTF-8-safe writer | `test_core` (corpus, UTF-8 cases, 40,000 fuzz mutations) |
| `src/core/api.c` | transcript, request body, reply parsing | `test_api` |
| `src/core/agent.c` | the state machine: turns, retries, interrupts, background tasks | `test_agent` (scenarios, chaos, out of memory) |
| `src/core/tools.c` | the six tools and their definitions | `test_agent` |
| `src/posix/ops.c` | processes, curl, files, timers | `tests/e2e.py` |
| `src/posix/main.c` | arguments, poll loop, terminal | `tests/e2e.py` (pipes and a pseudo-terminal) |
| `tests/harness.h` | a fake world (scripted model, in-memory files, scripted processes) | used by `test_agent` |

## The rules the conversation must keep

The API refuses a conversation that breaks these, and then refuses every later request too, so
`tests/harness.h` checks them on every request the agent builds:

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

## Sleeping and waking

`bash` with `run_in_background` starts a process whose output goes to `.barb/tasks/N.log` and
returns at once. When the model ends its turn, the agent is idle with a task pending: it prints one
line saying it is waiting and the platform blocks in `poll()`. When the task exits, the platform
delivers an exit event, the agent queues a message about it and starts a model call. `schedule_wakeup`
is the same with a timer. In one-shot mode (`-p`) the agent finishes once the user will type no
more and nothing is pending. A model-call limit per user message (default 60) stops a loop of
wake-ups from spending without end.

## Adding a tool

Add its JSON definition to `TOOLS_JSON`, write it as a small state machine in `tools.c` (a phase
number in `tc.phase`; keep what it needs between steps in `agent->ta`), and add scenarios to
`tests/test_agent.c`. Write its errors for a model to read: what happened and what to do next.

## Adding a platform

Implement the eight actions of `agent.h` (say, HTTP, read, write, spawn, timer, cancel, ask, done),
turn their results into events, and never call the agent re-entrantly. `src/posix/ops.h` is the
interface the POSIX version uses; it is a good template. The core wants a `mem_t`, the clock only
through timers the platform runs, and nothing else.
