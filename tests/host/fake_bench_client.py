#!/usr/bin/env python3
# Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries.
#
# Subject to your compliance with these terms, you may use Microchip software
# and any derivatives exclusively with Microchip products. It is your
# responsibility to comply with third party license terms applicable to your
# use of third party software (including open source software) that may
# accompany Microchip software.
#
# THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
# EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
# WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
# PARTICULAR PURPOSE.
#
# IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
# INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
# WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
# BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
# FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
# ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
# THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.

r"""tests/host/fake_bench_client.py - a fixture standing in for
C:\work\Claas\Relay\bench_client.py, against the fixed tunnel contract
tools/remote.py's RemoteBench codes against (see that module's docstring)
plus bench_client's existing `flash` request. It never talks to a real
relay or a real bench_agent - tests/host/test_remote.py (RemoteBench's own
unit tests) and tools/board_run.py's/tools/adc_gui.py's own --selftest
("remote" scenarios) point RemoteBench at this script instead, exactly the
way they would point it at the real one with --bench-client.

    python fake_bench_client.py flash <hex> [--after N]
    python fake_bench_client.py tunnel --listen host:port

flash: parses the hex file's own name for board_run/'s naming convention
("<A|B>-<board>-<rev>.hex") and prints a boot banner line carrying that
revision ("... git <rev> (master)"), exactly the shape
tools/board_run.py's extract_banner_rev() looks for - so a --selftest run
really does check a "flashed" revision the way it would check a real
board's banner, not a canned string. What it "flashed" is written to
FAKE_BENCH_STATE_DIR (env, required) so a later, separate `tunnel`
invocation knows which firmware's console to play.

tunnel: reads that state and serves the console wire protocol through
tools/board_run.py's own ReplayTarget (imported, not reimplemented - one
command table whether ReplayTarget is driven in-process, by board_run.py's
own local --selftest, or over a real loopback socket here), plus this
file's own small amount of byte-level framing for "stream grab" (built from
ReplayTarget._grab_wire(), which hands back the same three raw wire pieces
grab() itself decodes, so there is exactly one place that builds a GRAB
frame's bytes).

Test control - all via environment variables, since a subprocess with this
fixed a CLI takes no other interactive input:

  FAKE_BENCH_STATE_DIR    required: a directory this script owns for the
                           test's duration (current.json, a hang_done
                           marker file).
  FAKE_BENCH_FAIL9=1      flash always exits 9 ("Programmer not found"),
                           state left untouched - the flash-failure
                           selftest case.
  FAKE_BENCH_WRONG_REV=1  flash "boots" with a revision that does NOT match
                           the hex file's own name - the banner-revision-
                           mismatch selftest case.
  FAKE_BENCH_SCENARIO=<name>
                           the reachability cases tools/remote.py's
                           RemoteBench.check()/check_board() tell apart,
                           each with the real bench_client's own exit code
                           and text (C:\work\Claas\Relay\bench_client.py,
                           connect()): "relay_down" (exit 3, "[client]
                           relay/agent not reachable: ..."), "agent_offline"
                           (exit 3, "[client] no agent at the relay within N
                           s"), "no_port" (info: uart "no MCP2221A port
                           found"; tunnel refused, exit 2), "port_busy"
                           (info: uart "<port>: <error>"), "tunnel_open"
                           (info: tunnel_open true), "board_silent" (info:
                           port open; the tunnel accepts but nothing ever
                           answers), "ok" (info: port open; the tunnel
                           plays board B). Unset: the flash/tunnel behaviour
                           below, unchanged.
  FAKE_BENCH_TIMEOUT_BLOCK=<cmd>
                           the tunnel hangs (sends nothing back, exactly
                           what a wedged board looks like on the wire) on
                           this one console command - ONCE for the whole
                           test, tracked by the state dir's hang_done
                           marker, so a re-flash (a fresh tunnel process)
                           behaves normally again - the mid-run-timeout-
                           recovered-by-re-flash selftest case.
"""
import argparse
import json
import os
import re
import socket
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import board_run  # noqa: E402
import protocol  # noqa: E402

