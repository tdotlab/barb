#!/usr/bin/env python3
"""End-to-end tests: the real barb binary against a fake Messages API on localhost.

The fake server plays a script of replies, checks what barb sends (headers, that the body is
valid UTF-8 JSON, that the API key never appears on a command line), and the test checks what
barb did to the files, how long it waited, and its exit status. Usage: e2e.py PATH_TO_BARB
"""
import itertools, json, os, pty, select, signal, subprocess, sys, tempfile, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BARB = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/barb")
KEY = "test-key-9f3a1c"
checks = failed = 0


def check(cond, what):
    global checks, failed
    checks += 1
    if not cond:
        failed += 1
        print("FAIL:", what)


def text(t, stop="end_turn"):
    return {"type": "message", "role": "assistant", "content": [{"type": "text", "text": t}],
            "stop_reason": stop, "usage": {"input_tokens": 50, "output_tokens": 10}}


_ids = itertools.count()


def tools(*uses, say=None):
    """A reply that calls tools. Ids are unique across the whole run, as the real API's are,
    and are named for the tool and its place in this reply: toolu_bash_0 is the first bash... of
    the reply, with a serial number after it that result_of() ignores."""
    content = [{"type": "text", "text": say}] if say else []
    for i, (name, inp) in enumerate(uses):
        content.append({"type": "tool_use", "id": f"toolu_{name}_{i}_{next(_ids)}", "name": name, "input": inp})
    return {"type": "message", "role": "assistant", "content": content,
            "stop_reason": "tool_use", "usage": {"input_tokens": 50, "output_tokens": 10}}


