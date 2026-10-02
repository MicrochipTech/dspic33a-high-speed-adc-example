#!/usr/bin/env python3
r"""test_protocol.py - tools/protocol.py's CRC and GRAB frame parsing.

    python tests\host\test_protocol.py

P6.5 of docs/IMPLEMENTATION-PLAN.md moved crc16_ccitt_false(), the "stream
grab" GRAB frame parser (parse_grab_frame(), the header/CRC-line regexes) and
Target out of tools/adc_gui.py into tools/protocol.py, so a host-side tool
can talk to the board's console without pulling in NiceGUI. This is not new
semantics: it re-runs, against tools/protocol.py directly, the same checks
tools/adc_gui.py's own "python tools/adc_gui.py --selftest" already makes
for the CRC value, a clean GRAB frame, a CRC mismatch and a truncated frame -
see selftest() there for the fuller GUI-level exercise (FakeTarget, the
fault-injection grid check, the spectrum). Target.grab() is exercised here
too, against the same kind of never-answering stub serial object selftest()
uses, bypassing Target.__init__ so no pyserial import is needed - the goal
is to prove the bounded-wait timeout, not to open a real port.

Needs only the standard library; runs with any Python 3.8+.
"""
import os
import socket
import sys
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import protocol  # noqa: E402


def check(label, ok, detail=""):
    print(f"{label}:", "PASS" if ok else "FAIL", ("- " + detail) if detail else "")
    return ok


def build_grab_frame(samples, **meta):
    """The wire shape parse_grab_frame() decodes: a GRAB header line, the
    payload (12-bit samples packed little-endian in 16-bit words, as the
    firmware sends them), a CRC line over the payload, then the prompt and
    ACK -- exactly cmd_stream_grab()'s framing (cli.c)."""
    n = len(samples)
    header = (f"GRAB n={n} from={meta.get('from_', 0)} ksps={meta.get('ksps', 8000)} "
              f"ov={meta.get('ov', 0)} late={meta.get('late', 0)} missed={meta.get('missed', 0)} "
              f"halves={meta.get('halves', 2)} xfer={meta.get('xfer', 2 * n)} "
              f"slp={meta.get('slp', 8)} dachz={meta.get('dachz', 320000000)}"
              + (f" proc={meta['proc']}" if "proc" in meta else "")
              + (f" load={meta['load']}" if "load" in meta else "")
              + (f" gz={meta['gz'][0]} gzs={meta['gz'][1]} gzd={meta['gz'][2]}" if "gz" in meta else "")
              + "".join(f" {k}={v}" for k, v in meta.get("ext", {}).items())
              + "\r\n")
    payload = b"".join(int(v & 0x0FFF).to_bytes(2, "little") for v in samples)
    crc = protocol.crc16_ccitt_false(payload)
    tail = f"\r\nCRC {crc:04X}\r\n> ".encode("ascii") + protocol.ACK
    return header, payload, tail


def check_target_over_socket_url():
    """Target(url) must open a plain COM port and a "socket://host:port"
    URL the same way (tools/remote.py's RemoteBench hands back exactly such
    a URL for a bench_client tunnel) - Target.__init__ now opens the port
    through serial.serial_for_url() instead of serial.Serial() for that
    reason. This drives a real Target over a real loopback TCP socket
    played by a tiny thread standing in for the firmware at byte level:
    the sync ACK, then one "stream grab" cycle whose payload deliberately
    contains 0x06 (ACK) and 0x15 (NAK) sample values - Target.grab() reads
    the header first to learn the byte count and must not stop on a sample
    byte that happens to match ACK/NAK (see Target.grab()'s own docstring;
    parse_grab_frame() already has a unit test for the decode, this one is
    for the transport)."""
    samples = [0x0006, 0x0015, 100, 4095, 0]
    header, payload, tail = build_grab_frame(samples, from_=0, slp=9)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    srv.listen(1)

    def _read_until_cr(conn):
        buf = b""
        while not buf.endswith(b"\r"):
            chunk = conn.recv(1)
            if not chunk:
                return buf
            buf += chunk
        return buf

    def serve_one():
        conn, _ = srv.accept()
        try:
            _read_until_cr(conn)                 # Target.sync()'s bare "\r"
            conn.sendall(protocol.ACK)
            line = _read_until_cr(conn)           # "stream grab\r"
            if line != b"stream grab\r":
                return
            conn.sendall(b"stream grab\r\n" + header.encode("ascii") + payload + tail)
            time.sleep(0.2)                       # give the client time to read before we close
        finally:
            conn.close()

    th = threading.Thread(target=serve_one, daemon=True)
    th.start()
    try:
        t = protocol.Target(f"socket://127.0.0.1:{port}")
        try:
            ok, decoded, meta = t.grab(timeout=2.0)
        finally:
            t.close()
    finally:
        srv.close()
        th.join(timeout=2)
    return check("Target over socket:// parses a GRAB frame with 0x06/0x15 sample bytes",
                 ok and list(decoded) == samples, f"ok={ok} samples={list(decoded)} meta={meta}")