_REV_RE = re.compile(r"^([AB])-.*-([0-9A-Fa-f]{6,40})\.hex$", re.I)


def _state_paths():
    d = os.environ.get("FAKE_BENCH_STATE_DIR")
    if not d:
        sys.exit("fake_bench_client: FAKE_BENCH_STATE_DIR must be set")
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, "current.json"), os.path.join(d, "hang_done")


def cmd_flash(args):
    if os.environ.get("FAKE_BENCH_FAIL9") == "1":
        print("ipecmd: Programmer not found", file=sys.stderr)
        return 9
    state_path, _ = _state_paths()
    name = os.path.basename(args.hex)
    m = _REV_RE.match(name)
    label = m.group(1) if m else "A"
    true_rev = m.group(2) if m else "0000000"
    reported_rev = "ffffff0" if os.environ.get("FAKE_BENCH_WRONG_REV") == "1" else true_rev
    with open(state_path, "w") as f:
        json.dump({"label": label, "has_route": label == "B", "rev": reported_rev}, f)
    print("[boot] uart up on FRC, 115200 8N1")
    print(f"[boot] adc_dma_40msps fake-selftest fake-time git {reported_rev} (master)")
    print("[boot] READY - nothing is converting, the console has the CPU")
    return 0


def cmd_reset(args):
    """bench_client reset (agent VERSION 6): the programmer reset - the
    firmware "flashed" last keeps running, its banner comes back.
    FAKE_BENCH_NO_RESET=1 plays an older agent that refuses the request."""
    if os.environ.get("FAKE_BENCH_NO_RESET") == "1":
        print("[agent] error: unknown request 'reset'", file=sys.stderr)
        return 2
    state_path, _ = _state_paths()
    rev = "0000000"
    try:
        with open(state_path) as f:
            rev = json.load(f).get("rev", rev)
    except OSError:
        pass
    print("[boot] uart up on FRC, 115200 8N1")
    print(f"[boot] adc_dma_40msps fake-selftest fake-time git {rev} (master)")
    print("[boot] READY - nothing is converting, the console has the CPU")
    return 0


def _read_until_cr(conn):
    """One console line, without its trailing '\\r' - None on disconnect,
    matching Target.cmd()/sync()'s own framing (tools/protocol.py): every
    line the client sends ends with a bare '\\r', never '\\r\\n'."""
    buf = b""
    while not buf.endswith(b"\r"):
        chunk = conn.recv(1)
        if not chunk:
            return None
        buf += chunk
    return buf[:-1].decode("ascii", "replace")


def _serve(conn, replay, hang_cmd, hang_marker):
    while True:
        line = _read_until_cr(conn)
        if line is None:
            return
        if line == "":
            conn.sendall(protocol.ACK)  # Target.sync()'s bare "\r"
            continue
        if line == "stream grab":
            header, payload, tail = replay._grab_wire()
            conn.sendall(b"stream grab\r\n" + header.encode("ascii") + payload + tail)
            continue
        if hang_cmd is not None and line == hang_cmd and not os.path.exists(hang_marker):
            open(hang_marker, "w").close()
            continue  # never reply - the real client's own timeout fires, like a wedged board
        ok, lines = replay.cmd(line)
        text = ("\r\n".join(lines) + "\r\n") if lines else ""
        conn.sendall(text.encode("ascii") + (protocol.ACK if ok else protocol.NAK))


