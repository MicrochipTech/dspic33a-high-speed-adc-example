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
import sys

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
              f"slp={meta.get('slp', 8)} dachz={meta.get('dachz', 320000000)}\r\n")
    payload = b"".join(int(v & 0x0FFF).to_bytes(2, "little") for v in samples)
    crc = protocol.crc16_ccitt_false(payload)
    tail = f"\r\nCRC {crc:04X}\r\n> ".encode("ascii") + protocol.ACK
    return header, payload, tail


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

    print("test_protocol", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