def check_ping_over_socket_url():
    """Target.ping() over a real loopback socket against a stand-in that
    answers every bare "\\r" with "\\r\\n> " + ACK after DELAY_S: the
    measured round trip must show that delay - and NOT the port's 50 ms
    read timeout on top of it, which is what ping() reads byte-wise to
    avoid (a ser.read(4096) would wait it out on every sample)."""
    DELAY_S, N = 0.030, 5
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    srv.listen(1)

    def serve():
        conn, _ = srv.accept()
        try:
            conn.recv(1)                              # Target.__init__'s sync()
            conn.sendall(protocol.ACK)
            for _ in range(N):
                if not conn.recv(1):
                    return
                time.sleep(DELAY_S)
                conn.sendall(b"\r\n> " + protocol.ACK)
            time.sleep(0.2)
        finally:
            conn.close()

    th = threading.Thread(target=serve, daemon=True)
    th.start()
    try:
        t = protocol.Target(f"socket://127.0.0.1:{port}")
        try:
            r = t.ping(n=N, timeout=2.0)
        finally:
            t.close()
    finally:
        srv.close()
        th.join(timeout=2)
    ms = DELAY_S * 1000.0
    ok = r["n"] == N and len(r["samples_ms"]) == N and ms * 0.9 <= r["min_ms"] and r["max_ms"] < ms + 40.0
    return check(f"Target.ping() measures a {ms:.0f} ms stand-in delay without the 50 ms read timeout",
                 ok, protocol.format_rtt(r))


