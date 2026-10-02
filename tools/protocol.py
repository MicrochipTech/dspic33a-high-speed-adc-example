#!/usr/bin/env python3
"""tools/protocol.py - the board's console wire protocol: CRC-16/CCITT-FALSE,
the "stream grab" GRAB frame parser, and Target, the serial console client.

Moved out of tools/adc_gui.py verbatim (P6.5 of docs/IMPLEMENTATION-PLAN.md)
so that a host-side tool can talk to the board's console without pulling in
NiceGUI or anything GUI-related. No NiceGUI, no asyncio event loop, no board
tables - just re, time, numpy (parse_grab_frame() decodes the payload with
np.frombuffer(), unchanged from the code this was moved out of) and pyserial,
and pyserial only inside Target.__init__, imported there and not at module
level, so importing this module - or exercising everything else in it, as
adc_gui.py's --selftest and tests/host/test_protocol.py do via a stand-in
serial object bypassing __init__ - does not require pyserial installed.

FakeTarget (the --fake/--selftest stand-in board) stays in tools/adc_gui.py:
it plays the firmware, not the console protocol, and pulls in
tools/eval_chain.py's synth() for its synthetic triangle - a GUI-side
concern, not a protocol one. probe_grab()/query_buf()/_parse_buf() also stay
in adc_gui.py: generic little helpers over a target's cmd(), but not part of
the CRC/GRAB-frame/Target move this task scoped.
"""
import re
import time

import numpy as np

ACK = b"\x06"
NAK = b"\x15"
BAUD = 115200


# ---------------------------------------------------------------------------
# CRC-16/CCITT-FALSE, shared by every binary frame the firmware sends
# (docs/PLAN-BINARY-TRANSFER.md): poly 0x1021, init 0xFFFF, no reflect, no
# xorout. Only "stream grab"'s GRAB frame uses it in this tool now - the
# back-to-back "blk" block transfer is retired here (the firmware command
# stays, for a terminal).
# ---------------------------------------------------------------------------
_CRC_LINE_RE = re.compile(rb"CRC ([0-9A-Fa-f]{4})")


def format_rtt(r) -> str:
    """Target.ping()'s result as one line, e.g. "RTT 84 ms (min 79, max 97, n=5)"."""
    return f"RTT {r['avg_ms']:.0f} ms (min {r['min_ms']:.0f}, max {r['max_ms']:.0f}, n={r['n']})"


