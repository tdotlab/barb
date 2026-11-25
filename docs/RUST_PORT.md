# The Rust port

Barb was written in C (about 8,200 lines) and was rewritten in Rust in November 2026, behaviour
for behaviour, on the `rust` branch. This note says why, what changed on the way, and where the
port stands. The C version is in Git history (`main` before the merge) and in RingTail's
`vendor/barb`.

## Why

T's decision (2026-11-23): RingTail's kernel stays in C, but the programs that run on it, Barb
first, move to Rust. The C version kept its memory safe by discipline (arenas, a sticky
out-of-memory state, bounds checked by hand) and by tests; Rust makes most of that the compiler's
job. The cost was the rewrite itself and a larger source: about 10,900 lines of Rust, much of the
difference being tests that are now in the same files as the code.

## What stayed the same

The design did not change: a core with no I/O (`Agent::step(event) -> actions`), a screen that
only turns actions and events into bytes, and one platform module per system. The rules the
conversation keeps, the six tools, the plain output formats that scripts read, the command line
and the config are as they were. The tests were ported with the code, and `tests/e2e.py` runs the
same checks against the Rust binary as it did against the C one. Still no dependencies:
`Cargo.lock` lists only Barb's three crates, the system calls std does not make are declared by
hand, and `curl` is still the one outside program.

## What changed

The build is cargo on every system; `Makefile`, `build.cmd` and the `vcvars` dance are gone (cargo
finds MSVC by itself). `python3 tools/test.py` replaces `make test` and `build.cmd test`.

`core` and `ui` are `#![no_std]` with `alloc` and `#![forbid(unsafe_code)]`. All the `unsafe` is in
the platform modules, each block with a `SAFETY` comment.

Memory: no arenas. The core uses the global allocator, and a failed allocation aborts. The C
version's test that failed every allocation in turn has no Rust equivalent, because the standard
collections cannot report failure; input sizes are bounded instead (8 MiB reads, head-and-tail
command output, a JSON nesting limit).

Found and fixed during the port, in behaviour both versions had:

- The plain-http warning compared the start of `ANTHROPIC_BASE_URL` with `http://localhost`, so
  `http://localhost.evil.com` and `http://localhost@evil.com` got no warning. The host is now
  parsed (userinfo, port, path and IPv6 brackets handled) and only `localhost`, `127.0.0.1` and
  `[::1]` are exempt.
- On Windows a CR LF pair from the console is one Enter. The end-to-end test could not check it,
  because a ConPTY turns every LF it is given into a second Enter before Barb reads it; it is a unit
  test now (`win32::tests::cr_lf_is_one_enter`), and the POSIX end-to-end test checks a CR LF paste.
- The test for atomic writes looked for leftover temporary files in the wrong directory, so a write
  that left one passed. It looks in the right one now.
- What a foreground command leaves running is killed when it ends; a new end-to-end test
  (`test_leftovers_killed`) checks it, where before only the code said so.
- A branch that returned a background task's output was dead (that output goes to the log), and is
  gone.

## Where it stands

`python3 tools/test.py` passes on Windows and Linux. Every row of `tools/mutate.py` is caught on
both: on Windows the common and `win32` rows, on Linux (in Docker) the common and `posix` rows.
macOS is untested, as before. No live run against a real model has been made with the Rust build
yet; that is the first item in `TODO.md`.

The Windows release `barb.exe` is about 310 KB.

RingTail still runs the C version from its `vendor/barb` and no longer gets Barb's changes. The
Rust core and screen are ready for it in principle (`no_std`, an allocator is all they need); a
RingTail target and a platform module are what is missing (see `TODO.md`).
