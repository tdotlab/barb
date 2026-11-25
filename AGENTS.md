# Agent instructions

Read these before changing code:

- `docs/ARCHITECTURE.md`: the map. The event/action design, what each file is for, the rules the
  conversation must keep, and which test covers what.
- `TODO.md`: what is next, with a model suggestion for each task.
- `README.md`: what Barb does and how to run it.
- `docs/RUST_PORT.md`: why Barb is in Rust, and what changed from the C version.

`python3 tools/test.py` runs everything: `cargo test --workspace` (the core's scenarios and chaos
test, JSON, the API layer, the line editor, the key decoder, the markdown renderer and the screen,
and the platform's unit tests), then the release binary end to end against a fake API. Run it
before committing; it takes about a minute. It is the same on Windows, where cargo finds MSVC by
itself.

The rules that keep this project what it is:

- No dependencies. Barb is meant to be a from-scratch showcase (T's decision, 2026-11-23): no
  crates, not even small ones, to save effort. Write it, test it. std is fine, and so are
  hand-written declarations of system calls (`barb/src/*/sys.rs`). `curl` for HTTPS is the one
  outside program; keep it that way.
- `core` and `ui` are `#![no_std]` with `alloc` and `#![forbid(unsafe_code)]`, and make no system
  calls, so that a kernel with no libc (RingTail) can host them one day. Anything that touches the
  system belongs in `barb/src/posix` or `barb/src/win32`. Every `unsafe` block there gets a
  `// SAFETY:` comment saying why it is sound.
- The core never sees the API key. The platform adds credentials to the request.
- Every `tool_use` the model makes must get a `tool_result` in the next user message, whatever
  happens in between. The fake world in `core/tests/agent.rs` checks this on every request the
  agent builds; if you change the agent, its chaos test is the one that will tell you.
- Tool results are written for a model to read: say what happened and what to do next.
- New code gets tests, and the tests get mutation-checked: add the ways it could break to the
  table in `tools/mutate.py` and run it (`python3 tools/mutate.py WORD` runs the rows whose names
  start with WORD). Every mutation must be caught, or carry a note saying why it changes nothing.
  It runs the rows for the system it is on: `barb/src/posix` on Linux and macOS, `barb/src/win32`
  on Windows, everything else on both. A mutation that makes a test hang counts as caught once the
  time limit stops it.
- On Windows the pseudo-terminal in `tests/e2e.py` is a ConPTY, which turns every LF it is given
  into a second Enter before Barb sees it; test Windows' CR LF handling with unit tests instead.
