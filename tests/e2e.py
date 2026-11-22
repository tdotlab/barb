#!/usr/bin/env python3
"""End-to-end tests: the real barb binary against a fake Messages API on localhost.

The fake server plays a script of replies, checks what barb sends (headers, that the body is
valid UTF-8 JSON, that the API key never appears on a command line), and the test checks what
barb did to the files, how long it waited, and its exit status. Usage: e2e.py PATH_TO_BARB

It runs on Linux, macOS and Windows. What differs is gathered in one place below: how to list
the command lines of running processes, how to press Ctrl-C at barb, and how to give it a
terminal (a pty, or a Windows pseudo console).
"""
import itertools, json, os, re, signal, subprocess, sys, tempfile, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WIN = os.name == "nt"
BARB = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ("build/barb.exe" if WIN else "build/barb"))
TEXT = dict(text=True, encoding="utf-8", errors="replace")     # barb speaks UTF-8 whatever the locale
KEY = "test-key-9f3a1c"
checks = failed = 0


def check(cond, what):
    global checks, failed
    checks += 1
    if not cond:
        failed += 1
        print("FAIL:", what)


# ---- what differs between the platforms ----

if WIN:
    import ctypes
    from ctypes import wintypes as W
    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _nt = ctypes.WinDLL("ntdll")
    _k32.OpenProcess.restype = W.HANDLE
    _k32.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    _k32.CloseHandle.argtypes = [W.HANDLE]
    _k32.K32EnumProcesses.argtypes = [ctypes.c_void_p, W.DWORD, ctypes.POINTER(W.DWORD)]
    _nt.NtQueryInformationProcess.argtypes = [W.HANDLE, W.ULONG, ctypes.c_void_p, W.ULONG, ctypes.POINTER(W.ULONG)]

    def cmdlines():
        """(pid, command line) for every process we may look at."""
        pids = (W.DWORD * 16384)()
        got = W.DWORD()
        _k32.K32EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(got))
        out, buf = [], ctypes.create_string_buffer(1 << 16)
        for pid in pids[:got.value // 4]:
            h = _k32.OpenProcess(0x1000, False, pid)        # PROCESS_QUERY_LIMITED_INFORMATION
            if not h:
                continue
            n = W.ULONG()
            ok = _nt.NtQueryInformationProcess(h, 60, buf, len(buf), ctypes.byref(n)) == 0   # ProcessCommandLineInformation
            _k32.CloseHandle(h)
            if ok:      # a UNICODE_STRING: its length in bytes, then a pointer into buf
                length = int.from_bytes(buf.raw[0:2], "little")
                ptr = int.from_bytes(buf.raw[8:16], "little")
                if ptr:
                    out.append((pid, ctypes.wstring_at(ptr, length // 2)))
        return out
else:
    def cmdlines():
        """(pid, command line) for every process we may look at."""
        out = []
        for pid in os.listdir("/proc"):
            if pid.isdigit():
                try:
                    raw = open(f"/proc/{pid}/cmdline", "rb").read()
                except OSError:
                    continue
                out.append((int(pid), raw.replace(b"\0", b" ").decode("utf-8", "replace")))
        return out


def running(*words):
    """Whether some other process has all these words in its command line (pgrep -f)."""
    return any(pid != os.getpid() and all(w in c for w in words) for pid, c in cmdlines())


def start(args, cwd, env, stdin=None):
    """barb in the background, ready to be interrupted. On Windows it gets a (hidden) console of
    its own, because Ctrl-C there goes to everything attached to a console."""
    kw = {}
    if WIN:
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 0
        kw = dict(creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=si)
    return subprocess.Popen([BARB] + args, cwd=cwd, env=env, stdin=stdin, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, **TEXT, **kw)


def interrupt(pr):
    """Ctrl-C, as the user would press it."""
    if not WIN:
        pr.send_signal(signal.SIGINT)
        return
    # A helper joins barb's console and presses Ctrl-C there (ignoring it itself).
    code = ("import ctypes, sys; k = ctypes.windll.kernel32; k.FreeConsole(); "
            "k.AttachConsole(int(sys.argv[1])) or sys.exit('cannot attach to the console'); "
            "k.SetConsoleCtrlHandler(None, 1); k.GenerateConsoleCtrlEvent(0, 0)")
    subprocess.run([sys.executable, "-c", code, str(pr.pid)], check=True)


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
                for pid, c in cmdlines():
                    if KEY in c:
                        fake.problems.append(f"the API key is on the command line of pid {pid}")
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

        class Server(ThreadingHTTPServer):
            def handle_error(self, request, address):
                # A curl that barb stopped (an interrupt, a timeout) hangs up mid-reply: expected.
                if not isinstance(sys.exc_info()[1], (ConnectionResetError, BrokenPipeError)):
                    super().handle_error(request, address)

        self.server = Server(("127.0.0.1", 0), H)
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
    p = subprocess.run([BARB] + args, cwd=cwd, env=env_for(fake), capture_output=True, **TEXT,
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


def test_key_not_inherited():
    """The key lives in the platform layer only: a command the model runs must not inherit it."""
    d = tempfile.mkdtemp()
    fake = Fake([
        tools(("bash", {"command": "echo \"[$ANTHROPIC_API_KEY]\"; env | grep -c ANTHROPIC_API_KEY"})),
        text("done"),
    ])
    p, _ = run(fake, ["-p", "show me your environment", "--yes"], d)
    check(p.returncode == 0, f"exit status {p.returncode}: {p.stderr}")
    out, err = result_of(fake.requests[1], "toolu_bash_0") if len(fake.requests) == 2 else (None, None)
    check(out is not None and KEY not in out, f"the key reached a command: {out!r}")
    check(out is not None and "[]" in out, f"variable should be empty: {out!r}")
    check(fake.requests and fake.requests[0]["model"] and not any(KEY in json.dumps(r) for r in fake.requests),
          "the key is in a request body")
    no_problems(fake, "key_not_inherited")
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
    check(out is not None and "[killed: it ran longer than its limit of 1 second." in out and err, f"timeout result {out!r}")
    check(took < 10, f"timeout took {took:.1f}s")
    check(not running("sleep", "31.5"), "the timed-out command is gone")
    fake.stop()

    # ...and it is asked to stop (SIGTERM) before it is killed, so it can clean up after itself.
    # (Windows has no SIGTERM: there a command out of time is killed at once.)
    if not WIN:
        fake = Fake([tools(("bash", {"command": "trap 'echo asked nicely > term.txt; exit 0' TERM; sleep 31.6 & wait", "timeout_ms": 1000})), text("ok")])
        p, took = run(fake, ["-p", "x", "--yes"], d)
        check(os.path.exists(os.path.join(d, "term.txt")), "a timed-out command is sent SIGTERM before SIGKILL")
        fake.stop()

    # Ctrl-C during a command stops it, calls no model, and leaves nothing running.
    fake = Fake([tools(("bash", {"command": "sleep 32.5"})), text("should never be asked for")])
    pr = start(["-p", "x", "--yes"], d, env_for(fake))
    time.sleep(1.5)
    check(running("sleep", "32.5"), "the command was running")
    interrupt(pr)
    try:
        pr.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pr.kill(); check(False, "barb did not stop on SIGINT")
    time.sleep(0.5)
    check(len(fake.requests) == 1, f"{len(fake.requests)} requests after interrupt")
    check(not running("sleep", "32.5"), "the interrupted command was killed")
    fake.stop()

    # Ctrl-C while the model is thinking cancels the request and leaves no curl behind.
    fake = Fake([lambda req: time.sleep(25) or text("too late")])
    pr = start(["-p", "x", "--yes"], d, env_for(fake))
    time.sleep(1.5)
    check(running("connect-timeout"), "the request was in flight")
    t0 = time.time()
    interrupt(pr)
    try:
        out, err = pr.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        pr.kill(); out, err = pr.communicate(); check(False, "barb did not stop on SIGINT during a request")
    check(time.time() - t0 < 5, "stopped quickly")
    check("Interrupted." in err, f"stderr {err!r}")
    time.sleep(0.5)
    check(not running("connect-timeout"), "curl was killed")
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
    pr = start(["-p", "x"], d, env_for(fake))
    time.sleep(2.5)
    interrupt(pr)
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
    pr = start(["--yes"], d, env_for(fake), stdin=subprocess.PIPE)
    pr.stdin.write("start the long thing\n"); pr.stdin.flush()
    for _ in range(100):
        time.sleep(0.1)
        if os.path.exists(os.path.join(d, "ready.txt")): break      # the trap is installed
    interrupt(pr)
    time.sleep(1.0)                                   # less than the 2 s grace before SIGKILL
    if not WIN:                                       # (no SIGTERM there, as above)
        check(os.path.exists(os.path.join(d, "int.txt")), "an interrupted command is sent SIGTERM first")
    check(pr.poll() is None, "the session is still alive after one Ctrl-C")
    check(len(fake.requests) == 1, "no model call after the interrupt")
    pr.stdin.write("now do this\n"); pr.stdin.flush()
    time.sleep(1.0)
    try:
        out, err = pr.communicate(timeout=10)      # closes its stdin: barb sees the end of its input
    except subprocess.TimeoutExpired:
        pr.kill(); out, err = pr.communicate(); check(False, "barb did not end at the end of its input")
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
    """Run argv on a terminal. `script` is a list of (wait_for_text, then_send); returns
    (exit status, all output, the steps that never happened)."""
    return (_conpty_session if WIN else _pty_session)(argv, cwd, env, script, timeout)


def _pty_session(argv, cwd, env, script, timeout):
    import pty, select
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


def _conpty_session(argv, cwd, env, script, timeout):
    """The same on Windows, with a pseudo console (ConPTY) for the terminal."""
    import queue
    k, H, D = _k32, W.HANDLE, W.DWORD

    class COORD(ctypes.Structure):
        _fields_ = [("X", ctypes.c_short), ("Y", ctypes.c_short)]

    class STARTUPINFOEX(ctypes.Structure):
        _fields_ = [("cb", D), ("lpReserved", W.LPWSTR), ("lpDesktop", W.LPWSTR), ("lpTitle", W.LPWSTR),
                    ("dwX", D), ("dwY", D), ("dwXSize", D), ("dwYSize", D), ("dwXCountChars", D),
                    ("dwYCountChars", D), ("dwFillAttribute", D), ("dwFlags", D), ("wShowWindow", W.WORD),
                    ("cbReserved2", W.WORD), ("lpReserved2", ctypes.c_void_p), ("hStdInput", H),
                    ("hStdOutput", H), ("hStdError", H), ("lpAttributeList", ctypes.c_void_p)]

    class PROCESS_INFORMATION(ctypes.Structure):
        _fields_ = [("hProcess", H), ("hThread", H), ("dwProcessId", D), ("dwThreadId", D)]

    k.CreatePipe.argtypes = [ctypes.POINTER(H), ctypes.POINTER(H), ctypes.c_void_p, D]
    k.CreatePseudoConsole.argtypes = [COORD, H, H, D, ctypes.POINTER(ctypes.c_void_p)]
    k.CreatePseudoConsole.restype = ctypes.c_long
    k.ClosePseudoConsole.argtypes = [ctypes.c_void_p]
    k.InitializeProcThreadAttributeList.argtypes = [ctypes.c_void_p, D, D, ctypes.POINTER(ctypes.c_size_t)]
    k.UpdateProcThreadAttribute.argtypes = [ctypes.c_void_p, D, ctypes.c_size_t, ctypes.c_void_p,
                                            ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p]
    k.CreateProcessW.argtypes = [W.LPCWSTR, W.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, W.BOOL, D,
                                 ctypes.c_void_p, W.LPCWSTR, ctypes.c_void_p, ctypes.c_void_p]
    k.ReadFile.argtypes = [H, ctypes.c_void_p, D, ctypes.POINTER(D), ctypes.c_void_p]
    k.WriteFile.argtypes = [H, ctypes.c_void_p, D, ctypes.POINTER(D), ctypes.c_void_p]
    k.WaitForSingleObject.argtypes = [H, D]
    k.GetExitCodeProcess.argtypes = [H, ctypes.POINTER(D)]
    k.TerminateProcess.argtypes = [H, ctypes.c_uint]

    in_r, in_w, out_r, out_w = H(), H(), H(), H()
    k.CreatePipe(ctypes.byref(in_r), ctypes.byref(in_w), None, 0)
    k.CreatePipe(ctypes.byref(out_r), ctypes.byref(out_w), None, 0)
    hpc = ctypes.c_void_p()
    hr = k.CreatePseudoConsole(COORD(120, 40), in_r, out_w, 0, ctypes.byref(hpc))
    k.CloseHandle(in_r)
    k.CloseHandle(out_w)
    if hr != 0:
        raise OSError(f"CreatePseudoConsole failed: {hr & 0xffffffff:#x}")
    size = ctypes.c_size_t()
    k.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))
    attrs = ctypes.create_string_buffer(size.value)
    k.InitializeProcThreadAttributeList(attrs, 1, 0, ctypes.byref(size))
    k.UpdateProcThreadAttribute(attrs, 0, 0x00020016, hpc, ctypes.sizeof(hpc), None, None)   # the pseudo console
    si = STARTUPINFOEX()
    si.cb = ctypes.sizeof(si)
    si.dwFlags = 0x100              # STARTF_USESTDHANDLES with none given: ours must not leak in
    si.lpAttributeList = ctypes.cast(attrs, ctypes.c_void_p)
    pi = PROCESS_INFORMATION()
    block = ctypes.create_unicode_buffer("".join(f"{a}={b}\0" for a, b in sorted(env.items())) + "\0")
    if not k.CreateProcessW(None, ctypes.create_unicode_buffer(subprocess.list2cmdline(argv)), None, None, False,
                            0x00080000 | 0x400,     # EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT
                            block, cwd, ctypes.byref(si), ctypes.byref(pi)):
        err = ctypes.WinError(ctypes.get_last_error())
        k.ClosePseudoConsole(hpc)
        raise err
    k.CloseHandle(pi.hThread)

    chunks = queue.Queue()

    def reader():
        buf, n = ctypes.create_string_buffer(4096), D()
        while k.ReadFile(out_r, buf, 4096, ctypes.byref(n), None) and n.value:
            chunks.put(buf.raw[:n.value])
        chunks.put(None)

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    raw = b""

    def take(wait):
        nonlocal raw
        try:
            c = chunks.get(timeout=wait)
        except queue.Empty:
            return
        if c:
            raw += c

    def send(s):
        b, n = s.replace("\n", "\r").encode(), D()     # a terminal's Enter is CR
        k.WriteFile(in_w, b, len(b), ctypes.byref(n), None)

    steps, matched, deadline = list(script), 0, time.time() + timeout
    while time.time() < deadline and k.WaitForSingleObject(pi.hProcess, 0) != 0:
        take(0.2)
        seen = raw.decode("utf-8", "replace")
        if steps and steps[0][0] in seen[matched:]:
            matched = seen.index(steps[0][0], matched) + len(steps[0][0])
            send(steps[0][1])
            steps.pop(0)
    if k.WaitForSingleObject(pi.hProcess, 10000) != 0:
        k.TerminateProcess(pi.hProcess, 1)
        k.WaitForSingleObject(pi.hProcess, 5000)
    code = D()
    k.GetExitCodeProcess(pi.hProcess, ctypes.byref(code))
    k.CloseHandle(pi.hProcess)
    end = time.time() + 1
    while time.time() < end:
        take(0.1)
    k.ClosePseudoConsole(hpc)       # ends the output; the reader sees it close
    t.join(5)
    while not chunks.empty():
        take(0)
    k.CloseHandle(in_w)
    k.CloseHandle(out_r)
    return code.value, raw.decode("utf-8", "replace"), steps


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
        ("Allow?", "y"),            # yes.txt: allowed, with one key and no Enter
        ("Allow?", "n"),            # no.txt: refused
        ("Allow?", "a"),            # the bash command: always
        ("finished", "\x04"),       # Ctrl-D at the prompt ends the session
    ])
    check(not left, f"every step of the conversation happened; left: {[x[0] for x in left]}\n{out!r}")
    check(open(os.path.join(d, "yes.txt")).read() == "1" if os.path.exists(os.path.join(d, "yes.txt")) else False, "yes.txt written after y")
    check(not os.path.exists(os.path.join(d, "no.txt")), "no.txt not written after n")
    check(os.path.exists(os.path.join(d, "ran.txt")), "the command ran after a (always)")
    check(os.path.exists(os.path.join(d, "again.txt")), "an always-allowed command ran without asking again")
    check(out.count("Allow?") == 3, f"asked {out.count('Allow?')} times")
    lines = [re.sub(r"\x1b\[[0-9;]*[mK]", "", l) for l in out.replace("\r", "\n").split("\n")]
    busy = [l for l in lines if l.startswith("> [tokens") or l.startswith(">   ")]
    check(not busy, f"no prompt is drawn while the agent is still working: {busy}")
    # A pseudo console may move a colour change ahead of the line end, so look past the escapes.
    plain = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", out).replace("\r\n", "\n")
    check("yes\n" in plain and "always\n" in plain, "the answer is echoed as a word")
    check("write_file wants to:" in out and "create yes.txt" in out, "the question says what it wants to do")
    check(code == 0, f"exit {code}")
    if len(fake.requests) >= 3:
        res, _ = result_of(fake.requests[2], "toolu_write_file_0", last=True)   # the second round's, no.txt
        check(res is not None and "declined" in res, f"model told: {res!r}")
        res, _ = result_of(fake.requests[2], "toolu_bash_1")
        check(res is not None and res.endswith("[exit code: 0]"), f"the command ran after a: {res!r}")
    fake.stop()


def test_one_key_typeahead():
    """A line typed while the model works must not answer the question that follows."""
    d = tempfile.mkdtemp()
    def slow_tools(req):
        time.sleep(3.5)
        return tools(("write_file", {"path": "ahead.txt", "content": "x"}))
    fake = Fake([slow_tools, text("ok")])
    code, out, left = pty_session([BARB], d, env_for(fake), [
        ("> ", "go\n"),
        ("waiting for the model", "yyy"),   # typed ahead, before the question exists
        ("Allow?", "n"),
        ("ok", "\x04"),
    ], timeout=30)
    check(not left, f"typeahead session finished; left {[x[0] for x in left]}\n{out!r}")
    check(not os.path.exists(os.path.join(d, "ahead.txt")), "keys typed before the question did not answer it")
    check("waiting for the model" in out, "a slow reply shows the waiting line")
    fake.stop()


def test_paste():
    """Windows: lines that arrive together are one message, with the newlines they were pasted with;
    a line typed afterwards is a message of its own. (The POSIX prompt is the terminal's own line
    discipline, which cannot tell a paste from typing.)"""
    if not WIN: return
    d = tempfile.mkdtemp()
    fake = Fake([text("reply one"), text("reply two")])
    code, out, left = pty_session([BARB], d, env_for(fake), [
        ("> ", "first line\nsecond line\n\nfourth, after a blank\n"),
        ("reply one", "typed later\n"),
        ("reply two", "\x04"),
    ], timeout=30)
    check(not left, f"paste session finished; left {[x[0] for x in left]}\n{out!r}")
    check(len(fake.requests) == 2, f"{len(fake.requests)} requests: a paste is one message")
    if len(fake.requests) == 2:
        check(last_user_texts(fake.requests[0]) == ["first line\nsecond line\n\nfourth, after a blank"],
              f"the pasted message: {last_user_texts(fake.requests[0])}")
        check(last_user_texts(fake.requests[1]) == ["typed later"], f"the next one: {last_user_texts(fake.requests[1])}")
    no_problems(fake, "paste")
    fake.stop()


def test_instructions():
    d = tempfile.mkdtemp()
    with open(os.path.join(d, "AGENTS.md"), "w") as f: f.write("Always answer in French.\n@STYLE.md\n")
    with open(os.path.join(d, "STYLE.md"), "w") as f: f.write("Tabs, not spaces.\n")
    fake = Fake([text("d'accord"), text("ok")])
    p, _ = run(fake, ["-p", "hi"], d)
    check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    sysp = fake.requests[0].get("system", []) if fake.requests else []
    sysp = " ".join(b.get("text", "") for b in sysp) if isinstance(sysp, list) else str(sysp)
    check("Always answer in French." in sysp and "Tabs, not spaces." in sysp, "AGENTS.md and its import are in the system prompt")
    check("following the project's instructions in AGENTS.md and STYLE.md" in p.stderr, f"and the user is told: {p.stderr!r}")
    p, _ = run(fake, ["-p", "hi", "--no-instructions"], d)
    sysp = fake.requests[1].get("system", []) if len(fake.requests) > 1 else []
    sysp = " ".join(b.get("text", "") for b in sysp) if isinstance(sysp, list) else str(sysp)
    check(len(fake.requests) == 2 and "French" not in sysp, "--no-instructions leaves them out")
    no_problems(fake, "instructions")
    fake.stop()


def test_startup_errors():
    d = tempfile.mkdtemp()
    bare = {k: v for k, v in os.environ.items() if k.upper() in ("PATH", "SYSTEMROOT")}
    p = subprocess.run([BARB, "-p", "x"], cwd=d, env=bare, capture_output=True, **TEXT)
    check(p.returncode == 2 and "ANTHROPIC_API_KEY" in p.stderr, "missing key")
    p = subprocess.run([BARB, "--nonsense"], cwd=d, env=bare, capture_output=True, **TEXT)
    check(p.returncode == 2, "bad option")
    p = subprocess.run([BARB, "--version"], capture_output=True, **TEXT)
    check(p.returncode == 0 and p.stdout.startswith("barb "), "version")


if __name__ == "__main__":
    for t in (test_startup_errors, test_files_and_bash, test_key_not_inherited, test_background_wake, test_timeout_and_interrupt,
              test_permissions_and_errors, test_interactive_pipe, test_interactive_interrupt, test_interactive_permissions,
              test_one_key_typeahead, test_paste, test_instructions):
        t()
    print(f"e2e: {checks} checks, {failed} failed")
    sys.exit(1 if failed else 0)