class Fake:
    """A scripted API. `script` is a list of replies: a dict (200), (status, dict), or a callable(request)."""

    def __init__(self, script):
        self.script, self.requests, self.times, self.problems = list(script), [], [], []
        fake = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a): pass

            def do_POST(self):
                body = self.rfile.read(int(self.headers["Content-Length"]))
                fake.times.append(time.time())
                try:
                    req = json.loads(body.decode("utf-8"))      # strict: invalid UTF-8 is an error
                except Exception as e:
                    fake.problems.append(f"request is not valid UTF-8 JSON: {e}")
                    req = {}
                fake.requests.append(req)
                if self.path != "/v1/messages": fake.problems.append("path " + self.path)
                if self.headers.get("x-api-key") != KEY: fake.problems.append("x-api-key header wrong")
                if self.headers.get("anthropic-version") != "2023-06-01": fake.problems.append("anthropic-version wrong")
                if self.headers.get("content-type") != "application/json": fake.problems.append("content-type wrong")
                # While curl is alive, the key must not be on any command line.
                for pid in os.listdir("/proc"):
                    if pid.isdigit():
                        try:
                            if KEY.encode() in open(f"/proc/{pid}/cmdline", "rb").read():
                                fake.problems.append(f"the API key is on the command line of pid {pid}")
                        except OSError:
                            pass
                if not fake.script:
                    status, out = 500, {"type": "error", "error": {"type": "api_error", "message": "script ran out"}}
                else:
                    item = fake.script.pop(0)
                    if callable(item): item = item(req)
                    status, out = item if isinstance(item, tuple) else (200, item)
                data = json.dumps(out).encode()
                self.send_response(status)
                self.send_header("content-type", "application/json")
                self.send_header("content-length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def stop(self):
        self.server.shutdown()
        self.server.server_close()      # really close the port, so connecting to it is refused


def env_for(fake):
    env = {k: v for k, v in os.environ.items() if "proxy" not in k.lower()}
    env.update(ANTHROPIC_API_KEY=KEY, ANTHROPIC_BASE_URL=fake.url, NO_PROXY="127.0.0.1", no_proxy="127.0.0.1")
    return env


def run(fake, args, cwd, timeout=60, stdin=None):
    t0 = time.time()
    p = subprocess.run([BARB] + args, cwd=cwd, env=env_for(fake), capture_output=True, text=True,
                       timeout=timeout, input=stdin)
    return p, time.time() - t0


def last_user_texts(req):
    out = []
    for blk in req["messages"][-1]["content"]:
        if blk["type"] == "text": out.append(blk["text"])
    return out


def result_of(req, tool_id, last=False):
    """The (content, is_error) of the tool_result whose id starts with tool_id (e.g. "toolu_bash_1");
    the first such, or with last=True the most recent."""
    found = (None, None)
    for m in req["messages"]:
        if m["role"] == "user":
            for b in m["content"]:
                if b.get("type") == "tool_result" and b["tool_use_id"].startswith(tool_id + "_"):
                    found = (b["content"], b.get("is_error", False))
                    if not last: return found
    return found


def no_problems(fake, name):
    for p in fake.problems: check(False, f"{name}: {p}")


def test_files_and_bash():
    d = tempfile.mkdtemp()
    fake = Fake([
        tools(("write_file", {"path": "hello.txt", "content": "héllo wörld ✓\n"}),
              ("bash", {"command": "cat hello.txt; printf '\\377\\376 not utf-8\\n'"}), say="Working on it."),
        tools(("edit_file", {"path": "hello.txt", "old_string": "wörld", "new_string": "there"}),
              ("read_file", {"path": "hello.txt"})),
        text("All done."),
    ])
    # edit_file needs the file read first; writing it counts, as the model knows the contents.
    p, _ = run(fake, ["-p", "make a greeting", "--yes"], d)
    check(p.returncode == 0, f"exit status {p.returncode}: {p.stderr}")
    check(p.stdout == "Working on it.\nAll done.\n", f"stdout was {p.stdout!r}")
    check(open(os.path.join(d, "hello.txt"), encoding="utf-8").read() == "héllo there ✓\n", "file contents")
    check(len(fake.requests) == 3, f"{len(fake.requests)} requests")
    out, err = result_of(fake.requests[1], "toolu_bash_1")
    check(out is not None and "héllo wörld ✓" in out and out.endswith("[exit code: 0]"), f"bash result {out!r}")
    check("�" in out, "invalid bytes became U+FFFD")      # and the server decoded the body strictly
    out, err = result_of(fake.requests[2], "toolu_read_file_1")
    check(out is not None and "1\théllo there ✓" in out, f"read result {out!r}")
    check(fake.requests[0]["model"] == "claude-sonnet-5-5", "default model")
    check(fake.requests[0]["tools"][0]["name"] == "read_file", "tools sent")
    check("Working directory: " + os.path.realpath(d) in fake.requests[0]["system"][0]["text"], "system prompt names the directory")
    no_problems(fake, "files_and_bash")
    fake.stop()


def test_background_wake():
    d = tempfile.mkdtemp()
    fake = Fake([
        tools(("bash", {"command": "sleep 1.2; echo built", "run_in_background": True})),
        text("Building. Waiting for it."),
        text("The build finished."),
    ])
    p, took = run(fake, ["-p", "build it", "--yes"], d)
    check(p.returncode == 0, f"exit status {p.returncode}: {p.stderr}")
    check(len(fake.requests) == 3, f"{len(fake.requests)} requests (a sleeping agent must not call the model)")
    gap = fake.times[2] - fake.times[1] if len(fake.times) == 3 else 0
    check(gap >= 1.0, f"woke after {gap:.2f}s, before the task ended")
    check(took < 8, f"took {took:.1f}s")
    wake = " ".join(last_user_texts(fake.requests[2])) if len(fake.requests) == 3 else ""
    check("[barb] Background task" in wake and "finished with exit code 0" in wake, f"wake message {wake!r}")
    logs = os.listdir(os.path.join(d, ".barb", "tasks"))
    check(len(logs) == 1 and open(os.path.join(d, ".barb/tasks", logs[0])).read() == "built\n", f"log files {logs}")
    check(p.stdout == "Building. Waiting for it.\nThe build finished.\n", f"stdout {p.stdout!r}")
    no_problems(fake, "background_wake")
    fake.stop()


def test_timeout_and_interrupt():
    d = tempfile.mkdtemp()
    fake = Fake([tools(("bash", {"command": "sleep 31.5", "timeout_ms": 1000})), text("it timed out")])
    p, took = run(fake, ["-p", "x", "--yes"], d)
    out, err = result_of(fake.requests[1], "toolu_bash_0") if len(fake.requests) > 1 else (None, None)
    check(out is not None and "[killed: it ran longer than 1 seconds" in out and err, f"timeout result {out!r}")
    check(took < 10, f"timeout took {took:.1f}s")
    check(subprocess.run(["pgrep", "-f", "sleep 31.5"], capture_output=True).returncode != 0, "the timed-out command is gone")
    fake.stop()

    # ...and it is asked to stop (SIGTERM) before it is killed, so it can clean up after itself.
    fake = Fake([tools(("bash", {"command": "trap 'echo asked nicely > term.txt; exit 0' TERM; sleep 31.6 & wait", "timeout_ms": 1000})), text("ok")])
    p, took = run(fake, ["-p", "x", "--yes"], d)
    check(os.path.exists(os.path.join(d, "term.txt")), "a timed-out command is sent SIGTERM before SIGKILL")
    fake.stop()

    # Ctrl-C during a command stops it, calls no model, and leaves nothing running.
    fake = Fake([tools(("bash", {"command": "sleep 32.5"})), text("should never be asked for")])
    pr = subprocess.Popen([BARB, "-p", "x", "--yes"], cwd=d, env=env_for(fake), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(1.5)
    check(subprocess.run(["pgrep", "-f", "sleep 32.5"], capture_output=True).returncode == 0, "the command was running")
    pr.send_signal(signal.SIGINT)
    try:
        pr.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pr.kill(); check(False, "barb did not stop on SIGINT")
    time.sleep(0.5)
    check(len(fake.requests) == 1, f"{len(fake.requests)} requests after interrupt")
    check(subprocess.run(["pgrep", "-f", "sleep 32.5"], capture_output=True).returncode != 0, "the interrupted command was killed")
    fake.stop()

    # Ctrl-C while the model is thinking cancels the request and leaves no curl behind.
    fake = Fake([lambda req: time.sleep(25) or text("too late")])
    pr = subprocess.Popen([BARB, "-p", "x", "--yes"], cwd=d, env=env_for(fake), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(1.5)
    check(subprocess.run(["pgrep", "-f", "connect-timeout"], capture_output=True).returncode == 0, "the request was in flight")
    t0 = time.time()
    pr.send_signal(signal.SIGINT)
    try:
        out, err = pr.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        pr.kill(); out, err = pr.communicate(); check(False, "barb did not stop on SIGINT during a request")
    check(time.time() - t0 < 5, "stopped quickly")
    check("Interrupted." in err, f"stderr {err!r}")
    time.sleep(0.5)
    check(subprocess.run(["pgrep", "-f", "connect-timeout"], capture_output=True).returncode != 0, "curl was killed")
    fake.server.shutdown()          # the handler thread is still asleep; do not wait for it


def test_permissions_and_errors():
    d = tempfile.mkdtemp()
    # Without --yes and with no one to ask, writes and commands are refused, and the model is told.
    fake = Fake([tools(("write_file", {"path": "no.txt", "content": "x"}), ("bash", {"command": "touch also-no"})), text("ok")])
    p, _ = run(fake, ["-p", "x"], d)
    check(not os.path.exists(os.path.join(d, "no.txt")) and not os.path.exists(os.path.join(d, "also-no")), "nothing was written or run")
    out, _ = result_of(fake.requests[1], "toolu_write_file_0")
    check(out is not None and "declined" in out, f"declined message {out!r}")
    check("--yes" in p.stderr, "the user is told how to allow it")
    fake.stop()

    # A transient error is retried; a permanent one fails the run.
    fake = Fake([(529, {"type": "error", "error": {"type": "overloaded_error", "message": "Overloaded"}}), text("recovered")])
    p, took = run(fake, ["-p", "x"], d)
    check(p.returncode == 0 and p.stdout == "recovered\n" and 1.0 <= took < 8, f"retry: status {p.returncode} out {p.stdout!r} in {took:.1f}s")
    check(fake.requests[0] == fake.requests[1], "the retry sent the same request")
    fake.stop()

    fake = Fake([(401, {"type": "error", "error": {"type": "authentication_error", "message": "invalid x-api-key"}})])
    p, _ = run(fake, ["-p", "x"], d)
    check(p.returncode == 1, f"a failed run exits non-zero, got {p.returncode}")
    check("authentication_error" in p.stderr and "ANTHROPIC_API_KEY" in p.stderr, f"stderr {p.stderr!r}")
    fake.stop()

    # The server is not there at all: barb says so, retries, and Ctrl-C stops the waiting.
    fake = Fake([])
    fake.stop()
    pr = subprocess.Popen([BARB, "-p", "x"], cwd=d, env=env_for(fake), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(2.5)
    pr.send_signal(signal.SIGINT)
    try:
        out, err = pr.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        pr.kill(); out, err = pr.communicate(); check(False, "barb did not stop on SIGINT while retrying")
    check("request failed" in err and "retrying in" in err, f"unreachable server, stderr {err!r}")


def test_interactive_pipe():
    d = tempfile.mkdtemp()
    fake = Fake([text("Hi, I am a fake."), text("You said world.")])
    p, _ = run(fake, ["--yes"], d, stdin="hello\nworld\n")
    check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    check("Hi, I am a fake." in p.stdout and "You said world." in p.stdout, f"stdout {p.stdout!r}")
    check(len(fake.requests) == 2, f"{len(fake.requests)} requests")
    check(fake.requests[1]["messages"][-1]["content"][0]["text"] == "world", "second message")
    p, _ = run(fake, ["--yes"], d, stdin="/exit\n")
    check(p.returncode == 0, "/exit")
    fake.stop()


def test_interactive_interrupt():
    d = tempfile.mkdtemp()
    fake = Fake([tools(("bash", {"command": "trap 'echo stopped > int.txt; exit 0' TERM; touch ready.txt; sleep 33.5 & wait"})), text("carrying on")])
    pr = subprocess.Popen([BARB, "--yes"], cwd=d, env=env_for(fake), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    pr.stdin.write("start the long thing\n"); pr.stdin.flush()
    for _ in range(100):
        time.sleep(0.1)
        if os.path.exists(os.path.join(d, "ready.txt")): break      # the trap is installed
    pr.send_signal(signal.SIGINT)
    time.sleep(1.0)                                   # less than the 2 s grace before SIGKILL
    check(os.path.exists(os.path.join(d, "int.txt")), "an interrupted command is sent SIGTERM first")
    check(pr.poll() is None, "the session is still alive after one Ctrl-C")
    check(len(fake.requests) == 1, "no model call after the interrupt")
    pr.stdin.write("now do this\n"); pr.stdin.flush()
    time.sleep(1.0)
    pr.stdin.close()
    out, err = pr.communicate(timeout=10)
    check(pr.returncode == 0, f"exit {pr.returncode}: {err}")
    check("carrying on" in out, f"stdout {out!r}")
    check(len(fake.requests) == 2, f"{len(fake.requests)} requests")
    if len(fake.requests) == 2:
        res, is_err = result_of(fake.requests[1], "toolu_bash_0")
        check(res is not None and "Interrupted by the user" in res, f"interrupted result {res!r}")
        check(last_user_texts(fake.requests[1]) == ["now do this"], f"the next message followed the results: {last_user_texts(fake.requests[1])}")
    no_problems(fake, "interactive_interrupt")
    fake.stop()


def pty_session(argv, cwd, env, script, timeout=30):
    """Run argv on a pseudo-terminal. `script` is a list of (wait_for_text, then_send); returns all output."""
    master, slave = pty.openpty()
    p = subprocess.Popen(argv, cwd=cwd, env=env, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)
    seen, full, deadline = "", "", time.time() + timeout
    steps = list(script)
    while time.time() < deadline and p.poll() is None:
        r, _, _ = select.select([master], [], [], 0.2)
        if r:
            try:
                data = os.read(master, 4096).decode("utf-8", "replace")
            except OSError:
                break
            seen += data
            full += data
        if steps and steps[0][0] in seen:
            os.write(master, steps[0][1].encode())
            seen = seen.replace(steps[0][0], "<matched>", 1)
            steps.pop(0)
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
    try:
        while True:
            r, _, _ = select.select([master], [], [], 0.2)
            if not r: break
            full += os.read(master, 4096).decode("utf-8", "replace")
    except OSError:
        pass
    os.close(master)
    return p.returncode, full, steps


def test_interactive_permissions():
    d = tempfile.mkdtemp()
    fake = Fake([
        tools(("write_file", {"path": "yes.txt", "content": "1"})),
        tools(("write_file", {"path": "no.txt", "content": "2"}), ("bash", {"command": "echo ran > ran.txt"})),
        tools(("bash", {"command": "echo again > again.txt"})),
        text("finished"),
    ])
    code, out, left = pty_session([BARB], d, env_for(fake), [
        ("> ", "make files\n"),
        ("Allow?", "y\n"),          # yes.txt: allowed
        ("Allow?", "n\n"),          # no.txt: refused
        ("Allow?", "a\n"),          # the bash command: always
        ("finished", "\x04"),       # Ctrl-D at the prompt ends the session
    ])
    check(not left, f"every step of the conversation happened; left: {[x[0] for x in left]}\n{out!r}")
    check(open(os.path.join(d, "yes.txt")).read() == "1" if os.path.exists(os.path.join(d, "yes.txt")) else False, "yes.txt written after y")
    check(not os.path.exists(os.path.join(d, "no.txt")), "no.txt not written after n")
    check(os.path.exists(os.path.join(d, "ran.txt")), "the command ran after a (always)")
    check(os.path.exists(os.path.join(d, "again.txt")), "an always-allowed command ran without asking again")
    check(out.count("Allow?") == 3, f"asked {out.count('Allow?')} times")
    check("write_file wants to:" in out and "create yes.txt" in out, "the question says what it wants to do")
    check(code == 0, f"exit {code}")
    if len(fake.requests) >= 3:
        res, _ = result_of(fake.requests[2], "toolu_write_file_0", last=True)   # the second round's, no.txt
        check(res is not None and "declined" in res, f"model told: {res!r}")
        res, _ = result_of(fake.requests[2], "toolu_bash_1")
        check(res is not None and res.endswith("[exit code: 0]"), f"the command ran after a: {res!r}")
    fake.stop()


def test_startup_errors():
    d = tempfile.mkdtemp()
    p = subprocess.run([BARB, "-p", "x"], cwd=d, env={"PATH": os.environ["PATH"]}, capture_output=True, text=True)
    check(p.returncode == 2 and "ANTHROPIC_API_KEY" in p.stderr, "missing key")
    p = subprocess.run([BARB, "--nonsense"], cwd=d, env={"PATH": os.environ["PATH"]}, capture_output=True, text=True)
    check(p.returncode == 2, "bad option")
    p = subprocess.run([BARB, "--version"], capture_output=True, text=True)
    check(p.returncode == 0 and p.stdout.startswith("barb "), "version")


if __name__ == "__main__":
    for t in (test_startup_errors, test_files_and_bash, test_background_wake, test_timeout_and_interrupt,
              test_permissions_and_errors, test_interactive_pipe, test_interactive_interrupt, test_interactive_permissions):
        t()
    print(f"e2e: {checks} checks, {failed} failed")
    sys.exit(1 if failed else 0)
