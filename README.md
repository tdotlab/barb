# Barb

A coding agent for the terminal. You give it a task; it reads and edits files and runs commands in
your working directory, asking before it changes anything, and it sleeps without spending a token
while a long build or test runs in the background. It talks to the Anthropic Messages API and
anything that speaks it (DeepSeek and OpenRouter work).

Barb is written from scratch, with no dependencies: the agent, the JSON and markdown handling, the
line editor and the screen are all its own code. The one outside program it calls is `curl`, for
HTTPS. That is the point of the project.

It comes in two versions, which share a design and go their own ways in features:

- `rust/`: the full one, and the one to use on Linux, macOS and Windows. A cargo workspace with no
  crates. See `rust/README.md`.
- `c/`: the one built into RingTail, a from-scratch operating system, where it runs with
  nothing else installed. It also runs on Linux, macOS and Windows. It gets fixes and the odd
  feature, not every new idea. See `c/README.md`.

`docs/VERSIONS.md` says why there are two and where they differ.

## Quick start

```
cd rust
cargo build --release
export ANTHROPIC_API_KEY=...
target/release/barb
```

## Layout

```
c/      the C version, with its RingTail platform, tests and tools
rust/   the Rust version, with its tests and tools
docs/   what the two share, and where they differ
```

Each version has its own `README.md` (use, tests, limits), `ARCHITECTURE.md` (the map) and
`AGENTS.md` (the rules for changing it). `TODO.md` is what comes next, for both.
