#!/usr/bin/env python3
"""
adc_gui.py - a browser GUI for the ADC/DMA example: set the sample rate and
the ADC parameters over the console, capture a block, transfer it, plot it
with its FFT, repeat.

How it works
  The firmware streams continuously into its ping-pong buffer. This tool
  does not stream from it (115200 baud could not carry it); it works in
  cycles, exactly as a scope with a single-shot trigger would:

      start -> let the board run for a moment -> stop -> dump N samples of
      the last completed buffer half -> plot time signal and FFT -> repeat

  Every command goes through the board's console (cli.c) and is
  acknowledged with the parser's ACK/NAK byte after the prompt, so the
  tool never guesses whether the board is ready (cmd_parser.h, "Prompt as
  a protocol element").

Modes
  --fake        no board: a built-in stand-in answers the same commands and
                delivers a synthetic signal (sine + harmonic + noise) at the
                configured rate. For testing the GUI.
  --port COMx   the board. Without --port the page offers a port list.
  --selftest    no GUI: run the fake target through one capture cycle,
                parse, FFT, print the numbers, exit 0/1.

Requirements: nicegui, pyserial, numpy  (pip install -r requirements-gui.txt)
"""
import argparse
import asyncio
import math
import re
import sys
import time

import numpy as np

ACK = b"\x06"
NAK = b"\x15"
BAUD = 115200

# Sample rate from the board's pacing source and period (see board.h,
# capture.c): repeat timer = 80000 / period kSPS, SCCP1 = 100000 / period.
PACING = {
    3:  ("ADC repeat timer (period in TAD, 2..63)",   lambda n: 80000.0 / n,  2, 63),
    32: ("SCCP1 timer (period in 10 ns ticks, 4..)",   lambda n: 100000.0 / n, 2, 65535),
    2:  ("back-to-back (no rate control)",            lambda n: float("nan"), 0, 0),
}


def rate_ksps(pacing: int, period: int) -> float:
    name, f, lo, hi = PACING[pacing]
    return f(period) if period else float("nan")


# ---------------------------------------------------------------------------
# Transport: the board's console over a COM port
# ---------------------------------------------------------------------------
class Target:
    """One command at a time, synchronised on the parser's ACK/NAK byte."""

    def __init__(self, port: str, baud: int = BAUD):
        import serial  # pyserial
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.port = port
        self.sync()

    def close(self):
        self.ser.close()

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

    def cmd(self, line: str, timeout: float = 5.0):
        """Send one command, return (ok, reply_lines) without echo and prompt."""
        self.ser.reset_input_buffer()
        self.ser.write(line.encode("ascii") + b"\r")
        raw = self._read_until_ready(timeout)
        ok = raw.endswith(ACK)
        text = raw[:-1].decode("ascii", "replace")
        lines = [l.rstrip("\r") for l in text.split("\n")]
        # Drop the echo of the command and the prompt line.
        out = []
        for l in lines:
            s = l.strip()
            if not s or s == line.strip() or s.startswith("> ") or s == ">":
                continue
            out.append(l.rstrip())
        return ok, out


