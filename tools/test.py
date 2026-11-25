#!/usr/bin/env python3
"""Run every test: the crates' own (cargo test), then the release binary end to end against a fake
API (tests/e2e.py). The same on Linux, macOS and Windows; cargo must be on PATH, or set CARGO.

    python3 tools/test.py
"""
import os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CARGO = os.environ.get("CARGO") or shutil.which("cargo") or "cargo"


def step(*cmd):
    print("+ " + " ".join(cmd), flush=True)
    if subprocess.run(list(cmd), cwd=ROOT).returncode != 0:
        sys.exit(f"failed: {' '.join(cmd)}")


step(CARGO, "test", "-q", "--workspace")
step(CARGO, "build", "-q", "--release", "-p", "barb")
target = os.environ.get("CARGO_TARGET_DIR") or os.path.join(ROOT, "target")
step(sys.executable, os.path.join("tests", "e2e.py"),
     os.path.join(target, "release", "barb.exe" if os.name == "nt" else "barb"))
print("all tests passed")
