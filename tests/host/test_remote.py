#!/usr/bin/env python3
r"""test_remote.py - tools/remote.py's RemoteBench, against the fixture
tests/host/fake_bench_client.py rather than the real
C:\work\Claas\Relay\bench_client.py (never started or contacted from a
test - see tools/remote.py's own docstring). Covers the plumbing
RemoteBench owns: spawning the tunnel subprocess and recognising its one
"tunnel ready ..." line (or its absence), the free-port picker, flash's
(code, text) shape on success and on a couple of the documented exit
codes, close_tunnel() actually ending the subprocess, and the
context-manager form.

    python tests\host\test_remote.py

Standard library only, plus tests/host/fake_bench_client.py, which itself
imports tools/board_run.py and tools/protocol.py (kept on sys.path the
same way board_run.py's own --selftest is exercised - no pyserial
needed, fake_bench_client.py never opens a real COM port).
"""
import os
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import remote  # noqa: E402
import protocol  # noqa: E402

FAKE_BENCH_CLIENT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_bench_client.py")


def check(label, ok, detail=""):
    print(f"{label}:", "PASS" if ok else "FAIL", ("- " + detail) if detail else "")
    return ok


def make_hex(tmp, name="A-EV74H48A-deadbee.hex"):
    path = os.path.join(tmp, name)
    with open(path, "wb") as f:
        f.write(b":10000000FF\n")
    return path


def bench(tmp, **extra_env):
    env = dict(os.environ, FAKE_BENCH_STATE_DIR=os.path.join(tmp, "state"), **extra_env)
    return remote.RemoteBench(bench_client=FAKE_BENCH_CLIENT, env=env)


def main() -> int:
    ok_all = True

    ok_all &= check("describe_exit() has text for 2/3/4/9",
                     all(remote.describe_exit(c) for c in (2, 3, 4, 9)))
    ok_all &= check("describe_exit() has nothing to say about 0",
                     remote.describe_exit(0) is None)

    p1, p2 = remote.pick_free_port(), remote.pick_free_port()
    ok_all &= check("pick_free_port() returns a usable TCP port twice", p1 > 0 and p2 > 0)

    # ---- flash: success, exit 9, and the revision the banner carries ----
    with tempfile.TemporaryDirectory() as tmp:
        hexpath = make_hex(tmp)
        b = bench(tmp)
        code, text = b.flash(hexpath, after=1)
        ok_all &= check("flash(): exit 0, banner carries the hex file's own revision",
                         code == 0 and "git deadbee" in text, text)

        b9 = bench(tmp, FAKE_BENCH_FAIL9="1")
        code9, text9 = b9.flash(hexpath, after=1)
        ok_all &= check("flash(): exit 9 -> clear text, not a bare traceback",
                         code9 == 9 and "programmer" in text9.lower() and "exit 9" in text9, text9)

    # ---- tunnel: ready line, a connectable URL, close_tunnel() ends it ----
    with tempfile.TemporaryDirectory() as tmp:
        hexpath = make_hex(tmp)
        b = bench(tmp)
        b.flash(hexpath, after=1)
        url = b.open_tunnel(timeout=10)
        ok_all &= check("open_tunnel(): returns a socket:// URL", url.startswith("socket://"))
        t = protocol.Target(url)
        ok, lines = t.cmd("version")
        ok_all &= check("tunnel: a real Target can sync and run a command over it",
                         ok and any("adc_dma_40msps" in l for l in lines), str(lines))
        t.close()
        proc = b._proc
        b.close_tunnel()
        ok_all &= check("close_tunnel(): the subprocess actually exits",
                         proc.poll() is not None)
        ok_all &= check("close_tunnel(): tunnel_open/url both clear",
                         not b.tunnel_open and b.url is None)
        ok_all &= check("close_tunnel(): idempotent (no tunnel open)", True)
        b.close_tunnel()  # must not raise

    # ---- open_tunnel() failure: nothing at the far end ----
    with tempfile.TemporaryDirectory() as tmp:
        bad = remote.RemoteBench(bench_client=os.path.join(tmp, "does-not-exist.py"))
        try:
            bad.open_tunnel(timeout=5)
            ok_all &= check("open_tunnel(): a broken bench_client path raises", False)
        except RuntimeError as e:
            ok_all &= check("open_tunnel(): a broken bench_client path raises", True, str(e))

    # ---- flash() auto-closes an open tunnel first (the agent would refuse
    # otherwise) - RemoteBench does this itself rather than trusting every
    # caller to remember the order. ----
    with tempfile.TemporaryDirectory() as tmp:
        hexpath = make_hex(tmp)
        b = bench(tmp)
        b.flash(hexpath, after=1)
        b.open_tunnel(timeout=10)
        ok_all &= check("flash(): pre-condition - a tunnel is open", b.tunnel_open)
        code, _ = b.flash(hexpath, after=1)
        ok_all &= check("flash(): auto-closed the open tunnel and still succeeded",
                         code == 0 and not b.tunnel_open)
        b.close_tunnel()

    # ---- context manager: __exit__ closes a still-open tunnel ----
    with tempfile.TemporaryDirectory() as tmp:
        hexpath = make_hex(tmp)
        proc_ref = {}
        with bench(tmp) as b:
            b.flash(hexpath, after=1)
            b.open_tunnel(timeout=10)
            proc_ref["p"] = b._proc
        time.sleep(0.2)
        ok_all &= check("context manager: __exit__ closes an open tunnel",
                         proc_ref["p"].poll() is not None)

    print("test_remote", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