class FakeTarget:
    """Answers like cli.c would, delivers a synthetic signal at the configured
    rate: a sine at `signal_khz` with a small second harmonic and noise,
    12-bit around mid scale. Used with --fake and --selftest."""

    def __init__(self, signal_khz: float = 100.0):
        self.port = "fake"
        self.pacing, self.period = 3, 4          # 20 MSPS nominal
        self.samc, self.input = 0, 5
        self.running = False
        self.signal_khz = signal_khz
        self.phase = 0.0
        self.counters = dict(overrun=0, late=0, missed=0, addr_err=0, bus_err=0, blocks=0)
        self.rng = np.random.default_rng(1)

    def close(self):
        pass

    def _samples(self, n: int) -> np.ndarray:
        fs = rate_ksps(self.pacing, self.period) * 1e3
        if not math.isfinite(fs):
            fs = 40e6
        t = (np.arange(n) / fs) + self.phase
        self.phase += n / fs
        f = self.signal_khz * 1e3
        v = 2048 + 1500 * np.sin(2 * np.pi * f * t) + 150 * np.sin(2 * np.pi * 2 * f * t + 0.7)
        v += self.rng.normal(0, 6, n)
        if self.input == 6:                       # the internal reference
            v = np.full(n, 3840.0) + self.rng.normal(0, 2, n)
        return np.clip(np.round(v), 0, 4095).astype(int)

    def cmd(self, line: str, timeout: float = 5.0):
        parts = line.split()
        if not parts:
            return True, []
        c, args = parts[0], parts[1:]
        try:
            if c == "start":
                self.running = True;  return True, ["running: 1"]
            if c == "stop":
                self.running = False; return True, ["running: 0"]
            if c == "period":
                n = int(args[0]); lo, hi = PACING[self.pacing][2:4]
                if self.pacing == 2 or not (lo <= n <= hi):
                    return False, ["period: out of range for the active pacing, or back-to-back (no period)"]
                self.period = n
                return True, [f"period: {n}", f"ksps nominal: {int(rate_ksps(self.pacing, n))}"]
            if c == "pacing":
                p = int(args[0])
                if p not in PACING:
                    return False, ["usage: pacing <3|32|2>"]
                self.pacing = p
                self.period = {3: 4, 32: 5, 2: 0}[p]
                return True, [f"pacing: {p}", PACING[p][0], f"period: {self.period}"]
            if c == "samc":
                self.samc = int(args[0]);  return True, [f"samc: {self.samc}"]
            if c == "input":
                self.input = int(args[0]); return True, [f"input: {self.input}"]
            if c == "status":
                self.counters["blocks"] += 17
                return True, [f"running: {int(self.running)}", f"blocks: {self.counters['blocks']}",
                              "overrun: 0", "late: 0", "missed: 0", "addr_err: 0", "bus_err: 0",
                              f"input: {self.input}", f"samc: {self.samc}",
                              f"pacing: {self.pacing}", f"period: {self.period}",
                              "last: 1798", "selftest_mean: 3840", "fail_code: 0"]
            if c == "version":
                return True, ["adc_dma_40msps (fake target)", "board: none, synthetic signal"]
            if c == "dump":
                count = int(args[0]) if args else 64
                offset = int(args[1]) if len(args) > 1 else 0
                if not (1 <= count <= 1024) or not (0 <= offset <= 1023):
                    return False, ["usage: dump [count 1..1024] [offset 0..1023]"]
                count = min(count, 1024 - offset)
                v = self._samples(count)
                lines = []
                for i in range(0, count, 8):
                    lines.append(f"{offset + i:04d}:" + "".join(f" {x}" for x in v[i:i + 8]))
                return True, lines
        except (ValueError, IndexError):
            return False, ["usage error"]
        return False, ["unknown command"]


def parse_dump(lines) -> np.ndarray:
    """'0000: 2048 2298 ...' lines -> samples, in index order."""
    vals = {}
    for l in lines:
        m = re.match(r"\s*(\d+):((?:\s+\d+)+)\s*$", l)
        if m:
            idx = int(m.group(1))
            for k, x in enumerate(m.group(2).split()):
                vals[idx + k] = int(x)
    if not vals:
        return np.zeros(0, dtype=int)
    n = max(vals) + 1
    out = np.zeros(n, dtype=int)
    for i, x in vals.items():
        out[i] = x
    return out[min(vals):]


def parse_status(lines) -> dict:
    d = {}
    for l in lines:
        m = re.match(r"\s*([a-z_]+):\s*(-?\d+)\s*$", l)
        if m:
            d[m.group(1)] = int(m.group(2))
    return d


def spectrum(samples: np.ndarray, fs_hz: float):
    """Hann-windowed magnitude spectrum in dB relative to full scale (4096 pk-pk).
    Returns (freq_hz, db)."""
    n = len(samples)
    if n < 8 or not math.isfinite(fs_hz):
        return np.zeros(0), np.zeros(0)
    x = samples.astype(float) - samples.mean()
    w = np.hanning(n)
    spec = np.abs(np.fft.rfft(x * w)) * 2.0 / (w.sum())
    db = 20 * np.log10(np.maximum(spec, 1e-3) / 2048.0)
    f = np.fft.rfftfreq(n, d=1.0 / fs_hz)
    return f, db


# ---------------------------------------------------------------------------
# One capture cycle: start, run a moment, stop, dump, status
# ---------------------------------------------------------------------------
def capture_cycle(target, count: int, settle_s: float = 0.02):
    ok, _ = target.cmd("start")
    if not ok:
        raise RuntimeError("start refused")
    time.sleep(settle_s)
    target.cmd("stop")
    ok, lines = target.cmd(f"dump {count} 0", timeout=10.0)
    if not ok:
        raise RuntimeError("dump refused: " + " ".join(lines))
    samples = parse_dump(lines)
    ok, st = target.cmd("status")
    return samples, parse_status(st) if ok else {}


