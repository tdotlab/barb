# Barb

A coding agent for the terminal, in C. You give it a task; it reads and edits files and runs
commands in your working directory, asking before it changes anything. It is built the way
RingTail's programs are: a core that does no I/O, tested on the host, and a thin layer for each
platform. Linux and macOS work today. Windows and RingTail are next (see TODO.md).

Status: v0.1.0, the base. It talks to the Anthropic Messages API and has six tools. It has not
yet been used on a real task with a real key; what is tested is described below.

## Use

```
export ANTHROPIC_API_KEY=...
make
build/barb                      # an interactive session
build/barb -p "fix the failing test in tests/foo.c" --yes
```

Options: `-p TASK` (do one task and exit; `-` reads it from standard input), `-m MODEL` (default
`claude-sonnet-5-5`, or `$BARB_MODEL`), `-y/--yes` (do not ask before writing files or running
commands), `-C DIR`, `--max-turns N` (model calls per message without you typing; default 60).
`ANTHROPIC_BASE_URL` points it at another endpoint (the tests use a fake one on localhost).

In a session, `/clear` forgets the conversation, `/exit` or Ctrl-D quits, and Ctrl-C stops what
is running (twice at the prompt quits). Type while it works and your message is queued.
`barb -p` exits 1 if the API call failed, so a script can tell.

It needs `curl` on the path (it makes the HTTPS requests) and a POSIX shell for the bash tool.

## What it does

Six tools: `read_file` (numbered lines, paged), `write_file`, `edit_file` (an exact string,
which must be unique, in a file it has read), `bash`, `task_stop` and `schedule_wakeup`.
Writing, editing and commands ask first: `y`, `n`, or `a` for always this session.

The part it was written for: anything slow runs as a background task, and the agent goes to sleep.
`bash` with `run_in_background` returns a task number at once and writes the output to
`.barb/tasks/N.log`. While it waits, Barb makes no model calls and uses no CPU (it sleeps in
`poll()`). When the task exits, or a `schedule_wakeup` timer fires, the model is woken with a message.
Add `.barb/` to your `.gitignore`.

It retries overloaded and failed requests with growing delays, caches the system prompt and the
conversation so a long session pays for what is new, and keeps thinking blocks and any block type
it does not know intact.

## Tests

`make test` runs, under the address and undefined-behaviour sanitizers (gcc):

- `test_core`: arena, strings, JSON (valid and invalid corpus, UTF-8 edge cases, nesting limit,
  40,000 fuzz mutations);
- `test_api`: the request body and reply parsing, including every allocation failing;
- `test_agent`: scenarios for every tool and error path, interrupts in every state, a randomized
  chaos test of event orders, and out-of-memory at every allocation point of a long scenario. A fake
  model and a fake world run the whole agent, and every request it builds is checked against the
  rules the API enforces (see docs/ARCHITECTURE.md);
- `tests/e2e.py`: the real binary against a fake API on localhost, including a pseudo-terminal
  session for the permission prompts, Ctrl-C, timeouts, retries, and a check that the API key is
  never on a command line.

The tests were mutation-checked: 28 deliberate breakages, all caught except one that is equivalent
(a redundant check that only improves an error message). One real run against the live API with
an invalid key confirmed TLS and the error path. Not tested: a real conversation with a valid key,
macOS, and long sessions.

## Limits

- Replies are not streamed: you see the answer when it is complete.
- No compaction: when the conversation outgrows the model's context window, the API says so and
  you `/clear`.
- The conversation is not saved between runs.
- No project instructions file is read yet (AGENTS.md, CLAUDE.md).
- A command that is not run in the background is killed, with its children, when it ends or after
  its timeout (default 2 minutes, at most 10). Background tasks are killed when Barb exits.
- `edit_file` and `write_file` follow symbolic links and write atomically, but do not keep file
  ownership.
- The request body, which includes your files' contents, is written to a mode-0600 temporary file
  for curl and removed afterwards. The API key goes to curl on standard input, not the command line.
  If `ANTHROPIC_BASE_URL` is plain http (other than localhost), Barb warns that the key travels in
  the clear.

## Layout

`src/core` is portable C (ISO C11, only `string.h`, `stddef.h` and `stdint.h`): the agent, the
tools, the API layer, JSON, memory. `src/posix` is the Linux and macOS platform. `tests` has the
tests and a fake world. `docs/ARCHITECTURE.md` is the map; `TODO.md` is what comes next.