def crc16_ccitt_false(data: bytes) -> int:
    """poly 0x1021, init 0xFFFF, no reflect, no xorout -- the frame's CRC."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


assert crc16_ccitt_false(b"123456789") == 0x29B1, "CRC-16/CCITT-FALSE check value"


# ---------------------------------------------------------------------------
# "stream grab": one halt/transfer/restart cycle of the standing chain
# (chaintest.c chain_stream_grab_begin/_end, cli.c cmd_stream_grab()). A
# text header line, the payload, a CRC line, then the usual prompt and
# ACK/NAK, same shape as the firmware's other binary frame ("blk", not used
# by this tool any more): the actual rate, where in the raw buffer the
# window starts, the stream counters since the PREVIOUS grab, and the DAC
# triangle setting (slpdat, DAC clock) the model slope is computed from.
# docs/PLAN-BINARY-TRANSFER.md.
# ---------------------------------------------------------------------------
_GRAB_HEADER_RE = re.compile(
    r"GRAB n=(\d+) from=(\d+) ksps=(\d+) ov=(\d+) late=(\d+) missed=(\d+) "
    r"halves=(\d+) xfer=(\d+) slp=(\d+) dachz=(\d+)(?: proc=(\d+))?(?: load=(\d+))?"
    r"(?: gz=(\d+) gzs=(\d+) gzd=(\d+))?(?: cnt=(\d+) cnr=(\d+) cpk=(\d+))?")
# proc= (01.10.2026): 1 = the payload is the firmware's signal processing
# result (src/core/sigproc.h, "sigproc on"), 0 = raw samples. Optional, so a
# firmware from before it parses as proc 0. load= (02.10.2026): the
# processing's mean share of a half period since the previous grab, per
# mille (1000 = it just keeps up); None when the firmware does not send it.
# Since 02.10.2026 proc= names the filter (0 none, 1 low-pass, 2 high-pass,
# 3 band-pass at fs/8, PROC_NAMES), and gz=/gzs=/gzd= - present only while
# the firmware's Goertzel at fs/16 runs - are its amplitude (LSB), the tone's
# share of the block's power (per mille) and detected (0/1): meta['gz'] is
# then a dict with amp/share_pm/detected, None otherwise.
PROC_NAMES = {0: "off", 1: "low-pass", 2: "high-pass", 3: "band-pass"}
# cnt=/cnr=/cpk= (CNT, 02.10.2026), present only while the impact counter
# runs: the count and its rate per second since the counter's reset, and the
# largest resonator magnitude since the previous grab (LSB) - meta['cnt'] is
# then a dict count/rate/peak, None otherwise.


def parse_grab_frame(header_line: str, payload: bytes, tail: bytes):
    """Decode one 'stream grab' frame from its three pieces (header text
    line, the payload bytes, everything after the payload up to and
    including the ACK/NAK). Used by both Target.grab() (read from serial)
    and FakeTarget.grab() (built in memory), so the same check runs against
    a real board and against the stand-in. Returns (ok, samples, meta); on
    any problem meta['error'] is set and ok is False."""
    m = _GRAB_HEADER_RE.match(header_line.strip())
    if not m:
        raise RuntimeError(f"grab: no GRAB header, got {header_line!r}")
    n = int(m.group(1))
    meta = dict(from_=int(m.group(2)), ksps=int(m.group(3)), overrun=int(m.group(4)),
                late=int(m.group(5)), missed=int(m.group(6)), halves=int(m.group(7)),
                transfers=int(m.group(8)), slpdat=int(m.group(9)), dac_hz=int(m.group(10)),
                proc=int(m.group(11) or 0),
                load_pm=int(m.group(12)) if m.group(12) is not None else None,
                gz=(dict(amp=int(m.group(13)), share_pm=int(m.group(14)), detected=int(m.group(15)))
                    if m.group(13) is not None else None),
                cnt=(dict(count=int(m.group(16)), rate=int(m.group(17)), peak=int(m.group(18)))
                     if m.group(16) is not None else None))
    m2 = _CRC_LINE_RE.search(tail)
    if not m2:
        raise RuntimeError(f"grab: no CRC line, got {tail!r}")
    crc_frame = int(m2.group(1), 16)
    crc_calc = crc16_ccitt_false(payload)
    samples = (np.frombuffer(payload, dtype="<u2").astype(int) & 0x0FFF) if n else np.zeros(0, dtype=int)
    ok = n > 0 and len(payload) == 2 * n and crc_frame == crc_calc and tail.endswith(ACK)
    if n == 0:
        meta["error"] = "NAK: no stream on, or the halt/restart failed"
    elif len(payload) != 2 * n:
        meta["error"] = f"short block: got {len(payload)} of {2 * n} bytes"
    elif crc_frame != crc_calc:
        meta["error"] = f"CRC mismatch: frame {crc_frame:04X}, computed {crc_calc:04X}"
    elif not tail.endswith(ACK):
        meta["error"] = "NAK after block"
    return ok, samples, meta


# ---------------------------------------------------------------------------
# Transport: the board's console over a COM port
# ---------------------------------------------------------------------------
class Target:
    """One command at a time, synchronised on the parser's ACK/NAK byte."""

    def __init__(self, port: str, baud: int = BAUD, on_log=None, sync_timeout: float = 20.0):
        import serial  # pyserial
        self.on_log = on_log  # optional callable(str): the console transcript
        # serial_for_url() opens a plain COM port exactly like serial.Serial()
        # did (its "port" and "hwgrep://" handlers fall back to it for any
        # string without "://"), and additionally understands "socket://
        # host:port" - the tunnel URL tools/remote.py's RemoteBench hands
        # back, transparent to everything below this line.
        self.ser = serial.serial_for_url(port, baud, timeout=0.05)
        self.port = port
        # sync_timeout: 20 s covers a board still booting; a caller that only
        # wants to know whether a board answers at all (remote.py's
        # RemoteBench.check_board()) passes less. On a timeout the port is
        # closed again before the TimeoutError leaves - the caller never got
        # an object to close.
        try:
            self.sync(timeout=sync_timeout)
        except TimeoutError:
            self.ser.close()
            raise

    def close(self):
        self.ser.close()

    def _log(self, line: str):
        if self.on_log:
            try:
                self.on_log(line)
            except Exception:
                pass

    def _read_until_ready(self, timeout: float) -> bytes:
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk
                if buf.endswith(ACK) or buf.endswith(NAK):
                    return buf
            # A dump of 1024 samples takes ~0.5 s at 115200; keep reading.
        raise TimeoutError(f"no ACK/NAK within {timeout} s; got {len(buf)} bytes: {buf[-80:]!r}")

    def sync(self, timeout: float = 20.0):
        """Wait for the board to be ready. After power-up the boot, the
        self-test, the rate test and the automatic sweep take several
        seconds; an empty line answers with a prompt once the parser runs."""
        self.ser.reset_input_buffer()
        self.ser.write(b"\r")
        self._read_until_ready(timeout)

    def ping(self, n: int = 5, timeout: float = 5.0):
        """Round-trip delay to the board's parser: n times an empty line out,
        the prompt and its ACK back - the shortest exchange the console has
        (a few bytes each way, no command runs). Over a local COM port that
        is the USB/UART path, over a bench_client tunnel ("socket://...")
        additionally host -> relay -> agent -> COM port and back, which is
        what this is for: one number at the start of a remote session.
        Reads byte-wise (ser.read(1) returns on the first byte, in_waiting
        for the rest) rather than through _read_until_ready(), whose
        ser.read(4096) waits out the port's 50 ms timeout whenever fewer than
        4096 bytes arrive and would add up to 50 ms to every sample.
        Returns dict(n, min_ms, avg_ms, max_ms, samples_ms)."""
        samples = []
        for _ in range(n):
            self.ser.reset_input_buffer()
            t0 = time.perf_counter()
            self.ser.write(b"\r")
            buf = b""
            while not (buf.endswith(ACK) or buf.endswith(NAK)):
                if time.perf_counter() - t0 > timeout:
                    raise TimeoutError(f"ping: no ACK/NAK within {timeout} s; got {buf[-40:]!r}")
                buf += self.ser.read(max(1, self.ser.in_waiting))
            samples.append((time.perf_counter() - t0) * 1000.0)
        r = dict(n=n, min_ms=min(samples), avg_ms=sum(samples) / n, max_ms=max(samples),
                 samples_ms=samples)
        self._log(f"# round trip: {format_rtt(r)}")
        return r

    def cmd(self, line: str, timeout: float = 5.0):
        """Send one command, return (ok, reply_lines) without echo and prompt."""
        self._log(f"> {line}")
        self.ser.reset_input_buffer()
        self.ser.write(line.encode("ascii") + b"\r")
        raw = self._read_until_ready(timeout)
        ok = raw.endswith(ACK)
        text = raw[:-1].decode("ascii", "replace")
        lines = [l.rstrip("\r") for l in text.split("\n")]
        for l in lines:
            if l.strip():
                self._log(f"< {l.rstrip()}")
        self._log(f"< {'[ACK]' if ok else '[NAK]'}")
        # Drop the echo of the command and the prompt line.
        out = []
        for l in lines:
            s = l.strip()
            if not s or s == line.strip() or s.startswith("> ") or s == ">":
                continue
            out.append(l.rstrip())
        return ok, out

    def _read_line(self, timeout: float) -> str:
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            b = self.ser.read(1)
            if b:
                buf += b
                if buf.endswith(b"\n"):
                    return buf.decode("ascii", "replace")
        raise TimeoutError(f"grab: no line within {timeout} s, got {buf!r}")

    def _read_exact(self, n: int, timeout: float) -> bytes:
        buf = b""
        t0 = time.time()
        while len(buf) < n and time.time() - t0 < timeout:
            chunk = self.ser.read(n - len(buf))
            if chunk:
                buf += chunk
        if len(buf) < n:
            raise TimeoutError(f"grab: expected {n} bytes, got {len(buf)} within {timeout} s")
        return buf

    def grab(self, timeout: float = 10.0):
        """'stream grab': one halt/transfer/restart cycle of the standing
        chain. Reads the header first to learn the byte count before
        looking for ACK/NAK -- a sample byte can equal 0x06 or 0x15 by
        chance, so the generic ACK/NAK scan in _read_until_ready() must not
        run over the payload (see parse_grab_frame). Only the ASCII framing
        is logged to the console transcript (on_log); the sample bytes
        themselves are not."""
        self._log("> stream grab")
        self.ser.reset_input_buffer()
        self.ser.write(b"stream grab\r")
        echo = self._read_line(timeout)
        self._log(f"< {echo.rstrip()}")
        header_line = self._read_line(timeout)
        self._log(f"< {header_line.rstrip()}")
        m = _GRAB_HEADER_RE.match(header_line.strip())
        if not m:
            raise RuntimeError(f"grab: unsupported or no GRAB header, got {header_line!r}")
        n = int(m.group(1))
        payload = self._read_exact(2 * n, timeout) if n else b""
        if payload:
            self._log(f"< [binary payload, {len(payload)} bytes, not shown]")
        tail = self._read_until_ready(timeout)
        ok = tail.endswith(ACK)
        tail_text = tail[:-1].decode("ascii", "replace") if tail else ""
        for l in tail_text.split("\n"):
            if l.strip():
                self._log(f"< {l.rstrip()}")
        self._log(f"< {'[ACK]' if ok else '[NAK]'}")
        return parse_grab_frame(header_line, payload, tail)
