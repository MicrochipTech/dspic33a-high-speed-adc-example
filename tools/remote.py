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
import json
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
    3: "no bench agent at the relay (or its TLS handshake failed)",
    5: "the relay server is not reachable",
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
        detail = (r.stderr or r.stdout or "").strip()
        if code == 9:
            text = f"{describe_exit(code)} (exit {code})" + (f": {detail}" if detail else "")
        else:
            # classify() tells exit 3's two cases apart (relay vs agent)
            text = f"{classify(code, (r.stdout or '') + (r.stderr or ''))[1]} (exit {code})"
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
        # --wait below our own timeout, so that a missing relay or agent is
        # reported by bench_client itself (exit 3 with its text, which
        # classify() tells apart) instead of our timeout cutting it short
        wait = max(1.0, timeout - 3.0)
        cmd = [self.python, self.bench_client, "--wait", f"{wait:g}", "tunnel",
               "--listen", f"127.0.0.1:{port}"]
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
            # bench_client printed its reason instead of the ready line (or
            # nothing): let it finish, so its exit code is known - poll()
            # right away often still reads None
            try:
                code = proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.terminate()
                code = proc.poll()
            rest = ""
            try:
                rest = (proc.stdout.read() or "") + (proc.stderr.read() or "")
            except Exception:
                pass
            text = ((line or "") + rest).strip()
            if code not in (None, 0):
                reason = classify(code, text)[1]
            else:
                reason = "exited with no output" + (f": {text}" if text else "")
            raise RuntimeError(f"tunnel failed to start: {reason}")
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

    # -- diagnosis: which link is missing --------------------------------
    def info(self, wait=10.0):
        """python bench_client.py --wait <wait> info. Returns (code, data,
        text): data is the agent's JSON reply on exit 0 (None otherwise),
        text everything the process printed - classify() needs it, because
        exit 3 alone does not say whether the relay or the agent is
        missing."""
        cmd = [self.python, self.bench_client, "--wait", f"{wait:g}", "info"]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, env=self.env,
                               timeout=wait + 20.0)
        except subprocess.TimeoutExpired:
            return 4, None, f"bench_client info did not finish within {wait + 20.0:g} s"
        except OSError as e:
            return -1, None, f"cannot start {self.bench_client}: {e}"
        text = ((r.stdout or "") + (r.stderr or "")).strip()
        data = None
        if r.returncode == 0:
            try:
                data = json.loads(r.stdout[r.stdout.index("{"):])
            except ValueError:
                data = None
        return r.returncode, data, text

    def check(self, wait=10.0):
        """The first three links, from one `info` request: relay reachable,
        bench agent online there, the agent's console port open. Returns a
        list of step dicts {name, ok, detail} (ok None = not reached, since
        an earlier step failed). The fourth link - does a board answer on
        that port - needs a tunnel: check_board()."""
        code, data, text = self.info(wait=wait)
        steps = [dict(name="relay", ok=None, detail=""),
                 dict(name="bench agent", ok=None, detail=""),
                 dict(name="console port", ok=None, detail="")]
        if code == 0 and data is not None:
            steps[0].update(ok=True, detail="reachable")
            steps[1].update(ok=True, detail=f"online - {data.get('host', '?')}, "
                                             f"agent {data.get('agent_version', '?')}")
            uart = str(data.get("uart", ""))
            if data.get("tunnel_open"):
                steps[2].update(ok=False, detail=f"{uart} - but a tunnel is already open: "
                                                 "another client holds the console")
            elif uart.startswith("open "):
                steps[2].update(ok=True, detail=uart)
            else:
                steps[2].update(ok=False, detail=f"{uart or 'no status'} - on the agent's PC: "
                                                 "board USB cable plugged in? COM port used by "
                                                 "another program (terminal, MPLAB X)?")
            # An agent from VERSION 5 on probes the board itself ("board":
            # answers/silent/garbled/unknown) - a board that does not answer
            # is then known without opening a tunnel. "answers" and "unknown"
            # leave the fourth step to check_board(), which also measures
            # the round trip through the whole chain.
            board = data.get("board")
            if steps[2]["ok"] and board in ("silent", "garbled"):
                why = ("no answer to an empty line" if board == "silent" else
                       "bytes come back, but no prompt/ACK - wrong baud rate or other firmware?")
                steps.append(dict(name="board", ok=False,
                                  detail=f"the console port is open, but the board does not answer "
                                         f"({why}; the agent's own probe) - board powered? firmware "
                                         "running (not held in a debug session)?"))
            return steps
        kind, sentence = classify(code, text)
        if kind == "relay":
            steps[0].update(ok=False, detail=sentence)
        elif kind == "agent":
            steps[0].update(ok=True, detail="reachable")
            steps[1].update(ok=False, detail=sentence)
        else:
            steps[0].update(ok=True, detail="reachable")
            steps[1].update(ok=False, detail=sentence)
        return steps

    def check_board(self, sync_timeout=8.0, on_log=None):
        """The fourth link: open the tunnel, wait up to sync_timeout s for
        the board's prompt, measure the round trip. Returns (step, target):
        target is the open protocol.Target on success (the caller keeps
        it), None on failure (the tunnel is closed again)."""
        import protocol  # tools/ - the caller's own sys.path already has it
        try:
            url = self.open_tunnel()
        except (RuntimeError, TimeoutError) as e:
            return dict(name="board", ok=False, detail=f"tunnel did not open: {e}"), None
        try:
            t = protocol.Target(url, on_log=on_log, sync_timeout=sync_timeout)
        except TimeoutError:
            self.close_tunnel()
            return dict(name="board", ok=False,
                        detail=f"the console port is open, but no board answers within "
                               f"{sync_timeout:g} s - board powered? firmware running "
                               "(not held in a debug session)? right COM port?"), None
        except Exception as e:  # noqa: BLE001 - the tunnel died under us
            self.close_tunnel()
            return dict(name="board", ok=False, detail=f"tunnel failed: {e}"), None
        try:
            rtt = t.ping(n=3)
            detail = "answers - " + protocol.format_rtt(rtt)
        except TimeoutError:
            detail = "answered the sync, but not the round-trip probe"
        return dict(name="board", ok=True, detail=detail), t


