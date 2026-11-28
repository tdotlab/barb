# Two versions

Barb started as a program for RingTail, in C, and grew into a coding agent that runs anywhere. In
November 2026 it was rewritten in Rust, behaviour for behaviour. The C version was kept rather
than retired, and both now live here: `c/` and `rust/`.

## Why both

RingTail is written in C with no dependencies, and a program built into it has to be too: anything
else would need a compiler or a runtime RingTail does not ship. The C Barb is that program. It runs
on RingTail with nothing else installed, which is worth keeping for its own sake.

The Rust Barb is the full one. RingTail's programs can move to Rust (T's decision, 2026-11-23), and
Rust makes the memory safety the C version kept by discipline the compiler's job, so new features
go there first. It has no crates either, so it is no less from scratch; it needs only a Rust
compiler, as the C one needs only a C compiler.

The two do not chase each other. The C version gets fixes and the features T asks for there; it is
not meant to match the Rust one feature for feature.

## What they share

The design: a core that does no I/O (`step(event) -> actions`), a screen that turns events and
actions into bytes, and a thin platform layer for each system. The rules the conversation keeps,
the six tools, the plain output formats that scripts read, the command line and the settings, and
the format of a saved conversation (`SESSIONS.md`), so either version can take up the other's. Each
version's `ARCHITECTURE.md` describes its own code, and the two read nearly alike.

## Where they differ

| | C (`c/`) | Rust (`rust/`) |
|---|---|---|
| Runs on | Linux, macOS, Windows, RingTail | Linux, macOS, Windows |
| Build | `make`, or `build.cmd` (MSVC) | `cargo build --release` |
| Tests | `make test` (sanitizers), `build.cmd test` | `python3 tools/test.py` |
| Memory | arenas; a failed allocation is reported and survived, and a test fails every allocation in turn | the global allocator; a failed allocation aborts, and input sizes are bounded instead (8 MiB reads, head-and-tail command output, a JSON nesting limit) |
| `unsafe` | all of it, by nature | only in the platform modules, each block with a `SAFETY` comment; `core` and `ui` forbid it |
| Saved conversations, verbose | not yet | `-c`, `-r`, `--sessions`; `-v`, `/verbose` |
| Model and effort in a session | `-m` at startup only | `/model`, `/effort`, `--effort` |
| Size | about 8,200 lines | about 11,700 lines, many of them tests in the same files as the code |

## The port, briefly

Found and fixed during the port, in behaviour both versions had. The Rust version has the fixes; the C version is getting them (see `TODO.md`):

- The plain-http warning compared the start of `ANTHROPIC_BASE_URL` with `http://localhost`, so
  `http://localhost.evil.com` and `http://localhost@evil.com` got no warning. The Rust version
  parses the host (userinfo, port, path and IPv6 brackets) and exempts only `localhost`,
  `127.0.0.1` and `[::1]`.
- The test for atomic writes looked for leftover temporary files in the wrong directory.
- What a foreground command leaves running is killed when it ends; an end-to-end test checks it.
