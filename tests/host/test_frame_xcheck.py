#!/usr/bin/env python3
r"""test_frame_xcheck.py - frame.c's output, parsed on the Python side.

    python tests\host\test_frame_xcheck.py [path\to\test_frame.exe]

P6.3 of docs/IMPLEMENTATION-PLAN.md moved the binary frame "blk" and
"stream grab" both send (header line, payload in chunks, CRC line) out of
cli.c into src/lib/frame.c, hardware-free and host-testable. This script
is the cross-check promised there: it asks the host-built test_frame.exe
(built by tools\hosttest.bat, which also runs this script afterwards) to
dump one clean "blk"-shaped frame and one "stream grab"-shaped frame with
realistic header fields, then parses both from the Python side and checks
that the CRC agrees and the payload round-trips exactly.

The GRAB frame is parsed with tools/protocol.py's own parse_grab_frame() -
the same function the GUI uses - so this also proves frame.c's output is
what the GUI expects, not just what this test expects.

The "blk" (BIN n=...) frame has no parser of its own any more:
tools/protocol.py dropped it when the GUI stopped sending "blk" (P6.5's
docstring). A small regex is enough for a cross-check that only needs to
read the count and CRC back, so it lives here rather than adding a parser
back to protocol.py for a shape nothing else needs.

Needs only the standard library; runs with any Python 3.8+.
"""
import os
import re
import subprocess
import sys

import numpy as np

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import protocol  # noqa: E402

_BIN_HEADER_RE = re.compile(
    rb"BIN n=(\d+) p1=(\d+) p2=(\d+) samc=(\d+) in=(\d+)\r\n")
_CRC_LINE_RE = re.compile(rb"CRC ([0-9A-Fa-f]{4})\r\n$")

# The same sample generator test_frame.c's --dump-blk/--dump-grab use
# (i * 37 + 5) % 4096, N = 100 - kept here only for the human reading this
# file; the check itself re-derives the samples from the parsed payload,
# it does not need to know how they were generated.
N_EXPECTED = 100


def check(label, ok, detail=""):
    print(f"{label}:", "PASS" if ok else "FAIL", ("- " + detail) if detail else "")
    return ok


def parse_bin_frame(data: bytes):
    """The "blk" shape: BIN header, 2*n payload bytes, CRC line - the one
    parser protocol.py no longer carries (see module docstring). Returns
    (ok, samples, meta), same contract as parse_grab_frame()."""
    m = _BIN_HEADER_RE.match(data)
    if not m:
        raise RuntimeError(f"blk: no BIN header, got {data[:64]!r}")
    n = int(m.group(1))
    meta = dict(p1=int(m.group(2)), p2=int(m.group(3)), samc=int(m.group(4)),
                pinsel=int(m.group(5)))
    rest = data[m.end():]
    payload = rest[:2 * n]
    tail = rest[2 * n:]
    m2 = _CRC_LINE_RE.search(tail)
    if not m2:
        raise RuntimeError(f"blk: no CRC line, got {tail!r}")
    crc_frame = int(m2.group(1), 16)
    crc_calc = protocol.crc16_ccitt_false(payload)
    samples = np.frombuffer(payload, dtype="<u2").astype(int) & 0x0FFF
    ok = (len(payload) == 2 * n) and (crc_frame == crc_calc)
    if len(payload) != 2 * n:
        meta["error"] = f"short block: got {len(payload)} of {2 * n} bytes"
    elif crc_frame != crc_calc:
        meta["error"] = f"CRC mismatch: frame {crc_frame:04X}, computed {crc_calc:04X}"
    return ok, samples, meta


def expected_samples(n: int):
    return [(i * 37 + 5) % 4096 for i in range(n)]


def main() -> int:
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'host', 'test_frame.exe')
    if not os.path.exists(exe):
        sys.exit(f'no {exe} - run tools\\hosttest.bat first (it builds it, then runs this script)')

    outdir = os.path.join(ROOT, 'build', 'host')
    os.makedirs(outdir, exist_ok=True)
    blk_path = os.path.join(outdir, 'frame_blk.bin')
    grab_path = os.path.join(outdir, 'frame_grab.bin')

    ok_all = True

    p = subprocess.run([exe, '--dump-blk', blk_path], capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f'{exe} --dump-blk failed (rc {p.returncode}): {p.stderr.strip()}')
    with open(blk_path, 'rb') as fh:
        blk_bytes = fh.read()
    ok, samples, meta = parse_bin_frame(blk_bytes)
    ok_all &= check("blk frame decodes, CRC matches",
                     ok and list(samples) == expected_samples(N_EXPECTED)
                     and meta["p1"] == 5 and meta["p2"] == 5 and meta["samc"] == 10 and meta["pinsel"] == 3,
                     f"ok={ok} n={len(samples)} meta={meta}")

    p = subprocess.run([exe, '--dump-grab', grab_path], capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f'{exe} --dump-grab failed (rc {p.returncode}): {p.stderr.strip()}')
    with open(grab_path, 'rb') as fh:
        grab_bytes = fh.read()
    # parse_grab_frame() wants (header_line, payload, tail) separately, as
    # Target.grab() reads them off the wire one piece at a time - split the
    # one blob the same way here.
    header_end = grab_bytes.index(b"\r\n") + 2
    header_line = grab_bytes[:header_end].decode("ascii")
    rest = grab_bytes[header_end:]
    n = N_EXPECTED
    payload = rest[:2 * n]
    tail = rest[2 * n:]
    ok, samples, meta = protocol.parse_grab_frame(header_line, payload, tail + protocol.ACK)
    ok_all &= check("GRAB frame decodes with protocol.parse_grab_frame(), CRC matches",
                     ok and list(samples) == expected_samples(N_EXPECTED)
                     and meta["from_"] == 1024 and meta["ksps"] == 8000 and meta["overrun"] == 3
                     and meta["late"] == 1 and meta["missed"] == 0 and meta["halves"] == 250
                     and meta["transfers"] == 2000000 and meta["slpdat"] == 18 and meta["dac_hz"] == 400000000,
                     f"ok={ok} n={len(samples)} meta={meta}")

    print("test_frame_xcheck", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