def classify(code, text):
    """(kind, sentence) for a failed bench_client call. Since 28.09.2026
    bench_client exits 5 when the relay itself is not reachable and keeps 3
    for "no agent at the relay" ("[client] no agent at the relay within N
    s") and for a failed TLS handshake with an agent that is there
    ("[client] agent found at the relay, but the TLS handshake with it
    failed"). An older bench_client exits 3 for the relay case too, with
    "[client] relay/agent not reachable: <error>" - that text is kept as the
    fallback, so both versions are told apart correctly."""
    t = text or ""
    detail = t.splitlines()[-1].strip() if t.strip() else ""
    if "can't open file" in t or "No such file or directory" in t:
        # python itself exits 2 for a missing script - not the agent's exit 2
        return "client", f"bench_client.py not found - check the path ({detail})"
    if code == 5:
        return "relay", ("the relay server is not reachable - network, VPN or proxy, or the "
                         "relay is down" + (f" ({detail})" if detail else ""))
    if code == 3 and "no agent at the relay" in t:
        return "agent", ("the relay answers, but no bench agent is online there - "
                         "bench_agent not started on the board's PC, or started with another token")
    if code == 3 and "TLS handshake" in t:
        return "agent", ("the bench agent is at the relay, but the TLS handshake with it failed - "
                         "certificates of agent and client from different packages?"
                         + (f" ({detail})" if detail else ""))
    if code == 3:
        return "relay", ("the relay server is not reachable - network, VPN or proxy, or the "
                         "relay is down" + (f" ({detail})" if detail else ""))
    if code == 2:
        return "refused", "the bench agent refused the request" + (f": {detail}" if detail else "")
    if code == 4:
        return "lost", "the connection to the bench agent was lost" + (f": {detail}" if detail else "")
    if code == -1:
        return "client", detail or "bench_client could not be started"
    return "other", f"bench_client exit {code}" + (f": {detail}" if detail else "")


def format_steps(steps):
    """One line per step: "  [ok] relay: reachable" / "  [--] ..." /
    "  [  ] ... (not reached)"."""
    out = []
    for s in steps:
        mark = "ok" if s["ok"] else ("--" if s["ok"] is False else "  ")
        out.append(f"  [{mark}] {s['name']}: {s['detail'] or 'not checked - an earlier step failed'}")
    return "\n".join(out)


def main(argv=None):
    """python tools/remote.py check [--bench-client PATH] [--wait S]:
    which of relay, bench agent, console port and board is reachable."""
    import argparse
    import sys
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=main.__doc__)
    ap.add_argument("op", choices=["check"])
    ap.add_argument("--bench-client", default=None)
    ap.add_argument("--wait", type=float, default=10.0)
    a = ap.parse_args(argv)
    bench = RemoteBench(bench_client=a.bench_client)
    steps = bench.check(wait=a.wait)
    if len(steps) == 3 and all(s["ok"] for s in steps):
        step, t = bench.check_board()
        steps.append(step)
        if t is not None:
            t.close()
        bench.close_tunnel()
    elif len(steps) == 3:
        steps.append(dict(name="board", ok=None, detail=""))
    print(format_steps(steps))
    return 0 if all(s["ok"] for s in steps) else 1


if __name__ == "__main__":
    raise SystemExit(main())
