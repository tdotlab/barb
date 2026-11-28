# Agent instructions

Barb has two versions in one repository: `rust/` (the full one) and `c/` (the one built into
RingTail). Before changing either, read its own `AGENTS.md` and `ARCHITECTURE.md`; run its tests
from its folder (`python3 tools/test.py` in `rust/`; `make test`, or `build.cmd test` on Windows,
in `c/`).

- `README.md`: what Barb is and the layout.
- `docs/VERSIONS.md`: why there are two versions and where they differ.
- `docs/SESSIONS.md`: the saved-conversation format. It is what the two must agree on; change
  it in both or bump its version.
- `TODO.md`: what is next, for both, with a model suggestion for each task.

Rules for both:

- No dependencies. Barb is a from-scratch showcase (T's decision, 2026-11-23): no library and no
  crate, not even a small one, to save effort. `curl` for HTTPS is the one outside program.
- The versions do not chase each other. A feature goes into Rust first; it goes into C only when
  T asks for it there, and C keeps working on RingTail with nothing else installed. A fix for a bug
  both have goes into both.
- Each version's own rules (the core does no I/O and never sees the API key, every `tool_use` gets
  its `tool_result`, new code gets tests and mutation checks) are in its `AGENTS.md`.
