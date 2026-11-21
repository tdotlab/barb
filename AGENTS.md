# Agent instructions

Read these before changing code:

- `docs/ARCHITECTURE.md`: the map. The event/action design, what each file is for, the rules the
  conversation must keep, and which test covers what.
- `TODO.md`: what is next, with a model suggestion for each task.
- `README.md`: what Barb does and how to run it.

`make test` runs everything: the core's unit tests, the agent scenarios and a randomized
chaos test (all under the address and undefined-behaviour sanitizers), then the end-to-end
test against a fake API. Run it before committing. It takes about half a minute.

The rules that keep this project what it is:

- `src/core` is portable C with no system calls: it includes only `stddef.h`, `stdint.h` and
  `string.h`, and calls nothing beyond the string functions. It must stay compilable as ISO C11
  (`-std=c11 -pedantic`) because RingTail, which has no real libc, has to be able to build it.
  Anything that touches the system belongs in `src/posix` (or a future platform folder).
- The core never sees the API key. The platform adds credentials to the request.
- Every `tool_use` the model makes must get a `tool_result` in the next user message, whatever
  happens in between. `tests/harness.h` checks this on every request the agent builds; if you
  change the agent, the chaos test in `tests/test_agent.c` is the one that will tell you.
- Tool results are written for a model to read: say what happened and what to do next.
- New code gets tests, and the tests get mutation-checked: break the code on purpose in a few
  places and make sure a test fails each time. (The harness used for that is not in the repo;
  it is a few lines of Python that edit a source file, run one test target and restore the file.
  Rebuild after restoring, or the next run tests the broken binary.)
- Sanitizer builds need gcc here; this container's clang has no sanitizer runtime. Both
  compilers build the project clean with `-Wall -Wextra -Werror`.