def main() -> int:
    ok_all = True

    # The firmware's own CRC check value (docs/PLAN-BINARY-TRANSFER.md),
    # asserted at import time in protocol.py too.
    ok_all &= check("CRC-16/CCITT-FALSE check value",
                     protocol.crc16_ccitt_false(b"123456789") == 0x29B1)

    # A clean GRAB frame parses: right sample count, right CRC, ACK seen.
    samples = [100, 200, 4095, 0, 2048]
    header, payload, tail = build_grab_frame(samples, from_=1024, slp=18)
    ok, decoded, meta = protocol.parse_grab_frame(header, payload, tail)
    ok_all &= check("GRAB frame decodes",
                     ok and list(decoded) == samples and meta["from_"] == 1024 and meta["slpdat"] == 18,
                     f"ok={ok} samples={list(decoded)} meta={meta}")

    # proc= (01.10.2026): the signal processing flag - read when present,
    # 0 when absent (a firmware from before it).
    _, _, meta_old = protocol.parse_grab_frame(*build_grab_frame(samples))
    _, _, meta_p0 = protocol.parse_grab_frame(*build_grab_frame(samples, proc=0))
    okp, decp, meta_p1 = protocol.parse_grab_frame(*build_grab_frame(samples, proc=1))
    ok_all &= check("GRAB frame proc= read, 0 when absent",
                     meta_old["proc"] == 0 and meta_p0["proc"] == 0 and meta_p1["proc"] == 1
                     and okp and list(decp) == samples,
                     f"absent={meta_old['proc']} proc0={meta_p0['proc']} proc1={meta_p1['proc']}")

    # load= (02.10.2026): per mille, None when the firmware does not send it
    _, _, meta_l = protocol.parse_grab_frame(*build_grab_frame(samples, proc=1, load=634))
    ok_all &= check("GRAB frame load= read, None when absent",
                     meta_l["load_pm"] == 634 and meta_l["proc"] == 1 and meta_old["load_pm"] is None,
                     f"load={meta_l['load_pm']} absent={meta_old['load_pm']}")

    # proc= names the filter and gz=/gzs=/gzd= the Goertzel (02.10.2026)
    _, _, meta_g = protocol.parse_grab_frame(*build_grab_frame(samples, proc=3, load=600, gz=(812, 997, 1)))
    ok_all &= check("GRAB frame proc=3 and the Goertzel fields read, gz None when absent",
                     meta_g["proc"] == 3 and protocol.PROC_NAMES[3] == "band-pass"
                     and meta_g["gz"] == dict(amp=812, share_pm=997, detected=1)
                     and meta_l["gz"] is None and meta_g["load_pm"] == 600,
                     f"proc={meta_g['proc']} gz={meta_g['gz']} absent={meta_l['gz']}")

    # any further key=value fields are the application's (gui_link_app_fields()),
    # after the Goertzel's: meta['ext'], empty when there are none
    _, _, meta_c = protocol.parse_grab_frame(*build_grab_frame(samples, proc=0, load=40,
                                                                gz=(5, 0, 0), ext=dict(abc=1234, x2=7)))
    ok_all &= check("GRAB frame the application's fields read, ext empty when absent",
                     meta_c["ext"] == dict(abc=1234, x2=7) and meta_c["gz"]["amp"] == 5
                     and meta_g["ext"] == {},
                     f"ext={meta_c['ext']} absent={meta_g['ext']}")

    # n=0 is the NAK shape ("stream on" not running / halt-restart failed) -
    # the same frame FakeTarget.grab() and the firmware send for that case.
    header0, payload0, tail0 = build_grab_frame([])
    tail0_nak = tail0[:-len(protocol.ACK)] + protocol.NAK
    ok0, samples0, meta0 = protocol.parse_grab_frame(header0, payload0, tail0_nak)
    ok_all &= check("GRAB frame with n=0 (NAK) refused",
                     (not ok0) and len(samples0) == 0 and "error" in meta0, meta0.get("error"))

    # A CRC mismatch (one flipped payload byte) must be caught, not silently
    # accepted.
    header, payload, tail = build_grab_frame(samples)
    bad_payload = bytes([payload[0] ^ 0xFF]) + payload[1:]
    ok, _, meta = protocol.parse_grab_frame(header, bad_payload, tail)
    ok_all &= check("GRAB frame CRC mismatch caught",
                     (not ok) and "CRC mismatch" in meta.get("error", ""), meta.get("error"))

    # A truncated frame (fewer payload bytes than the header promises) is
    # what a Ctrl+C or a disconnect mid-transfer looks like on the wire.
    header, payload, tail = build_grab_frame(samples)
    short_payload = payload[:4]  # 4 of the 10 bytes promised
    ok, _, meta = protocol.parse_grab_frame(header, short_payload, tail)
    ok_all &= check("GRAB frame truncated payload caught",
                     (not ok) and "short block" in meta.get("error", ""), meta.get("error"))

    # A header that is not a GRAB frame at all is a hard error (a bug on
    # either side of the link), not a quiet False.
    try:
        protocol.parse_grab_frame("not a grab header\r\n", b"", tail)
        ok_all &= check("bad header raises", False)
    except RuntimeError:
        ok_all &= check("bad header raises", True)

    # Target.grab() itself, against a serial stub that never delivers a
    # byte -- the same bounded-wait code path a disconnected or wedged
    # board would hit. __init__ is bypassed (Target.__new__) so this needs
    # no pyserial import and opens no real port, exactly like adc_gui.py's
    # own --selftest does for the same check.
    class _NullSerial:
        def read(self, n=1):
            return b""

        def write(self, data):
            return len(data)

        def reset_input_buffer(self):
            pass

    t = protocol.Target.__new__(protocol.Target)
    t.ser = _NullSerial()
    t.on_log = None
    t.port = "null (test_protocol)"
    try:
        t.grab(timeout=0.05)
        ok_timeout = False
    except TimeoutError:
        ok_timeout = True
    ok_all &= check("Target.grab() timeout with no bytes caught", ok_timeout)

    ok_all &= check_target_over_socket_url()
    ok_all &= check_ping_over_socket_url()

    print("test_protocol", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