def selftest() -> int:
    t = FakeTarget(signal_khz=250.0)
    for c in ("pacing 3", "period 4", "samc 0", "input 5"):
        ok, r = t.cmd(c)
        assert ok, (c, r)
    samples, status = capture_cycle(t, 1024, settle_s=0.0)
    fs = rate_ksps(3, 4) * 1e3
    f, db = spectrum(samples, fs)
    peak = f[np.argmax(db[1:]) + 1]
    print(f"samples: {len(samples)}, min {samples.min()} max {samples.max()} mean {samples.mean():.0f}")
    print(f"fs {fs/1e6:.3f} MHz, FFT peak at {peak/1e3:.1f} kHz (expect 250.0), status keys {sorted(status)[:5]}...")
    ok = len(samples) == 1024 and abs(peak - 250e3) < fs / 1024 and status.get("pacing") == 3
    print("selftest", "PASS" if ok else "FAIL")
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# The GUI
# ---------------------------------------------------------------------------
def main_gui(args):
    from nicegui import ui, run

    state = dict(target=None, live=False, busy=False, cycles=0)

    def ports():
        try:
            from serial.tools import list_ports
            return [p.device for p in list_ports.comports()]
        except Exception:
            return []

    # ---- header: connection ----
    with ui.header().classes("items-center gap-4"):
        ui.label("dsPIC33A ADC/DMA - capture, plot, FFT").classes("text-lg")
        port_sel = ui.select(options=["fake"] + ports(), value=args.port or ("fake" if args.fake else None),
                             label="port").classes("w-40")
        conn_btn = ui.button("connect")
        conn_lbl = ui.label("not connected")

    # ---- settings ----
    with ui.row().classes("w-full gap-6"):
        with ui.card().classes("w-96"):
            ui.label("Sample rate").classes("text-base font-medium")
            pacing_sel = ui.select({k: v[0] for k, v in PACING.items()}, value=3, label="pacing (TRG2SRC)")
            period_in = ui.number("period", value=4, min=2, max=65535, step=1, format="%d")
            rate_lbl = ui.label()
            samc_in = ui.number("samc (sample time, 0..31)", value=0, min=0, max=31, step=1, format="%d")
            input_in = ui.number("input (PINSEL, 6 = internal ref)", value=5, min=0, max=15, step=1, format="%d")
            apply_btn = ui.button("apply to board")
            apply_lbl = ui.label()
        with ui.card().classes("w-96"):
            ui.label("Capture").classes("text-base font-medium")
            count_sel = ui.select([64, 128, 256, 512, 1024], value=1024, label="samples per capture (one buffer half max)")
            interval_in = ui.number("live interval, ms", value=500, min=100, max=5000, step=100, format="%d")
            with ui.row():
                single_btn = ui.button("single capture")
                live_btn = ui.button("live start")
            cyc_lbl = ui.label()
            cnt_lbl = ui.label().classes("text-sm")
            if args.fake or True:
                sig_in = ui.number("fake signal, kHz (fake target only)", value=100.0, min=0.1, max=20000.0, step=10)

    # ---- charts ----
    time_chart = ui.echart({
        "animation": False,
        "grid": {"left": 60, "right": 20, "top": 30, "bottom": 40},
        "xAxis": {"type": "value", "name": "sample", "min": 0},
        "yAxis": {"type": "value", "name": "ADC counts", "min": 0, "max": 4096},
        "series": [{"type": "line", "showSymbol": False, "data": [], "lineStyle": {"width": 1}}],
        "title": {"text": "time signal", "left": "center"},
    }).classes("w-full h-72")
    fft_chart = ui.echart({
        "animation": False,
        "grid": {"left": 60, "right": 20, "top": 30, "bottom": 40},
        "xAxis": {"type": "value", "name": "kHz", "min": 0},
        "yAxis": {"type": "value", "name": "dBFS", "min": -100, "max": 0},
        "series": [{"type": "line", "showSymbol": False, "data": [], "lineStyle": {"width": 1}}],
        "title": {"text": "spectrum (Hann window)", "left": "center"},
    }).classes("w-full h-72")

    def update_rate_label():
        try:
            r = rate_ksps(int(pacing_sel.value), int(period_in.value or 0))
            rate_lbl.text = f"nominal rate: {r/1000:.3f} MSPS" if math.isfinite(r) else "nominal rate: not under control (back-to-back)"
        except Exception:
            rate_lbl.text = ""
    pacing_sel.on_value_change(lambda e: update_rate_label())
    period_in.on_value_change(lambda e: update_rate_label())
    update_rate_label()

    # ---- connection ----
    def do_connect():
        if state["target"]:
            state["live"] = False
            state["target"].close()
            state["target"] = None
            conn_btn.text = "connect"
            conn_lbl.text = "not connected"
            return
        try:
            if port_sel.value == "fake":
                state["target"] = FakeTarget(signal_khz=float(sig_in.value or 100.0))
            else:
                state["target"] = Target(port_sel.value)
            ok, lines = state["target"].cmd("version")
            conn_lbl.text = " | ".join(lines[:2]) if ok else "connected, version refused"
            conn_btn.text = "disconnect"
        except Exception as ex:
            conn_lbl.text = f"connect failed: {ex}"
            state["target"] = None
    conn_btn.on_click(do_connect)

    async def apply_settings():
        t = state["target"]
        if not t:
            apply_lbl.text = "not connected"
            return
        if isinstance(t, FakeTarget):
            t.signal_khz = float(sig_in.value or 100.0)
        msgs = []
        for c in (f"pacing {int(pacing_sel.value)}", f"period {int(period_in.value or 0)}",
                  f"samc {int(samc_in.value or 0)}", f"input {int(input_in.value or 0)}"):
            if c.startswith("period") and int(pacing_sel.value) == 2:
                continue
            ok, lines = await run.io_bound(t.cmd, c)
            msgs.append(("ok " if ok else "NAK ") + c + (": " + lines[0] if lines else ""))
        apply_lbl.text = " | ".join(msgs)
    apply_btn.on_click(apply_settings)

    # ---- capture ----
    async def one_cycle():
        t = state["target"]
        if not t or state["busy"]:
            return
        state["busy"] = True
        try:
            samples, status = await run.io_bound(capture_cycle, t, int(count_sel.value))
            fs = rate_ksps(int(status.get("pacing", pacing_sel.value)), int(status.get("period", period_in.value or 0))) * 1e3
            f, db = spectrum(samples, fs)
            time_chart.options["series"][0]["data"] = [[i, int(v)] for i, v in enumerate(samples)]
            time_chart.options["xAxis"]["max"] = max(len(samples) - 1, 1)
            time_chart.update()
            fft_chart.options["series"][0]["data"] = [[float(fx) / 1e3, float(d)] for fx, d in zip(f, db)]
            fft_chart.options["xAxis"]["max"] = float(f[-1]) / 1e3 if len(f) else 1
            fft_chart.update()
            state["cycles"] += 1
            peak = (f[np.argmax(db[1:]) + 1] / 1e3) if len(db) > 1 else float("nan")
            cyc_lbl.text = (f"cycle {state['cycles']}: {len(samples)} samples, "
                            f"fs {fs/1e6:.3f} MHz" + (f", peak {peak:.1f} kHz" if math.isfinite(peak) else ""))
            cnt_lbl.text = " ".join(f"{k}={status[k]}" for k in ("overrun", "late", "missed", "addr_err", "bus_err", "blocks") if k in status)
        except Exception as ex:
            cyc_lbl.text = f"cycle failed: {ex}"
            state["live"] = False
            live_btn.text = "live start"
        finally:
            state["busy"] = False

    single_btn.on_click(one_cycle)

    async def live_loop():
        while state["live"]:
            await one_cycle()
            await asyncio.sleep(max(0.05, float(interval_in.value or 500) / 1000.0))

    def toggle_live():
        state["live"] = not state["live"]
        live_btn.text = "live stop" if state["live"] else "live start"
        if state["live"]:
            asyncio.create_task(live_loop())
    live_btn.on_click(toggle_live)

    if args.fake or args.port:
        ui.timer(0.5, do_connect, once=True)

    ui.run(title="ADC/DMA capture", port=args.http_port, show=not args.no_browser, reload=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="COM port of the board's console")
    ap.add_argument("--fake", action="store_true", help="use the built-in stand-in instead of a board")
    ap.add_argument("--selftest", action="store_true", help="fake target through one cycle, no GUI")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--no-browser", action="store_true", help="do not open a browser window")
    args = ap.parse_args()
    if args.selftest:
        sys.exit(selftest())
    main_gui(args)


if __name__ in {"__main__", "__mp_main__"}:
    main()
