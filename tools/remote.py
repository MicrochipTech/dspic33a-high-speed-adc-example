#!/usr/bin/env python3
"""tools/remote.py - drives C:\\work\\Claas\\Relay\\bench_client.py's two
non-console requests (`flash`, `tunnel`) as short-lived subprocesses, so
board_run.py and adc_gui.py can reach the colleague's board without ever
importing anything from the Relay folder (a different agent develops that
folder in parallel; this module only ever shells out to the fixed CLI
contract it exposes).

The tunnel contract this module codes against (given, not implemented
here):

    python bench_client.py tunnel --listen 127.0.0.1:<port>
        runs in the foreground until killed; forwards the console's raw
        serial bytes (115200 8N1) both ways over the relay/TLS link; prints
        exactly one line "tunnel ready 127.0.0.1:<port>" to stdout once it
        is accepting; exit codes 3 (agent offline), 2 (refused), 4
        (connection lost). While a tunnel is open, `flash` is refused
        (exit 2) - close the tunnel first.

    python bench_client.py flash <hex> --after 5
        programs the board and prints the boot banner read for 5 s;
        exit 0 ok, 9 = programmer not found, 3 = agent offline.

RemoteBench wraps both as (code, text) calls and a tunnel URL
("socket://127.0.0.1:<port>") that tools/protocol.py's Target opens
exactly like a COM port, via serial.serial_for_url() (see protocol.py).

Never contacts the real relay or a real bench_client.py in a test: the
unit tests (tests/host/test_remote.py) and the callers' own --selftest
point RemoteBench at a small fixture script instead
(tests/host/fake_bench_client.py).
"""
import os
import socket
import subprocess
import sys
import threading
import queue

DEFAULT_BENCH_CLIENT = r"C:\work\Claas\Relay\bench_client.py"

# bench_client's own exit codes (README.md "The agent handles exactly five
# requests" / its docstring): 0 ok, `flash` otherwise returns ipecmd's own
# code (9 = "Programmer not found"), 2 = the agent refused or failed, 3 = no
# agent at the relay, 4 = connection lost. The tunnel contract above reuses
# 2/3/4 unchanged and needs no code of its own.
_EXIT_TEXTS = {
    2: "the agent refused or failed",
    3: "the bench agent is not reachable (offline at the relay)",
    4: "the connection to the agent was lost",
    9: "the programmer was not found (ipecmd exit 9 - \"Programmer not found\")",
}


def describe_exit(code):
    """A clear sentence for a bench_client exit code, or None for one this
    module has no special text for (0, or something neither flash nor
    tunnel documents)."""
    return _EXIT_TEXTS.get(code)


def pick_free_port():
    """One free TCP port on localhost, for a tunnel's --listen when the
    caller has no fixed one. Races with anything else that binds the same
    port between this call and the tunnel subprocess's own bind - the
    window is short and this is only ever used for a local loopback
    listener, never a shared or public one."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class RemoteBench:
    """One remote bench, reached through `bench_client`'s `flash` and
    `tunnel` requests. Context-manager friendly: `with RemoteBench(...) as
    bench:` closes any open tunnel on the way out, whatever raised.

    `bench_client` defaults to BENCH_CLIENT's environment value, else
    tools/remote.DEFAULT_BENCH_CLIENT (Relay's own default path); `python`
    defaults to sys.executable, per the task's instruction to run
    bench_client.py with the same interpreter this process runs under.
    `port` fixes the tunnel's local --listen port; left None, a free one is
    picked by pick_free_port() on every open_tunnel() call."""

    def __init__(self, bench_client=None, port=None, python=None, env=None):
        self.bench_client = bench_client or os.environ.get("BENCH_CLIENT", DEFAULT_BENCH_CLIENT)
        self.python = python or sys.executable
        self.port = port
        self.env = env  # None = inherit this process's environment (subprocess default)
        self._proc = None
        self._url = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close_tunnel()
        return False

    # -- flash ---------------------------------------------------------
    def flash(self, hex_path, after=5):
        """python bench_client.py flash <hex_path> --after <after>.
        Returns (code, text): on success (code 0) text is the boot banner
        bench_client read for `after` seconds; on failure text is a clear
        sentence for the exit code (describe_exit()) with whatever the
        process printed appended, never a bare traceback or an empty
        string. Auto-closes an open tunnel first (the agent refuses `flash`
        while one is open, so there is nothing to gain by making every
        caller remember the order) - a no-op if none is open."""
        self.close_tunnel()
        cmd = [self.python, self.bench_client, "flash", hex_path, "--after", str(after)]
        r = subprocess.run(cmd, capture_output=True, text=True, env=self.env)
        code = r.returncode
        if code == 0:
            return code, r.stdout
        reason = describe_exit(code) or f"unexpected exit {code}"
        detail = (r.stderr or r.stdout or "").strip()
        text = f"{reason} (exit {code})" + (f": {detail}" if detail else "")
        return code, text

    # -- tunnel ----------------------------------------------------------
    def open_tunnel(self, timeout=15.0):
        """Starts `bench_client.py tunnel --listen 127.0.0.1:<port>` and
        waits up to `timeout` s for its one "tunnel ready ..." line.
        Returns the URL tools/protocol.py's Target can open
        ("socket://127.0.0.1:<port>"). Raises RuntimeError if the process
        exits or prints anything else first, TimeoutError if it never
        prints the ready line at all."""
        if self._proc is not None:
            raise RuntimeError("a tunnel is already open - call close_tunnel() first")
        port = self.port or pick_free_port()
        cmd = [self.python, self.bench_client, "tunnel", "--listen", f"127.0.0.1:{port}"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 text=True, bufsize=1, env=self.env)
        q = queue.Queue()

        def _read_first_line():
            try:
                line = proc.stdout.readline()
            except Exception:
                line = ""
            q.put(line)

        threading.Thread(target=_read_first_line, daemon=True).start()
        try:
            line = q.get(timeout=timeout)
        except queue.Empty:
            proc.terminate()
            raise TimeoutError(f"tunnel: no \"tunnel ready\" line within {timeout} s")
        if not line or "tunnel ready" not in line:
            code = proc.poll()
            stderr = ""
            try:
                stderr = (proc.stderr.read() or "").strip()
            except Exception:
                pass
            proc.terminate()
            reason = describe_exit(code) if code is not None else "exited with no output"
            reason = reason or f"exit {code}"
            raise RuntimeError(f"tunnel failed to start: {reason}" + (f": {stderr}" if stderr else ""))
        self._proc = proc
        self._url = f"socket://127.0.0.1:{port}"
        return self._url

    def close_tunnel(self):
        """Terminates the tunnel subprocess, if one is open. Idempotent -
        safe to call when no tunnel is open (perform_full_run_remote()
        calls this unconditionally before every flash, per the contract:
        "close any tunnel"."""
        if self._proc is None:
            return
        proc, self._proc, self._url = self._proc, None, None
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)

    @property
    def tunnel_open(self):
        return self._proc is not None

    @property
    def url(self):
        return self._url