def cmd_tunnel(args):
    sc = os.environ.get("FAKE_BENCH_SCENARIO")
    if sc in ("no_port", "port_busy", "tunnel_open"):
        print("[client] agent refused: the console port is not open", file=sys.stderr)
        return 2
    state_path, hang_marker = _state_paths()
    if sc in ("ok", "board_silent", "board_silent_v4") and not os.path.exists(state_path):
        with open(state_path, "w") as f:   # no flash first: play board B
            json.dump({"label": "B", "has_route": True}, f)
    if sc in ("board_silent", "board_silent_v4"):
        host, port_s = args.listen.rsplit(":", 1)
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, int(port_s)))
        srv.listen(1)
        print(f"tunnel ready {host}:{port_s}", flush=True)
        _serve_silent(srv)
        srv.close()
        return 0
    if not os.path.exists(state_path):
        print("fake_bench_client: tunnel with no prior flash - no state", file=sys.stderr)
        return 2
    with open(state_path) as f:
        state = json.load(f)
    hang_cmd = os.environ.get("FAKE_BENCH_TIMEOUT_BLOCK")
    if hang_cmd and os.path.exists(hang_marker):
        hang_cmd = None  # used once already - behave normally after the re-flash
    replay = board_run.ReplayTarget(state["label"], has_route=state["has_route"])

    host, port_s = args.listen.rsplit(":", 1)
    port = int(port_s)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    print(f"tunnel ready {host}:{port}", flush=True)
    try:
        while True:
            conn, _ = srv.accept()
            try:
                _serve(conn, replay, hang_cmd, hang_marker)
            finally:
                conn.close()
    except (KeyboardInterrupt, OSError):
        pass
    finally:
        srv.close()
    return 0


def _scenario_exit(args):
    """The two exit-3 failures, before any request runs - exactly where the
    real bench_client's connect() fails. None: carry on."""
    sc = os.environ.get("FAKE_BENCH_SCENARIO")
    if sc == "relay_down":          # bench_client since 28.09.2026: its own exit 5
        print("[client] relay not reachable: [WinError 10061] No connection could be made "
              "because the target machine actively refused it")
        return 5
    if sc == "relay_down_old":      # an older bench_client: exit 3, the shared text
        print("[client] relay/agent not reachable: [WinError 10061] No connection could be made "
              "because the target machine actively refused it")
        return 3
    if sc == "agent_offline":
        print(f"[client] no agent at the relay within {args.wait:g} s")
        return 3
    return None


def cmd_info(args):
    sc = os.environ.get("FAKE_BENCH_SCENARIO") or "ok"
    uart = {"no_port": "no MCP2221A port found",
            "port_busy": "COM5: could not open port 'COM5': PermissionError(13, 'Access is denied.')"
            }.get(sc, "open COM5 115200")
    print(json.dumps({"t": "info", "agent_version": 3, "host": "BENCH-PC", "uart": uart,
                      "ipecmd": None, "supports_tunnel": True,
                      "tunnel_open": sc == "tunnel_open",
                      # VERSION 5 agents probe the board themselves; the
                      # "_v4" variant plays an older agent without the field
                      **({} if sc.endswith("_v4") else
                         {"board": "silent" if sc.startswith("board_silent") else "answers",
                          "board_rtt_ms": None if sc.startswith("board_silent") else 12.5})},
                     indent=2))
    return 0


def _serve_silent(srv):
    """board_silent: accept, read and drop everything, never answer."""
    try:
        while True:
            conn, _ = srv.accept()
            try:
                while conn.recv(64):
                    pass
            finally:
                conn.close()
    except (KeyboardInterrupt, OSError):
        pass


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--wait", type=float, default=30)
    sub = ap.add_subparsers(dest="op", required=True)
    sub.add_parser("info")
    p_flash = sub.add_parser("flash")
    p_flash.add_argument("hex")
    p_flash.add_argument("--after", type=float, default=5)
    p_reset = sub.add_parser("reset")
    p_reset.add_argument("--after", type=float, default=5)
    p_tunnel = sub.add_parser("tunnel")
    p_tunnel.add_argument("--listen", required=True)
    args = ap.parse_args(argv)
    rc = _scenario_exit(args)
    if rc is not None:
        return rc
    if args.op == "info":
        return cmd_info(args)
    if args.op == "flash":
        return cmd_flash(args)
    if args.op == "reset":
        return cmd_reset(args)
    return cmd_tunnel(args)


if __name__ == "__main__":
    sys.exit(main())
