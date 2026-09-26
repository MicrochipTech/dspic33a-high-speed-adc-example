#!/usr/bin/env python3
"""
adc_gui.py - a browser GUI for the ADC/DMA example: configure the triggered
chain stream, capture its live data over the console, plot it with its FFT,
repeat.

How it works
  The only data path is the triggered chain (SCCP1 -> ADC -> DMA0 ->
  ping-pong, chaintest.c/cli.c): "stream on <ksps> [core pinsel [samc]]"
  starts it and the firmware's main loop processes every half from then on;
  this tool's own cycle is "stream grab" - halt the trigger just long
  enough to send the half that stood still as one binary frame, restart it,
  repeat (docs/PLAN-BINARY-TRANSFER.md, the GRAB frame). The back-to-back
  burst mode ("pll"/"snap"/"dump"/"blk", the sweep tile) is retired from
  this tool - the owner's decision, 25.09.2026: the triggered chain is the
  only path the GUI shows now. The firmware keeps the back-to-back commands
  for a terminal; this tool simply no longer sends them.

Modes
  --fake        no board: a built-in stand-in answers the same commands.
                With the DAC2 test triangle input it plays back the same
                synthetic triangle the firmware's own chain test is judged
                against (tools/eval_chain.py's synth()); with any other
                input it plays a sine with harmonics and noise, so
                SNR/THD/harmonics have something to show in the spectrum.
  --port COMx   the board. Without --port the page offers a port list.
  --selftest    no GUI: run the fake target through the stream/grab cycle,
                parse, FFT, judge the triangle, print the numbers, exit 0/1.
  --settings F  settings file, read at start-up and written by "save"
                (default: adc_gui_settings.json next to this script). Every
                control on the page is in it, so a session survives a
                restart; "save as" and "load as" name a different file. An
                older file (from before 25.09.2026, with "pll"/"sweep"/
                "capture" keys) still loads - those keys are simply not
                read any more.

Requirements: nicegui, pyserial, numpy  (pip install -r requirements-gui.txt)
"""
import argparse
import asyncio
import json
import math
import os
import re
import sys
import time

import numpy as np

ACK = b"\x06"
NAK = b"\x15"
BAUD = 115200


# ---------------------------------------------------------------------------
# Packages, boards, and where a channel comes out
#
# 'core <1..5> [pinsel]' and 'input <0..15>' exist in cli.c. Which package
# pin such a pair reads is silicon layout: pins128.py (MPS512, TQFP-128)
# and pins64.py (MPS506, the Nano's 64-pin part) carry the data sheet's own
# tables, DS70005591D Tables 11 and 5, so a pair is resolved from the
# document and never guessed.
#
# Where that pin comes out on a board is board layout, and that is
# boards.py: the DIM information sheet DS70005563A for the EV74H48A, the
# Curiosity Nano user guide DS70005634A for the EV17P63A.
# ---------------------------------------------------------------------------
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pins128  # noqa: E402  (needs the path above)
import pins64  # noqa: E402
from boards import BOARDS  # noqa: E402
# The triangle analysis reuses the SAME evaluator the firmware's tri_eval()
# (chaintest.c) is ported from, rather than a second implementation that
# could silently disagree with it; synth() also backs the fake target's
# "stream grab" frames for the test input (host-testable without a board).
from eval_chain import tri_eval as chain_tri_eval  # noqa: E402
from eval_chain import grid_ok as chain_grid_ok  # noqa: E402
from eval_chain import synth as chain_synth  # noqa: E402


# ---------------------------------------------------------------------------
# Settings file
#
# Every control on the page has its value here, so a session can be put
# down and picked up: the file is read at start-up, "save" writes the
# controls back to it, "save as" and "load as" name a different one. The
# built-in defaults below are the fallback for a missing file and for
# any key a file does not carry, so an old or hand-edited file still loads.
# ---------------------------------------------------------------------------
SETTINGS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "adc_gui_settings.json")
SETTINGS_VERSION = 2
SETTINGS_DEFAULTS = {
    "version": SETTINGS_VERSION,
    "board": "EV74H48A",
    "view": {"dac_source": 0, "tooltips": True, "vref": 3.3},
    "acquisition": {
        "mode": "test",           # "test" (RA8/DAC2 triangle, core 5 pin 3) or "custom"
        "ksps": 8000,
        "core": 3, "pinsel": 5, "samc": 0,
        "interval_ms": 500,
    },
    "buffer": {"size": 2048},
    "dac": {
        "1": {"on": False, "low": 0x100, "high": 0xF00, "slpdat": 8},
        "2": {"on": False, "low": 0x100, "high": 0xF00, "slpdat": 8},
    },
    "fake": {"signal_khz": 100.0, "amplitude": 1500.0,
             "noise": 6.0, "harmonic2": 150.0, "harmonic3": 0.0},
}


def settings_merge(base, over):
    """`over` wins, key by key, one level deep per section - a file that
    carries only half a section keeps the defaults for the other half."""
    out = {}
    for key, val in base.items():
        if isinstance(val, dict) and isinstance(over.get(key), dict):
            out[key] = settings_merge(val, over[key])
        else:
            out[key] = over.get(key, val)
    for key, val in over.items():
        out.setdefault(key, val)
    return out


def settings_read(path):
    """(settings, message). A missing file is not an error: it is the
    first start, and the defaults are the answer."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            got = json.load(fh)
    except FileNotFoundError:
        return dict(SETTINGS_DEFAULTS), f"{os.path.basename(path)} not there yet, using defaults"
    except (OSError, ValueError) as exc:
        return dict(SETTINGS_DEFAULTS), f"{os.path.basename(path)}: {exc} - using defaults"
    return settings_merge(SETTINGS_DEFAULTS, got), f"loaded {os.path.basename(path)}"


def settings_write(path, data):
    try:
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(data, fh, indent=2, sort_keys=True)
            fh.write("\n")
    except OSError as exc:
        return f"could not write {path}: {exc}"
    return f"saved {os.path.basename(path)}"


BOARD_DEFAULT = "EV74H48A"
BOARD_OPTIONS = {key: b["title"] for key, b in BOARDS.items()}

PORT_RE = re.compile(r"R[A-H]\d{1,2}")
ADC_IN_RE = re.compile(r"AD([1-5])AN(\d{1,2})")
ADC_NEG_RE = re.compile(r"AD([1-5])ANN(\d{1,2})")


class Pin:
    """One package pin, exactly as the data sheet's table lists it."""

    __slots__ = ("n", "functions", "port", "adc", "dac", "kind")

    def __init__(self, n, functions):
        toks = [t for t in functions.split("/") if t]
        self.n = n
        self.functions = toks
        self.port = next((t for t in reversed(toks) if PORT_RE.fullmatch(t)), None)
        self.adc = [t for t in toks if ADC_IN_RE.fullmatch(t) or ADC_NEG_RE.fullmatch(t)]
        self.dac = [t for t in toks if t.startswith("DACOUT")]
        if self.port:
            self.kind = "io"
        elif toks[0] in ("VSS", "AVSS", "SWVSS"):
            self.kind = "gnd"
        elif toks[0] in ("VDD", "AVDD", "VDDCORE", "SWVDD", "LX", "VBAT"):
            self.kind = "pwr"
        elif toks[0] == "NC":
            self.kind = "nc"
        else:
            self.kind = "special"


class Package:
    """A device package: its pins, and which pin each (core, PINSEL) reads.
    Positive inputs only -- ADnANNm is the negative input of a differential
    pair (NINSEL), not what PINSEL selects."""

    def __init__(self, module):
        self.per_side = module.PINS_PER_SIDE
        self.pins = [Pin(n, module.PIN_FUNCTIONS[n]) for n in sorted(module.PIN_FUNCTIONS)]
        self.count = len(self.pins)
        self.adc_pin = {}
        for pin in self.pins:
            for fn in pin.adc:
                m = ADC_IN_RE.fullmatch(fn)
                if m:
                    self.adc_pin[(int(m.group(1)), int(m.group(2)))] = pin

    def by_number(self, n):
        return self.pins[n - 1] if 1 <= n <= self.count else None

    def channels(self, core):
        """PINSEL values this package brings out for `core`, plus the two
        internal ones every core has."""
        got = sorted(ps for (c, ps) in self.adc_pin if c == core)
        return got + [ps for ps in sorted(INTERNAL_INPUTS) if ps not in got]


PACKAGES = {128: Package(pins128), 64: Package(pins64)}

# PINSEL values that stay inside the chip on every core (Table 16-2).
INTERNAL_INPUTS = {
    6: "internal 15/16 x VDD reference, the self-test input - no pin",
    7: "internal UREF line - no pin, and the way a DAC reaches any core "
       "without a wire",
}

# UREF: one reference line for the whole chip, and AN7 of every core samples
# it (Table 16-2, "ADC n UREF input"). UREFCON has a single INSEL field -
# one instance at 0x3B20, one SFR - which picks what sits on the line:
# 1 AVDD/2, 2 VDD/2, 3 VDDcore, 4 bandgap, 5 temperature, 6..13 DAC1..DAC8,
# 14 AVSS, 15 AVDD. DAC1 is 6, DAC2 is 7, and because the line is shared
# only one DAC can be on it at a time. UREFOUTEN would also drive it onto a
# pin; the internal path does not need that.
UREF_INSEL_OF_DAC = {1: 6, 2: 7}
UREF_CHANNEL = 7
UREF_NOTE = ("one line for the whole chip, so only one DAC at a time; "
             "measured on core 3 so far, the other four follow from Table 16-2")


# Board-level notes the documents do not spell out but this project knows.
BOARD_NOTES = {
    ("EV74H48A", 3, 5): "the example's default measurement input",
    ("EV74H48A", 5, 3): "DACOUT2 is on this pin: ADC5 reads DAC2 back with no wire (phase 2 of the boot tests)",
    ("EV17P63A", 5, 3): "DACOUT2 is on this pin: ADC5 reads DAC2 back with no wire (phase 2 of the boot tests)",
    ("EV17P63A", 1, 0): "the Nano profile's default measurement input (board.h)",
}


def package_of(board_key):
    return PACKAGES[BOARDS[board_key]["pin_count"]]


def channel_pin(board_key, core, pinsel):
    """The package pin (core, PINSEL) reads on this board's device, or None
    for an internal input and for a pair the package does not bring out."""
    return package_of(board_key).adc_pin.get((core, pinsel))


def channel_site(board_key, core, pinsel):
    """Where the channel comes out on the board: (kind, label, detail).
    kind is 'connector', 'edge', 'onboard', 'internal' or 'none'."""
    board = BOARDS[board_key]
    if pinsel in INTERNAL_INPUTS:
        return ("internal", INTERNAL_INPUTS[pinsel], "")
    pin = channel_pin(board_key, core, pinsel)
    if pin is None:
        return ("none", f"AD{core}AN{pinsel} is not brought out in the {board['package']} package", "")
    for name, socket in board.get("connectors", {}).items():
        for socket_pin, entry in sorted(socket.items()):
            if entry["dev"] == pin.n:
                sig = f" ({entry['sig']})" if entry["sig"] else ""
                return ("connector", f"{name} pin {socket_pin}{sig}", f"device pin {pin.n} · {pin.port}")
    for side, row in board.get("edge_rows", {}).items():
        for pad in row:
            if pad["dev"] == pin.n:
                return ("edge", f"{side} edge row, pad {pad['pos']} ({pad['label']})",
                        f"device pin {pin.n} · {pin.port}")
    if pin.n in board.get("onboard", {}):
        return ("onboard", board["onboard"][pin.n], f"device pin {pin.n} · {pin.port}")
    return ("none", f"device pin {pin.n} ({pin.port}) is not brought out on this board", "")


# The two DACs with an output buffer, and the ADC input that shares each
# pin - the only two channels a DAC can reach without a wire (dac.h).
DAC_UNITS = (1, 2)
DAC_OPTIONS = {0: "no DAC", 1: "DAC1 · RA1", 2: "DAC2 · RA8"}


def dac_pin(board_key, unit):
    """The package pin DACOUTn drives, from the pin table itself."""
    if unit not in DAC_UNITS:
        return None
    want = f"DACOUT{unit}"
    for pin in package_of(board_key).pins:
        if want in pin.functions:
            return pin
    return None


def dac_channels(board_key, unit):
    """(core, PINSEL) pairs that read the DAC's own pin - no wire needed."""
    pin = dac_pin(board_key, unit)
    if pin is None:
        return []
    pkg = package_of(board_key)
    return [key for key, p in pkg.adc_pin.items() if p.n == pin.n]


def wire_hint(board_key, core, pinsel, unit):
    """What it takes to get this DAC into this ADC channel:
    (needed, headline, detail). `needed` is False for the two routes that
    need no wire at all - the DAC's own pin, and the internal UREF line."""
    if unit not in DAC_UNITS:
        return (False, "", "")
    dpin = dac_pin(board_key, unit)
    apin = channel_pin(board_key, core, pinsel)
    if pinsel == UREF_CHANNEL:
        return (False,
                f"internal: UREF carries DAC{unit}, read as AN7 - no pin, no wire, any core",
                f"UREFCON.INSEL = {UREF_INSEL_OF_DAC[unit]} puts DAC{unit} on the line; {UREF_NOTE}")
    if dpin is None:
        return (True, f"DACOUT{unit} is not brought out in this package", "")
    if apin is None:
        why = "the self-test reference" if pinsel == 6 else "not brought out"
        return (True, f"AD{core}AN{pinsel} is {why} - a wire cannot reach it",
                "for a DAC signal without a wire, pick channel AN7: UREF reaches every core")
    if apin.n == dpin.n:
        return (False, f"no wire needed: DAC{unit} drives pin {dpin.n} ({dpin.port}), "
                       f"which is AD{core}AN{pinsel} itself", "")
    dsite = channel_site_of_pin(board_key, dpin)
    asite = channel_site_of_pin(board_key, apin)
    return (True, f"wire {dsite} -> {asite}",
            f"DAC{unit} out on pin {dpin.n} ({dpin.port}), ADC in on pin {apin.n} ({apin.port})"
            "  -  or pick channel AN7 and take the internal UREF line instead, no wire")


def channel_site_of_pin(board_key, pin):
    """Where a device pin comes out on the board, as one short phrase."""
    board = BOARDS[board_key]
    for name, socket in board.get("connectors", {}).items():
        for socket_pin, entry in sorted(socket.items()):
            if entry["dev"] == pin.n:
                sig = f" {entry['sig']}" if entry["sig"] else ""
                return f"{name} pin {socket_pin}{sig}"
    for side, row in board.get("edge_rows", {}).items():
        for pad in row:
            if pad["dev"] == pin.n:
                return f"{side} row pad {pad['pos']} ({pad['label']})"
    if pin.n in board.get("onboard", {}):
        return board["onboard"][pin.n]
    return f"pin {pin.n} ({pin.port}), not brought out"


def channel_pin_info(board_key, core, pinsel):
    kind, label, detail = channel_site(board_key, core, pinsel)
    note = BOARD_NOTES.get((board_key, core, pinsel))
    text = label if not detail else f"{detail} → {label}"
    return text + (f" — {note}" if note else "")


# ---------------------------------------------------------------------------
# The package drawing
#
# The chip as the data sheet's pin diagram shows it: pin 1 at the marked
# corner, numbering counterclockwise. The selected channel's pin is lit and
# called out by number and port name, the other inputs of the same core are
# dimmed cyan, so the drawing answers "where do I connect the signal" at a
# glance. `full=True` labels every pin.
# ---------------------------------------------------------------------------
CHIP_VIEW, CHIP_BODY, CHIP_PINLEN, CHIP_PINW = 1000.0, 640.0, 26.0, 9.0
CHIP_OFF = (CHIP_VIEW - CHIP_BODY) / 2.0

# Mirrors the GUI palette in main_gui (ACCENT / ACCENT2 / DIM).
CHIP_COL = {
    "body": "#0f172a", "edge": "#1f2937", "body_text": "#64748b",
    "io": "#475569", "nc": "#233045", "gnd": "#334155", "pwr": "#9f4b4b",
    "core": "#0e7490", "sel": "#22d3ee", "dac": "#a78bfa",
    "label": "#94a3b8", "label_sel": "#e2e8f0", "board": "#16202f",
}


def _pin_geometry(n, per_side):
    """(x, y, w, h) of the pin's rectangle, (label x, y, rotation, anchor),
    and the point on the body edge a callout should point at."""
    pitch = CHIP_BODY / (per_side + 1)
    i, side = (n - 1) % per_side, (n - 1) // per_side
    span = (per_side - 1) * pitch
    along = CHIP_OFF + (CHIP_BODY - span) / 2.0 + i * pitch
    far = CHIP_OFF + CHIP_BODY
    back = far - (along - CHIP_OFF)          # sides 2 and 3 run backwards
    w = min(CHIP_PINW, pitch * 0.55)
    if side == 0:                            # left side, downwards
        return ((CHIP_OFF - CHIP_PINLEN, along - w / 2, CHIP_PINLEN, w),
                (CHIP_OFF - CHIP_PINLEN - 6, along, 0, "end"), (CHIP_OFF, along))
    if side == 1:                            # bottom, to the right
        return ((along - w / 2, far, w, CHIP_PINLEN),
                (along, far + CHIP_PINLEN + 6, 90, "start"), (along, far))
    if side == 2:                            # right side, upwards
        return ((far, back - w / 2, CHIP_PINLEN, w),
                (far + CHIP_PINLEN + 6, back, 0, "start"), (far, back))
    return ((back - w / 2, CHIP_OFF - CHIP_PINLEN, w, CHIP_PINLEN),
            (back, CHIP_OFF - CHIP_PINLEN - 6, -90, "start"), (back, CHIP_OFF))


def _pin_colour(pin, core, selected_n):
    if pin.n == selected_n:
        return CHIP_COL["sel"]
    if any(ADC_IN_RE.fullmatch(f) and int(f[2]) == core for f in pin.adc):
        return CHIP_COL["core"]
    if pin.dac:
        return CHIP_COL["dac"]
    return CHIP_COL.get(pin.kind, CHIP_COL["io"])


def _svg_open(view_w, view_h, label):
    return (f'<svg viewBox="0 0 {view_w:.0f} {view_h:.0f}" role="img" aria-label="{label}" '
            'style="width:100%;height:auto;display:block;'
            'font-family:ui-monospace,Consolas,monospace">')


def _text(x, y, s, fill, size, anchor="middle", weight=False, rot=None, baseline="middle"):
    w = ' font-weight="600"' if weight else ''
    r = f' transform="rotate({rot} {x:.1f} {y:.1f})"' if rot else ''
    return (f'<text x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}" dominant-baseline="{baseline}" '
            f'fill="{fill}" font-size="{size}"{w}{r}>{s}</text>')


def chip_svg(board_key, core, pinsel, full=False, dac_unit=0, dac_on=False):
    pkg = package_of(board_key)
    board = BOARDS[board_key]
    sel_pin = channel_pin(board_key, core, pinsel)
    sel_n = sel_pin.n if sel_pin else None
    dpin = dac_pin(board_key, dac_unit)
    dac_n = dpin.n if dpin else None
    out = [_svg_open(CHIP_VIEW, CHIP_VIEW,
                     f"{board['package']} pinout, AD{core}AN{pinsel} highlighted")]
    out.append(f'<rect x="{CHIP_OFF}" y="{CHIP_OFF}" width="{CHIP_BODY}" height="{CHIP_BODY}" '
               f'rx="10" fill="{CHIP_COL["body"]}" stroke="{CHIP_COL["edge"]}" stroke-width="2"/>')
    out.append(f'<circle cx="{CHIP_OFF + 34}" cy="{CHIP_OFF + 34}" r="10" fill="none" '
               f'stroke="{CHIP_COL["body_text"]}" stroke-width="2"/>')
    for pin in pkg.pins:
        (x, y, w, h), (lx, ly, rot, anchor), _ = _pin_geometry(pin.n, pkg.per_side)
        colour = _pin_colour(pin, core, sel_n)
        extra = f' stroke="{CHIP_COL["sel"]}" stroke-width="3"' if pin.n == sel_n else ''
        if pin.n == dac_n and pin.n != sel_n:
            colour = CHIP_COL["dac"]
            extra = f' stroke="{CHIP_COL["dac"]}" stroke-width="3"'
        out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="1.5" '
                   f'fill="{colour}"{extra}><title>pin {pin.n}: {"/".join(pin.functions)}</title></rect>')
        if not (full or pin.n in (sel_n, dac_n) or (pin.n - 1) % pkg.per_side == 0):
            continue
        is_sel = pin.n == sel_n
        is_dac = pin.n == dac_n and not is_sel
        size = 13 if (is_sel or is_dac) else (9.5 if pkg.count > 64 else 12)
        fill = CHIP_COL["label_sel"] if is_sel else (CHIP_COL["dac"] if is_dac else CHIP_COL["label"])
        out.append(_text(lx, ly, f'{pin.n} {pin.port or pin.functions[0]}',
                         fill, size, anchor, is_sel or is_dac, rot))
    out.append(_text(500, 452, board["device"], CHIP_COL["body_text"], 24, weight=True))
    out.append(_text(500, 480, f'{board["package"]} · 5 × 12-bit ADC', CHIP_COL["body_text"], 15))
    if sel_pin is not None:
        (_, _, _, _), _, (ex, ey) = _pin_geometry(sel_pin.n, pkg.per_side)
        cy = 300.0 if ey < CHIP_VIEW / 2 else 640.0
        out.append(f'<line x1="{ex:.1f}" y1="{ey:.1f}" x2="500" y2="{cy:.0f}" '
                   f'stroke="{CHIP_COL["sel"]}" stroke-width="2"/>')
        out.append(f'<rect x="270" y="{cy - 24:.0f}" width="460" height="52" rx="6" '
                   f'fill="{CHIP_COL["body"]}" stroke="{CHIP_COL["sel"]}" stroke-width="2"/>')
        out.append(_text(500, cy, f'AD{core}AN{pinsel} = pin {sel_pin.n} · {sel_pin.port}',
                         CHIP_COL["sel"], 19, weight=True))
        out.append(_text(500, cy + 19, "/".join(sel_pin.functions), CHIP_COL["label"], 11))
    else:
        why = INTERNAL_INPUTS.get(pinsel, "not brought out in this package")
        out.append(_text(500, 570, f'AD{core}AN{pinsel}: {why}', CHIP_COL["label"], 16))
    if dpin is not None:
        (_, _, _, _), _, (dx, dy) = _pin_geometry(dpin.n, pkg.per_side)
        needed, head, _ = wire_hint(board_key, core, pinsel, dac_unit)
        if needed and sel_pin is not None:
            # the wire, drawn as the dashed link it would be on the bench
            (_, _, _, _), _, (ax, ay) = _pin_geometry(sel_pin.n, pkg.per_side)
            out.append(f'<line x1="{dx:.1f}" y1="{dy:.1f}" x2="{ax:.1f}" y2="{ay:.1f}" '
                       f'stroke="{CHIP_COL["dac"]}" stroke-width="2" stroke-dasharray="7 5"/>')
        cy = 360.0 if dy < CHIP_VIEW / 2 else 700.0
        out.append(f'<line x1="{dx:.1f}" y1="{dy:.1f}" x2="500" y2="{cy:.0f}" '
                   f'stroke="{CHIP_COL["dac"]}" stroke-width="1.5" stroke-dasharray="4 4"/>')
        out.append(f'<rect x="300" y="{cy - 17:.0f}" width="400" height="34" rx="6" '
                   f'fill="{CHIP_COL["body"]}" stroke="{CHIP_COL["dac"]}" stroke-width="2"/>')
        out.append(_text(500, cy, f'DAC{dac_unit} ({"on" if dac_on else "off"}) = pin {dpin.n} · {dpin.port}',
                         CHIP_COL["dac"], 15, weight=True))
    out.append('</svg>')
    return "".join(out)


# ---------------------------------------------------------------------------
# The board drawing
#
# Not a photograph: the connectors a signal can actually be reached on,
# drawn with their real pin order, plus the on-board things an ADC pin can
# be tied to (potentiometer, touch pads, buttons). The pad carrying the
# selected channel is lit; if the channel only reaches an on-board part,
# that part is lit instead, which is the answer "you cannot wire to it".
# ---------------------------------------------------------------------------
BOARD_VIEW_W, BOARD_VIEW_H = 1000.0, 640.0


def _pad(out, x, y, w, h, fill, stroke=None, title=None, rx=2):
    s = f' stroke="{stroke}" stroke-width="3"' if stroke else ''
    t = f'<title>{title}</title>' if title else ''
    out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" '
               f'fill="{fill}"{s}>{t}</rect>')


dac_hit = []


def _draw_socket(out, name, socket, x0, y0, cols, rows, sel_dev, pkg, vertical, dac_dev=None):
    """One connector: `cols` x `rows` pads in its own pin order. mikroBUS
    counts 1..8 down the left column and 16..9 down the right one; the
    XPRO header counts odd numbers along one row and even along the other."""
    pw, ph, gap = (48, 20, 4) if not vertical else (66, 19, 5)
    out.append(_text(x0, y0 - 12, name, CHIP_COL["label"], 12, anchor="start", weight=True))
    hit = None
    for idx in range(cols * rows):
        if vertical:                       # mikroBUS: two columns of 8
            c, r = divmod(idx, rows)
            pin = r + 1 if c == 0 else 16 - r
        else:                              # XPRO: two rows of 10
            r, c = divmod(idx, cols)
            pin = 2 * c + 1 + r
        x = x0 + c * (pw + gap)
        y = y0 + r * (ph + gap)
        entry = socket.get(pin)
        dev = entry["dev"] if entry else None
        port = pkg.by_number(dev).port if dev else None
        is_sel = dev is not None and dev == sel_dev
        is_dac = dev is not None and dev == dac_dev and not is_sel
        if is_sel:
            hit = (x + pw / 2, y + ph / 2, pin)
        if is_dac:
            dac_hit.append((x + pw / 2, y + ph / 2))
        fill = (CHIP_COL["sel"] if is_sel else CHIP_COL["dac"] if is_dac
                else (CHIP_COL["io"] if dev else CHIP_COL["nc"]))
        _pad(out, x, y, pw, ph, fill,
             CHIP_COL["sel"] if is_sel else (CHIP_COL["dac"] if is_dac else None),
             f'{name} pin {pin}' + (f' = {port} (device pin {dev})' if dev else ' - power or not on the device'))
        label = f'{pin} {port}' if port else str(pin)
        out.append(_text(x + pw / 2, y + ph / 2, label,
                         CHIP_COL["body"] if (is_sel or is_dac) else CHIP_COL["label"],
                         9.5 if port else 9, weight=is_sel or is_dac))
    return hit


def _draw_edge_row(out, side, row, x0, y0, pw, sel_dev, pkg, dac_dev=None):
    gap = 2.0
    hit = None
    for pad in row:
        x = x0 + (pad["pos"] - 1) * (pw + gap)
        dev = pad["dev"]
        is_sel = dev is not None and dev == sel_dev
        is_dac = dev is not None and dev == dac_dev and not is_sel
        if is_sel:
            hit = (x + pw / 2, y0 + 11, pad["pos"])
        if is_dac:
            dac_hit.append((x + pw / 2, y0 + 11))
        fill = (CHIP_COL["sel"] if is_sel else CHIP_COL["dac"] if is_dac
                else (CHIP_COL["io"] if dev else CHIP_COL["nc"]))
        _pad(out, x, y0, pw, 22, fill,
             CHIP_COL["sel"] if is_sel else (CHIP_COL["dac"] if is_dac else None),
             f'{side} row pad {pad["pos"]}: {pad["label"]}'
             + (f' (device pin {dev})' if dev else ''))
        out.append(_text(x + pw / 2, y0 + 11, pad["label"].replace("CDC ", ""),
                         CHIP_COL["body"] if (is_sel or is_dac) else CHIP_COL["label"],
                         7.5, weight=is_sel or is_dac, rot=-90))
    return hit


def board_svg(board_key, core, pinsel, dac_unit=0, dac_on=False):
    board = BOARDS[board_key]
    pkg = package_of(board_key)
    pin = channel_pin(board_key, core, pinsel)
    sel_dev = pin.n if pin else None
    dpin = dac_pin(board_key, dac_unit)
    dac_dev = dpin.n if dpin else None
    kind, label, detail = channel_site(board_key, core, pinsel)
    out = [_svg_open(BOARD_VIEW_W, BOARD_VIEW_H, f"{board_key} board, AD{core}AN{pinsel} highlighted")]
    out.append(f'<rect x="8" y="8" width="{BOARD_VIEW_W - 16}" height="{BOARD_VIEW_H - 16}" rx="14" '
               f'fill="{CHIP_COL["board"]}" stroke="{CHIP_COL["edge"]}" stroke-width="2"/>')
    out.append(_text(28, 36, board["title"], CHIP_COL["body_text"], 17, anchor="start", weight=True))
    hit = None
    del dac_hit[:]
    if board_key == "EV74H48A":
        conn = board["connectors"]
        hit = _draw_socket(out, "mikroBUS A", conn["mikroBUS A"], 40, 90, 2, 8, sel_dev, pkg, True, dac_dev) or hit
        hit = _draw_socket(out, "mikroBUS B", conn["mikroBUS B"], 210, 90, 2, 8, sel_dev, pkg, True, dac_dev) or hit
        hit = _draw_socket(out, "XPRO1", conn["XPRO1"], 420, 90, 10, 2, sel_dev, pkg, False, dac_dev) or hit
        hit = _draw_socket(out, "XPRO2", conn["XPRO2"], 420, 190, 10, 2, sel_dev, pkg, False, dac_dev) or hit
        # on-board parts an ADC pin can be tied to
        out.append(_text(40, 330, "on board", CHIP_COL["label"], 12, anchor="start", weight=True))
        items = sorted(board["onboard"].items(), key=lambda kv: kv[1])
        x, y = 40, 344
        for dev, what in items:
            p = pkg.by_number(dev)
            if p is None or not p.adc:
                continue                    # only the ones an ADC can read
            w = 168
            is_sel = dev == sel_dev
            is_dac = dev == dac_dev and not is_sel
            if is_sel:
                hit = (x + w / 2, y + 13, None)
            if is_dac:
                dac_hit.append((x + w / 2, y + 13))
            _pad(out, x, y, w, 26,
                 CHIP_COL["sel"] if is_sel else (CHIP_COL["dac"] if is_dac else CHIP_COL["io"]),
                 CHIP_COL["sel"] if is_sel else (CHIP_COL["dac"] if is_dac else None),
                 f'{what} = {p.port} (device pin {dev})', rx=6)
            out.append(_text(x + w / 2, y + 13, f'{what} · {p.port}',
                             CHIP_COL["body"] if (is_sel or is_dac) else CHIP_COL["label"],
                             9.5, weight=is_sel or is_dac))
            x += w + 8
            if x + w > BOARD_VIEW_W - 40:
                x, y = 40, y + 34
    else:
        rows = board["edge_rows"]
        pw = (BOARD_VIEW_W - 200) / 28 - 2
        # the PCB itself, with the two castellated rows on its long edges
        _pad(out, 60, 96, BOARD_VIEW_W - 130, 268, CHIP_COL["body"], None, None, rx=10)
        _pad(out, 60, 120, 52, 132, CHIP_COL["edge"], None, "USB Type-C, on-board debugger", rx=6)
        out.append(_text(86, 186, "USB", CHIP_COL["body_text"], 11))
        out.append(_text(126, 112, "left edge row", CHIP_COL["label"], 11, anchor="start", weight=True))
        hit = _draw_edge_row(out, "left", rows["left"], 126, 122, pw, sel_dev, pkg, dac_dev) or hit
        out.append(_text(126, 300, "right edge row", CHIP_COL["label"], 11, anchor="start", weight=True))
        hit = _draw_edge_row(out, "right", rows["right"], 126, 310, pw, sel_dev, pkg, dac_dev) or hit
        # what sits between the two rows
        _pad(out, 430, 196, 170, 62, CHIP_COL["edge"], None, board["device"], rx=6)
        out.append(_text(515, 219, board["device"][:9], CHIP_COL["body_text"], 12))
        out.append(_text(515, 236, board["device"][9:], CHIP_COL["body_text"], 12))
        for dev, what in sorted(board.get("onboard", {}).items()):
            pin_o = pkg.by_number(dev)
            x = 640 if "LED" in what else 760
            is_sel = dev == sel_dev
            _pad(out, x, 206, 108, 24, CHIP_COL["sel"] if is_sel else CHIP_COL["edge"],
                 CHIP_COL["sel"] if is_sel else None,
                 f'{what} = {pin_o.port if pin_o else "?"} (device pin {dev})', rx=6)
            out.append(_text(x + 54, 218, what.split(" (")[0],
                             CHIP_COL["body"] if is_sel else CHIP_COL["body_text"], 9))
        out.append(_text(126, 344, "pad 1 is the USB end, and these names are on the silkscreen",
                         CHIP_COL["body_text"], 11, anchor="start"))
    # the verdict, and a leader to the lit pad
    cy = BOARD_VIEW_H - 74
    if hit is not None:
        out.append(f'<line x1="{hit[0]:.1f}" y1="{hit[1]:.1f}" x2="500" y2="{cy:.0f}" '
                   f'stroke="{CHIP_COL["sel"]}" stroke-width="2"/>')
    box_fill = CHIP_COL["board"]
    out.append(f'<rect x="150" y="{cy - 24:.0f}" width="700" height="52" rx="6" fill="{box_fill}" '
               f'stroke="{CHIP_COL["sel"] if hit else CHIP_COL["edge"]}" stroke-width="2"/>')
    out.append(_text(500, cy, f'AD{core}AN{pinsel} → {label}',
                     CHIP_COL["sel"] if hit else CHIP_COL["label"], 17, weight=True))
    out.append(_text(500, cy + 19, detail or ("nothing to wire to" if kind != "connector" else ""),
                     CHIP_COL["label"], 11))
    if dpin is not None:
        needed, head, sub = wire_hint(board_key, core, pinsel, dac_unit)
        if needed and dac_hit and hit is not None:
            out.append(f'<line x1="{dac_hit[0][0]:.1f}" y1="{dac_hit[0][1]:.1f}" '
                       f'x2="{hit[0]:.1f}" y2="{hit[1]:.1f}" stroke="{CHIP_COL["dac"]}" '
                       'stroke-width="2.5" stroke-dasharray="8 5"/>')
        out.append(f'<rect x="150" y="{cy + 34:.0f}" width="700" height="38" rx="6" '
                   f'fill="{CHIP_COL["board"]}" stroke="{CHIP_COL["dac"]}" stroke-width="2"/>')
        out.append(_text(500, cy + 48, f'DAC{dac_unit} ({"on" if dac_on else "off"}): {head}',
                         CHIP_COL["dac"], 14, weight=True))
        if sub:
            out.append(_text(500, cy + 63, sub, CHIP_COL["label"], 10))
    out.append('</svg>')
    return "".join(out)


# ---------------------------------------------------------------------------
# DAC2 triangle wave (dac.c): DACOUT2 = RA8 = AD5AN3. CLKGEN7 (its clock) is
# PLL1 undivided, always 320 MHz, no divider register exists (clock.c:
# clock_dac_hz() returns ADC_CLK_HZ outright) -- unlike CLKGEN6, so no
# separate GUI control is needed for it.
# ---------------------------------------------------------------------------
DAC_CLK_HZ = 400e6   # CLKGEN7 on the PLL1 VCO divider (clock.c, 25.09.2026; was 320e6, below the DAC's spec)


def dac_period_ns_of(low: int, high: int, slp: int) -> float:
    """Mirrors dac.c's dac_period_ns(): two slopes of (high-low)*16 DAC
    clocks each, so a full triangle period is (high-low)*32 DAC clocks."""
    if slp <= 0 or high <= low:
        return 0.0
    clocks = (high - low) * 32
    return clocks * 1e9 / (DAC_CLK_HZ * slp)


# ---------------------------------------------------------------------------
# The chain stream's triangle (chaintest.c: triangle_for(), dac.h): the
# range is not part of the "stream grab" frame (only slpdat and the DAC
# clock are, docs/PLAN-BINARY-TRANSFER.md), because it is not a free
# choice -- triangle_for() always picks the widest range these two fixed
# limits (dac.h) and slpdat allow, with 32 codes of margin inside that.
# Reproducing the same arithmetic here, from slpdat alone, gives the exact
# model slope length the firmware's own tri_eval() call is judged against
# (chaintest.c stage5's "ratio_x1000"), without having to also transmit
# low/high.
# ---------------------------------------------------------------------------
CHAIN_DAC_CODE_MIN = 0x0CD
CHAIN_DAC_CODE_MAX = 0xF32


def chain_triangle_range(slp: int):
    return CHAIN_DAC_CODE_MIN + slp + 32, CHAIN_DAC_CODE_MAX - slp - 32


def chain_model_slope_samples(slp: int, dac_hz: float, ksps: float) -> float:
    """Mirrors dac_slope_samples_x1000()/1000 (dac.c) for the chain
    triangle's range: one slope's length in samples, at the rate and DAC
    clock a 'stream grab' frame reports."""
    if slp <= 0 or dac_hz <= 0 or ksps <= 0:
        return 0.0
    low, high = chain_triangle_range(slp)
    if high <= low:
        return 0.0
    return (high - low) * 32.0 * (ksps * 1e3) / (slp * dac_hz)


# ---------------------------------------------------------------------------
# CRC-16/CCITT-FALSE, shared by every binary frame the firmware sends
# (docs/PLAN-BINARY-TRANSFER.md): poly 0x1021, init 0xFFFF, no reflect, no
# xorout. Only "stream grab"'s GRAB frame uses it in this tool now - the
# back-to-back "blk" block transfer is retired here (the firmware command
# stays, for a terminal).
# ---------------------------------------------------------------------------
_CRC_LINE_RE = re.compile(rb"CRC ([0-9A-Fa-f]{4})")


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
    r"halves=(\d+) xfer=(\d+) slp=(\d+) dachz=(\d+)")


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
                transfers=int(m.group(8)), slpdat=int(m.group(9)), dac_hz=int(m.group(10)))
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


def probe_grab(target) -> bool:
    """Does this target's 'stream' understand the 'grab' sub-command? Ask
    'help' once rather than trying 'stream grab' itself and guessing at a
    NAK's cause."""
    try:
        ok, lines = target.cmd("help")
    except Exception:
        return False
    return ok and any(("stream" in l) and ("grab" in l) for l in lines)


def _parse_buf(lines) -> int:
    """'buf: <n>' (plus a 'half: <n/2>' line) -> n, or None if not found."""
    for l in lines:
        m = re.match(r"\s*buf:\s*(\d+)", l)
        if m:
            return int(m.group(1))
    return None


def query_buf(target) -> int:
    """Ask 'buf' (no argument) for the total, currently configured ping-pong
    buffer size. Falls back to the legacy fixed 2048 (1024-sample halves) for
    a board or firmware build that does not have the 'buf' command yet."""
    try:
        ok, lines = target.cmd("buf")
    except Exception:
        ok, lines = False, []
    n = _parse_buf(lines) if ok else None
    return n if n is not None else 2048


# ---------------------------------------------------------------------------
# Transport: the board's console over a COM port
# ---------------------------------------------------------------------------
class Target:
    """One command at a time, synchronised on the parser's ACK/NAK byte."""

    def __init__(self, port: str, baud: int = BAUD, on_log=None):
        import serial  # pyserial
        self.on_log = on_log  # optional callable(str): the console transcript
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.port = port
        self.sync()

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


class FakeTarget:
    """Answers like cli.c would, for the triggered chain only - 'stream
    on/off/grab', 'dac', 'buf', 'version', 'help'. The back-to-back
    commands this tool no longer sends ('pll', 'samc', 'core', 'input',
    'clk', 'status', 'snap', 'rate', 'dump', 'blk') are not answered here
    either. Used with --fake and --selftest.

    'stream on <ksps>' (the test form): core 5, PINSEL 3 (RA8), the DAC2
    test triangle as the signal - grab frames carry the SAME triangle
    tools/eval_chain.py's synth() and the firmware's own chain test use, so
    a PASS/FAIL verdict here means the same thing it means in a 'chain all'
    log.
    'stream on <ksps> <core> <pinsel> [<samc>]' (the custom form): that
    input, the DAC left alone (the 'dac' command drives it if wanted) -
    the stand-in plays a sine with harmonics and noise instead, at the
    configured level, so SNR/THD/harmonics have something to show; grab
    frames carry slp=0, exactly as chain_stream_on_input() reports for a
    non-test signal.
    """

    # What board.h's BOARD_NAME says for each profile - the fake answers
    # 'version' with it exactly as the firmware does ("[build] board: ..."),
    # so that the GUI's board detection is exercised without a board.
    FAKE_BOARD_NAMES = {
        "EV74H48A": "EV74H48A, dsPIC33AK512MPS512 GP DIM",
        "EV17P63A": "EV17P63A, dsPIC33AK512MPS506 Curiosity Nano",
    }

    def __init__(self, signal_khz: float = 100.0, amplitude: float = 1500.0,
                 noise_std: float = 6.0, harm2_amp: float = 150.0, harm3_amp: float = 0.0,
                 on_log=None, board: str = "EV74H48A"):
        self.on_log = on_log  # optional callable(str): the console transcript
        self.board = board if board in self.FAKE_BOARD_NAMES else "EV74H48A"
        self.port = "fake"
        # Both DACs, as dac.c has them - only ever touched by the 'dac'
        # command, never by 'stream on' itself (the test form's own
        # internal DAC2 triangle is modelled separately below, independent
        # of this dict, exactly as chain_stream_on()'s dac2_level_start()
        # is independent of the 'dac' command in the firmware).
        self.dac = {1: {"on": False, "low": 0x100, "high": 0xF00, "slp": 8},
                    2: {"on": False, "low": 0x100, "high": 0xF00, "slp": 8}}
        self.buf_size = 2048                      # total ping-pong buffer, 'buf'
        self.signal_khz = signal_khz               # custom-input sine, kHz
        self.amplitude = amplitude                 # fundamental peak, ADC counts
        self.noise_std = noise_std                 # noise, ADC counts (sets the SNR)
        self.harm2_amp = harm2_amp                 # 2nd harmonic peak, ADC counts
        self.harm3_amp = harm3_amp                 # 3rd harmonic peak, ADC counts
        self.t0 = None                             # wall-clock anchor, lazy
        self.rng = np.random.default_rng(1)
        # The chain stream ("stream on/off/grab", chaintest.c).
        self.chain_on = False
        self.chain_ksps = 0
        self.chain_core, self.chain_pinsel, self.chain_samc = 5, 3, 0
        self.chain_test = True                     # DAC2 triangle (True) or the configured input (False)
        self.chain_ready_half = 0                  # alternates like ready_half in capture.c
        self.chain_grabs = 0
        self.grab_fault = None                     # None, "drop", "dup", "overrun", "missed"

    def _chain_slpdat(self) -> int:
        """triangle_for()'s SLOPE_TARGET=128-samples-per-slope search, for
        the rate the stand-in's chain is "on" at -- same formula as
        chain_model_slope_samples(), solved the other way around."""
        rate = self.chain_ksps * 1000
        f = DAC_CLK_HZ
        full = (CHAIN_DAC_CODE_MAX - CHAIN_DAC_CODE_MIN) - 64
        s = 1
        while (2 * s + 64) < full:
            span = full - 2 * s
            samples = span * 32 * rate / (s * f) if (s * f) else 0.0
            if samples <= 128:
                break
            s += 1
        return s

    def close(self):
        pass

    def _log(self, line: str):
        if self.on_log:
            try:
                self.on_log(line)
            except Exception:
                pass

    def _actual_ksps(self, want: int) -> int:
        """Mirrors chaintest.c's period_for()/ksps_of(): the nearest
        160 MHz / N (CLKGEN13, g_trig_hz), not a free number - and the
        SAME value both 'stream'/'stream on' and every 'stream grab'
        report from then on."""
        trig_hz = 160_000_000
        n = max(4, round(trig_hz / 1000.0 / max(1, want)))
        return round(trig_hz / 1000.0 / n)

    def _custom_wave_samples(self, n: int) -> np.ndarray:
        """A window of the configured sine (plus harmonics and noise) at
        the chain's actual rate, as of *now* - not simply the next n
        samples after the previous call: two grabs close together in
        wall-clock time overlap almost completely, the same idea the old
        back-to-back stand-in modelled for a continuously running signal."""
        fs = self.chain_ksps * 1e3
        if self.t0 is None:
            self.t0 = time.time()
        end_idx = (time.time() - self.t0) * fs
        start_idx = max(end_idx - n, 0.0)
        t = (start_idx + np.arange(n)) / fs
        f = self.signal_khz * 1e3
        v = (2048 + self.amplitude * np.sin(2 * np.pi * f * t)
             + self.harm2_amp * np.sin(2 * np.pi * 2 * f * t + 0.7)
             + self.harm3_amp * np.sin(2 * np.pi * 3 * f * t + 1.3))
        v += self.rng.normal(0, self.noise_std, n)
        return np.clip(np.round(v), 0, 4095).astype(int)

    # Where each DAC's output reaches an ADC input with no wire: DACOUT1 =
    # RA1 = AD5AN1, DACOUT2 = RA8 = AD5AN3 (board.h, both boards).
    DAC_ON_INPUT = {(5, 1): 1, (5, 3): 2}

    def _custom_input_samples(self, n: int) -> np.ndarray:
        """What a custom input reads: on a DAC pin, that DAC - its triangle
        from the DAC tile's low/high/SLPDAT when it is on, a pin at the
        bottom of the range with a little noise when it is off; on any other
        pin the configured sine (the generator on the wire)."""
        unit = self.DAC_ON_INPUT.get((self.chain_core, self.chain_pinsel))
        if unit is None:
            return self._custom_wave_samples(n)
        d = self.dac[unit]
        if not d["on"]:
            v = 20 + self.rng.normal(0, self.noise_std, n)
            return np.clip(np.round(v), 0, 4095).astype(int)
        low, high, slp = d["low"], d["high"], max(1, d["slp"])
        fs = self.chain_ksps * 1e3
        # one slope = (high - low) * 32 / (SLPDAT * F_DAC), Equation 18-4
        slope_s = (high - low) * 32.0 / (slp * DAC_CLK_HZ)
        slope_n = max(slope_s * fs, 1.0)
        if self.t0 is None:
            self.t0 = time.time()
        start = ((time.time() - self.t0) * fs) % (2 * slope_n)
        ph = np.mod(start + np.arange(n), 2 * slope_n)
        v = np.where(ph < slope_n, low + (high - low) * ph / slope_n,
                     high - (high - low) * (ph - slope_n) / slope_n)
        v = v + self.rng.normal(0, self.noise_std, n)
        return np.clip(np.round(v), 0, 4095).astype(int)

    def _board_limit_counters(self):
        """A simplified model of the board's own limits (the last hardware
        run, HARDWARE-LOG.md): mild overruns from about 10 MSPS, missed
        halves from about 16 MSPS. Guidance only, exactly like the UI text
        under the rate field - not a claim about the exact thresholds, and
        not exercised by anything but this stand-in."""
        ov = 1 if self.chain_ksps > 10000 else 0
        missed = 2 if self.chain_ksps > 16000 else 0
        return ov, missed

    def cmd(self, line: str, timeout: float = 5.0):
        """Logs the traffic like Target.cmd() does, then answers like cli.c
        would (_cmd_impl)."""
        self._log(f"> {line}")
        ok, out = self._cmd_impl(line, timeout)
        for l in out:
            self._log(f"< {l}")
        self._log(f"< {'[ACK]' if ok else '[NAK]'}")
        return ok, out

    def _cmd_impl(self, line: str, timeout: float = 5.0):
        parts = line.split()
        if not parts:
            return True, []
        c, args = parts[0], parts[1:]
        usage_stream = ["usage: stream on <ksps 1..40000> [<core 1..5> <pinsel 0..15> "
                        "[<samc 0..31>]] | stream off | stream grab | stream"]
        try:
            if c == "dac":
                usage = ["usage: dac <1|2> <on|off> [low] [high] [slpdat]"]
                if len(args) < 2 or args[0] not in ("1", "2"):
                    return False, usage
                unit = int(args[0])
                d = self.dac[unit]
                if args[1].startswith("off"):
                    d["on"] = False
                    return True, [f"dac: {unit}", "off"]
                if not args[1].startswith("on"):
                    return False, usage
                low = int(args[2], 0) if len(args) > 2 else 0x100
                high = int(args[3], 0) if len(args) > 3 else 0xF00
                slp = int(args[4], 0) if len(args) > 4 else 8
                if not (0 <= low <= 4095) or not (0 <= high <= 4095) or high <= low or not (1 <= slp <= 255):
                    return False, usage
                d.update(on=True, low=low, high=high, slp=slp)
                return True, [f"dac: {unit}", "RA1" if unit == 1 else "RA8",
                              f"low: {low}", f"high: {high}", f"slpdat: {slp}",
                              f"period ns: {round(dac_period_ns_of(low, high, slp))}"]
            if c == "buf":
                if args:
                    n = int(args[0])
                    if n < 16 or n > 8192 or n % 2:
                        return False, ["usage: buf <n, even, 16..8192> - total ping-pong buffer"]
                    self.buf_size = n
                return True, [f"buf: {self.buf_size}", f"half: {self.buf_size // 2}"]
            if c == "version":
                return True, ["[build] adc_dma_40msps (fake target, synthetic signal)",
                              "[build] board: " + self.FAKE_BOARD_NAMES[self.board]]
            if c == "help":
                return True, [
                    "commands: dac buf version stream",
                    "stream on <ksps> [core pinsel [samc]] | off | grab - the chain streaming",
                ]
            if c == "stream":
                if args and args[0] == "on":
                    rest = args[1:]
                    if len(rest) not in (1, 3, 4) or not rest[0].isdigit():
                        return False, usage_stream
                    ksps = int(rest[0])
                    if not (1 <= ksps <= 40000):
                        return False, usage_stream
                    if len(rest) == 1:
                        core, pinsel, samc, test = 5, 3, 0, True
                    else:
                        if not (rest[1].isdigit() and rest[2].isdigit()):
                            return False, usage_stream
                        core, pinsel = int(rest[1]), int(rest[2])
                        samc = int(rest[3]) if len(rest) == 4 and rest[3].isdigit() else 0
                        if not (1 <= core <= 5) or not (0 <= pinsel <= 15) or not (0 <= samc <= 31):
                            return False, usage_stream
                        test = False
                    self.chain_on = True
                    self.chain_ksps = self._actual_ksps(ksps)
                    self.chain_core, self.chain_pinsel, self.chain_samc = core, pinsel, samc
                    self.chain_test = test
                    self.chain_ready_half = 0
                    self.chain_grabs = 0
                    return True, [f"stream: on - {self.chain_ksps} ksps"
                                  + ("" if test else f"  core {core} pinsel {pinsel} samc {samc}")]
                if args and args[0] == "off":
                    self.chain_on = False
                    self.chain_core, self.chain_pinsel, self.chain_samc, self.chain_test = 5, 3, 0, True
                    return True, ["stream: off, boot configuration restored"]
                if args and args[0] == "grab":
                    return False, ["usage: use target.grab(), not cmd('stream grab') - binary framing"]
                if args:
                    return False, usage_stream
                if not self.chain_on:
                    return True, ["stream: off"]
                return True, [f"stream: on - {self.chain_ksps} ksps",
                              f"core: {self.chain_core}", f"pinsel: {self.chain_pinsel}",
                              f"grabs: {self.chain_grabs}"]
        except (ValueError, IndexError):
            return False, ["usage error"]
        return False, ["unknown command"]

    def grab(self, timeout: float = 10.0, corrupt_payload: bool = False):
        """Builds the identical frame bytes cmd_stream_grab() (cli.c) would
        send for one halt/transfer/restart cycle, then decodes them with
        parse_grab_frame() -- the same function Target.grab() uses for a
        real board. n=0 (NAK) if the stream is not on, mirroring
        chain_stream_grab_begin() returning false. The test form's window
        is eval_chain.synth()'s triangle, with grab_fault injecting exactly
        the faults tri_eval() is built to catch (see chaintest.c's own host
        test of the evaluator); the custom form's window is the configured
        sine, with slp=0 in the frame, exactly as chain_stream_on_input()
        reports for a non-test signal. Only the ASCII framing goes to the
        console transcript (on_log); the sample bytes themselves are not
        logged."""
        self._log("> stream grab")
        if not self.chain_on:
            header_line = "GRAB n=0 from=0 ksps=0 ov=0 late=0 missed=0 halves=0 xfer=0 slp=0 dachz=0\r\n"
            self._log(f"< {header_line.rstrip()}")
            crc = crc16_ccitt_false(b"")
            self._log(f"< CRC {crc:04X}")
            self._log("< [NAK]")
            tail = f"\r\nCRC {crc:04X}\r\n> ".encode("ascii") + NAK
            return parse_grab_frame(header_line, b"", tail)

        self.chain_grabs += 1
        n = self.buf_size // 2
        frm = self.chain_ready_half * n
        self.chain_ready_half ^= 1
        ov, missed = self._board_limit_counters()
        if self.chain_test:
            slp = self._chain_slpdat()
            # DAC_CLK_HZ both drives the synthetic samples and is reported
            # as "dachz", so the GUI's model-vs-measured ratio comes out
            # near 1.0 on a fault-free cycle -- the point of a stand-in,
            # not a claim about which PLL output the real chain's CLKGEN7
            # runs from (the frame carries the real board's actual
            # clock_dac_hz() there).
            slope_samples = max(1.0, chain_model_slope_samples(slp, DAC_CLK_HZ, self.chain_ksps))
            drop = n // 2 if self.grab_fault == "drop" else None
            dup = n // 2 if self.grab_fault == "dup" else None
            v = chain_synth(n, slope_samples,
                            phase=float(self.chain_grabs * 7 % int(2 * slope_samples) or 1),
                            drop=drop, dup=dup, seed=self.chain_grabs)
        else:
            slp = 0
            v = self._custom_input_samples(n)
        if self.grab_fault == "overrun":
            ov = max(ov, 3)
        if self.grab_fault == "missed":
            missed = max(missed, 2)
        header_line = (f"GRAB n={n} from={frm} ksps={self.chain_ksps} ov={ov} late=0 "
                        f"missed={missed} halves=2 xfer={2 * n} slp={slp} dachz={int(DAC_CLK_HZ)}\r\n")
        self._log(f"< {header_line.rstrip()}")
        payload = np.asarray(v, dtype="<u2").tobytes()
        self._log(f"< [binary payload, {len(payload)} bytes, not shown]")
        crc = crc16_ccitt_false(payload)
        if corrupt_payload and payload:
            payload = bytes([payload[0] ^ 0xFF]) + payload[1:]
        self._log(f"< CRC {crc:04X}")
        self._log("< [ACK]")
        tail = f"\r\nCRC {crc:04X}\r\n> ".encode("ascii") + ACK
        return parse_grab_frame(header_line, payload, tail)


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


def analyze_spectrum(f: np.ndarray, db: np.ndarray, n_harmonics: int = 5, exclude_bins: int = 2) -> dict:
    """Signal-quality read-out for the FFT panel: the fundamental (highest
    bin, DC excluded), each harmonic 2..n_harmonics with its own frequency
    and level, SNR (fundamental vs. the median of everything that is
    neither DC, the fundamental nor one of its harmonics) and THD (the
    harmonics' combined power against the fundamental's)."""
    n = len(db)
    if n < 8:
        return {}
    mask = np.ones(n, dtype=bool)
    mask[:exclude_bins] = False  # DC and its skirt
    fund_bin = int(np.argmax(np.where(mask, db, -np.inf)))
    fund_freq, fund_db = float(f[fund_bin]), float(db[fund_bin])

    def exclude_around(bin_idx):
        lo, hi = max(0, bin_idx - exclude_bins), min(n, bin_idx + exclude_bins + 1)
        mask[lo:hi] = False

    exclude_around(fund_bin)
    df = f[1] - f[0] if n > 1 else 1.0
    harmonics = []
    harmonic_power = 0.0
    for k in range(2, n_harmonics + 1):
        hbin = int(round(fund_freq * k / df))
        if hbin >= n:
            break
        h_freq, h_db = float(f[hbin]), float(db[hbin])
        harmonics.append(dict(k=k, freq=h_freq, db=h_db, rel_db=h_db - fund_db))
        harmonic_power += 10 ** (h_db / 10.0)
        exclude_around(hbin)
    noise_db = float(np.median(db[mask])) if mask.any() else float(db.min())
    fund_power = 10 ** (fund_db / 10.0)
    thd_pct = 100.0 * math.sqrt(harmonic_power / fund_power) if fund_power > 0 else float("nan")
    return dict(fund_freq=fund_freq, fund_db=fund_db, noise_db=noise_db,
                snr_db=fund_db - noise_db, thd_pct=thd_pct, harmonics=harmonics)


def selftest() -> int:
    ok_all = True
    t = FakeTarget(signal_khz=250.0, amplitude=1500.0)

    # ---- 'stream grab' before 'stream on': refused, exactly what
    # chain_stream_grab_begin() returning false produces on the board. ----
    assert probe_grab(t), "fake target's help must advertise 'stream' and 'grab'"
    ok, samples, meta = t.grab()
    ok_no_stream = (not ok) and len(samples) == 0 and "error" in meta
    ok_all &= ok_no_stream
    print("grab without 'stream on' refused:", "PASS" if ok_no_stream else "FAIL", "-", meta.get("error"))

    # ---- the test form: 'stream on <ksps>' - core 5, RA8, DAC2 triangle ----
    ok, lines = t.cmd("stream on 8000")
    assert ok, ("stream on", lines)
    ok, samples, meta = t.grab()
    r = chain_tri_eval([int(v) for v in samples])
    ok_grab = (ok and len(samples) == t.buf_size // 2 and meta["from_"] == 0
              and meta["slpdat"] > 0 and chain_grid_ok(r))
    ok_all &= ok_grab
    print(f"grab (test triangle): {len(samples)} samples, from {meta['from_']}, "
          f"slp {meta['slpdat']}, slip {r['slip']:.3f} -> {'PASS' if ok_grab else 'FAIL'}")

    # The frame's own 'ksps' is the actual rate (the nearest 160 MHz / N,
    # chaintest.c's ksps_of()) - the FFT must use it as fs, not the number
    # typed at 'stream on'.
    ok_fs = abs(meta["ksps"] - 8000) < 200
    ok_all &= ok_fs
    f, db = spectrum(np.asarray(samples), meta["ksps"] * 1e3)
    print(f"grab actual rate: {meta['ksps']} ksps (asked for 8000), used as FFT fs ->",
          "PASS" if ok_fs else "FAIL")

    # The buffer alternates halves like ready_half does in capture.c - the
    # second grab must land at the OTHER half (from > 0), not the same one.
    ok2, samples2, meta2 = t.grab()
    ok_from = ok2 and meta2["from_"] == t.buf_size // 2 and meta2["from_"] > 0
    ok_all &= ok_from
    print(f"grab #2 from={meta2['from_']} (> 0, the other half):", "PASS" if ok_from else "FAIL")

    # A lost/repeated sample must fail the SAME grid check the firmware's
    # own tri_eval() uses - the fake target injects exactly what
    # chaintest.c's host test of the evaluator injects.
    # One window holds only about eight slopes, and a fault in the first or
    # last one cannot be enclosed by four turning points (see tri_eval), so
    # a single grab is caught or missed depending on where the fault lands.
    # Judged over eight grabs at different phases: most faulty windows must
    # fail, and no clean one may (the host test of the evaluator: no false
    # alarm, 96-100 % found in 2048-sample windows).
    caught = 0
    for _ in range(8):
        t.grab_fault = "drop"
        ok3, samples3, meta3 = t.grab()
        caught += ok3 and not chain_grid_ok(chain_tri_eval([int(v) for v in samples3]))
    t.grab_fault = None
    false_alarms = 0
    for _ in range(8):
        ok3, samples3, meta3 = t.grab()
        false_alarms += ok3 and not chain_grid_ok(chain_tri_eval([int(v) for v in samples3]))
    ok_fault = caught >= 5 and false_alarms == 0
    ok_all &= ok_fault
    print(f"grab with a lost sample: caught in {caught} of 8 windows, false alarms in "
          f"{false_alarms} of 8 clean ones ->", "PASS" if ok_fault else "FAIL")

    # ---- a custom input on the DAC2 pin sees the DAC tile's settings ----
    ok, _ = t.cmd("stream on 8000 5 3 0")
    t.cmd("dac 2 on 256 1000 39")
    okd, sd, _m = t.grab()
    hi_on = int(np.max(sd)) if okd else 0
    t.cmd("dac 2 on 256 3000 39")
    okd2, sd2, _m = t.grab()
    hi_on2 = int(np.max(sd2)) if okd2 else 0
    t.cmd("dac 2 off")
    okd3, sd3, _m = t.grab()
    hi_off = int(np.max(sd3)) if okd3 else 9999
    ok_dac = abs(hi_on - 1000) < 60 and abs(hi_on2 - 3000) < 60 and hi_off < 120
    ok_all &= ok_dac
    print(f"custom input core 5 / AN3 follows DAC2: high 1000 -> max {hi_on}, high 3000 -> "
          f"max {hi_on2}, off -> max {hi_off}:", "PASS" if ok_dac else "FAIL")

    # ---- the custom form: 'stream on <ksps> <core> <pinsel> [<samc>]' ----
    ok, lines = t.cmd("stream on 5000 3 5 0")
    assert ok, ("stream on (custom input)", lines)
    ok, samples, meta = t.grab()
    ok_custom = ok and meta["slpdat"] == 0 and len(samples) == t.buf_size // 2
    ok_all &= ok_custom
    print(f"grab (custom input, core 3 pin 5): slp={meta['slpdat']} (expect 0) ->",
          "PASS" if ok_custom else "FAIL")
    f2, db2 = spectrum(np.asarray(samples), meta["ksps"] * 1e3)
    metrics = analyze_spectrum(f2, db2)
    ok_peak = bool(metrics) and abs(metrics["fund_freq"] - 250e3) < meta["ksps"] * 1e3 / len(samples)
    ok_all &= ok_peak
    print(f"grab (custom input) FFT peak {metrics.get('fund_freq', 0)/1e3:.1f} kHz (expect 250.0) ->",
          "PASS" if ok_peak else "FAIL")

    # A damaged frame must be reported, never silently accepted.
    ok4, _, meta4 = t.grab(corrupt_payload=True)
    ok_grab_corrupt = (not ok4) and "CRC mismatch" in meta4.get("error", "")
    ok_all &= ok_grab_corrupt
    print("grab corruption caught:", "PASS" if ok_grab_corrupt else "FAIL", "-", meta4.get("error"))

    # A truncated frame (fewer payload bytes than the header promises) is
    # exactly what a Ctrl+C or a disconnect mid-transfer looks like on the
    # wire (docs/PLAN-BINARY-TRANSFER.md's Ctrl+C risk) - parse_grab_frame()
    # is the function both Target.grab() and FakeTarget.grab() decode
    # through, so exercising it directly proves the check without needing
    # to fake a real interruption.
    header = "GRAB n=64 from=0 ksps=8000 ov=0 late=0 missed=0 halves=2 xfer=128 slp=8 dachz=320000000\r\n"
    short_payload = b"\x00\x00" * 30                      # 60 of the 128 bytes promised
    ok5, _, meta5 = parse_grab_frame(header, short_payload, b"\r\nCRC 0000\r\n> " + ACK)
    ok_truncated = (not ok5) and "short block" in meta5.get("error", "")
    ok_all &= ok_truncated
    print("grab truncated frame caught:", "PASS" if ok_truncated else "FAIL", "-", meta5.get("error"))

    # Target.grab() itself, against a serial stub that never delivers a
    # byte: the SAME bounded-wait code a real disconnected or wedged board
    # would hit, without opening a port. TimeoutError, not a hang.
    class _NullSerial:
        def read(self, n=1):
            return b""

        def write(self, data):
            return len(data)

        def reset_input_buffer(self):
            pass

    tt = Target.__new__(Target)   # bypass __init__: no real port is opened
    tt.ser = _NullSerial()
    tt.on_log = None
    tt.port = "null (selftest)"
    try:
        tt.grab(timeout=0.05)
        ok_timeout = False
    except TimeoutError:
        ok_timeout = True
    ok_all &= ok_timeout
    print("grab timeout (no bytes at all) caught:", "PASS" if ok_timeout else "FAIL")

    ok_all &= crc16_ccitt_false(b"123456789") == 0x29B1

    # Board detection from the 'version' reply, both profiles, as the
    # firmware prints it and as the fake does.
    ok_nano = detect_board(FakeTarget(board="EV17P63A").cmd("version")[1]) == "EV17P63A"
    ok_plat = detect_board(["[build] board: EV74H48A, dsPIC33AK512MPS512 GP DIM"]) == "EV74H48A"
    ok_none = detect_board(["[build] adc_dma_40msps"]) is None
    ok_all &= ok_nano and ok_plat and ok_none
    print("board detection from 'version' (Nano, Platform, none):",
          "PASS" if (ok_nano and ok_plat and ok_none) else "FAIL")

    print("selftest", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


# ---------------------------------------------------------------------------
# The GUI
# ---------------------------------------------------------------------------
# The DAC loopback both boards have: DAC2 drives DACOUT2 = RA8, which is
# AD5AN3 on the 128-pin MPS512 and on the Nano's 64-pin MPS506 alike, so
# ADC core 5 / PINSEL 3 reads the DAC with no wire. The test input
# ("stream on <ksps>") is exactly this route with the firmware's triangle.
LOOPBACK = {"core": 5, "pinsel": 3, "samc": 0, "dac": 2}


def detect_board(lines):
    """The board profile a 'version' reply names ("[build] board: EV17P63A,
    ..." - board.h's BOARD_NAME, printed by diag_report_build()), or None
    if it names none this tool knows."""
    for line in lines or []:
        m = re.search(r"board:\s*(EV[0-9A-Z]+)", line)
        if m and m.group(1) in BOARDS:
            return m.group(1)
    return None


def main_gui(args):
    from nicegui import ui, run

    # One command at a time on the serial port: the live loop runs its
    # commands in worker threads, and without this they could interleave
    # on the same port. acq_active: the (mode, ksps, core, pinsel, samc)
    # the board is currently streaming - None until 'stream on' succeeds,
    # and cleared by 'stream off' or by a failed grab, so the next cycle
    # knows to send 'stream on' again.
    port_lock = asyncio.Lock()
    state = dict(target=None, live=False, busy=False, cycles=0, grabs=0,
                 acq_active=None, live_t0=None, buf_size=2048,
                 settings_path=args.settings)

    def ports():
        try:
            from serial.tools import list_ports
            return [p.device for p in list_ports.comports()]
        except Exception:
            return []

    # ---- look: dark, one accent colour, rounded cards ----
    ACCENT, ACCENT2, DIM = "#22d3ee", "#a78bfa", "#94a3b8"
    ui.colors(primary=ACCENT, secondary=ACCENT2, accent=ACCENT, dark="#0b1220", positive="#34d399", negative="#f87171")
    ui.add_head_html("""<style>
      body { background: #0b1220; }
      .q-card { background: #111827 !important; border: 1px solid #1f2937; }
      .q-field__label, .q-field__native, .q-field__control { color: #e5e7eb; }
      .card-title { color: #94a3b8; font-size: 0.75rem; letter-spacing: .12em; text-transform: uppercase; }
      .mono { font-family: ui-monospace, Consolas, monospace; }
      /* Tooltips readable, and switchable off from the header ("tooltips").
         !important because some tooltips carry an inline font-size. */
      .q-tooltip { font-size: 15px !important; line-height: 1.45 !important;
                   max-width: 34rem !important; padding: 8px 12px !important; }
      body.no-tips .q-tooltip { display: none !important; }
      /* Collapsible tiles: a click on a tile's title folds everything below
         the title (its first child) away; the arrow says which state. */
      .tile .card-title { cursor: pointer; user-select: none; }
      .tile .card-title::before { content: "▾ "; color: #64748b; }
      .tile.collapsed .card-title::before { content: "▸ "; }
      .tile.collapsed > :not(:first-child) { display: none !important; }
      /* Chips readable: 14 px, and the neutral ones (color grey-8 - a value
         without a verdict) light instead of dark grey on the dark cards.
         Chips with a verdict keep their colour: positive / negative. */
      .q-chip { font-size: 14px !important; }
      .q-chip--dense { height: auto !important; padding: 3px 10px !important; }
      .q-chip.text-grey-8 { color: #cbd5e1 !important; }
    </style>
    <script>
      // Fold / unfold a tile on a click on its title, remember it per title
      // in this browser (a convenience only: a blocked localStorage just
      // means every tile opens unfolded).
      (function () {
        const KEY = "adc_gui_collapsed";
        function load() { try { return JSON.parse(localStorage.getItem(KEY) || "{}"); } catch (e) { return {}; } }
        function save(m) { try { localStorage.setItem(KEY, JSON.stringify(m)); } catch (e) {} }
        function titleOf(tile) { const t = tile.querySelector(".card-title"); return t ? t.textContent.trim() : ""; }
        document.addEventListener("click", function (ev) {
          const title = ev.target.closest(".card-title");
          if (!title) return;
          const tile = title.closest(".tile");
          if (!tile) return;
          const folded = tile.classList.toggle("collapsed");
          const m = load(); m[titleOf(tile)] = folded; save(m);
          if (!folded) setTimeout(function () { window.dispatchEvent(new Event("resize")); }, 50);
        });
        function restore() {
          const m = load();
          document.querySelectorAll(".tile").forEach(function (tile) {
            if (m[titleOf(tile)]) tile.classList.add("collapsed");
          });
        }
        if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", function () { setTimeout(restore, 300); });
        else setTimeout(restore, 300);
        setTimeout(restore, 1500);
      })();
    </script>""")

    def chart(title, x_name, y_name, y_min, y_max, colour, second_x_name=None):
        """`second_x_name` adds a top x-axis on the same grid (samples and
        time together for the time-signal chart); both axes span the same
        pixel width, so their tick positions line up as long as their
        min/max are kept proportional."""
        x_axes = [{"type": "value", "name": x_name, "min": 0, "nameTextStyle": {"color": DIM},
                   "axisLine": {"lineStyle": {"color": "#374151"}}, "axisLabel": {"color": DIM},
                   "splitLine": {"lineStyle": {"color": "#1f2937"}}}]
        grid_top = 44
        if second_x_name is not None:
            x_axes.append({"type": "value", "name": second_x_name, "min": 0, "position": "top",
                           "nameTextStyle": {"color": DIM}, "axisLine": {"lineStyle": {"color": "#374151"}},
                           "axisLabel": {"color": DIM}, "splitLine": {"show": False}})
            grid_top = 74
        return ui.echart({
            "backgroundColor": "transparent",
            "animation": False,
            "title": {"text": title, "left": 16, "top": 8,
                      "textStyle": {"color": "#e5e7eb", "fontSize": 14, "fontWeight": "normal"}},
            "grid": {"left": 64, "right": 24, "top": grid_top, "bottom": 44},
            "tooltip": {"trigger": "axis", "backgroundColor": "#1f2937", "borderColor": "#374151",
                        "textStyle": {"color": "#e5e7eb"}},
            "xAxis": x_axes,
            "yAxis": {"type": "value", "name": y_name, "min": y_min, "max": y_max,
                      "nameTextStyle": {"color": DIM}, "axisLine": {"lineStyle": {"color": "#374151"}},
                      "axisLabel": {"color": DIM}, "splitLine": {"lineStyle": {"color": "#1f2937"}}},
            "series": [{"type": "line", "showSymbol": False, "data": [], "smooth": False, "xAxisIndex": 0,
                        "lineStyle": {"width": 1.5, "color": colour},
                        "areaStyle": {"color": {"type": "linear", "x": 0, "y": 0, "x2": 0, "y2": 1,
                                                "colorStops": [{"offset": 0, "color": colour + "66"},
                                                               {"offset": 1, "color": colour + "00"}]}}}],
        }, theme="dark").classes("w-full h-80 rounded-xl")

    # ---- header ----
    with ui.header().classes("items-center gap-4 px-6").style("background: #0f172a; border-bottom: 1px solid #1f2937"):
        ui.icon("show_chart", size="md").classes("text-cyan-400")
        with ui.column().classes("gap-0"):
            ui.label("dsPIC33A ADC / DMA").classes("text-lg font-medium leading-tight")
            ui.label("triggered chain · capture · plot · FFT").classes("text-xs text-slate-400 leading-tight")
        ui.space()
        # Every control on this page explains itself in a tooltip; this
        # switches them all off (a CSS class on <body>, see the style above).
        tips_cb = ui.checkbox("tooltips", value=True).classes("text-slate-300")

        def set_tooltips(on):
            if on:
                ui.query("body").classes(remove="no-tips")
            else:
                ui.query("body").classes(add="no-tips")
        tips_cb.on_value_change(lambda e: set_tooltips(bool(e.value)))
        port_sel = ui.select(options=["fake"] + ports(), value=args.port or ("fake" if args.fake else None),
                             label="port").classes("w-44").props("dense outlined")
        conn_btn = ui.button("connect", icon="usb").props("unelevated")
        conn_chip = ui.chip("not connected", icon="link_off", color="grey-8").props("outline")

    with ui.row().classes("w-full p-4 gap-4 items-start no-wrap"):
        # ---- left: settings ----
        with ui.column().classes("gap-4").style("width: 22rem; min-width: 22rem"):
            with ui.card().classes("tile w-full rounded-xl p-4 gap-2"):
                ui.label("settings file").classes("card-title")
                settings_path_lbl = ui.label().classes("text-xs text-slate-400 mono break-all")
                with ui.row().classes("w-full gap-2"):
                    save_btn = ui.button("save", icon="save").props("unelevated dense").classes("flex-grow")
                    save_as_btn = ui.button("save as", icon="save_as").props("outline dense").classes("flex-grow")
                    load_as_btn = ui.button("load as", icon="folder_open").props("outline dense").classes("flex-grow")
                settings_msg_lbl = ui.label().classes("text-xs text-slate-400 mono")

            with ui.card().classes("tile w-full rounded-xl p-4 gap-2"):
                ui.label("acquisition · triggered chain").classes("card-title")
                rate_in = ui.number("rate, kSPS (1..40000)", value=8000, min=1, max=40000,
                                    step=100, format="%d").props("dense outlined")
                rate_hint_lbl = ui.label().classes("text-xs mono")
                input_mode_sel = ui.select(
                    {"test": "RA8 / DAC2 test triangle (core 5, pin 3)", "custom": "custom input"},
                    value="test", label="input").props("dense outlined")
                with ui.row().classes("w-full gap-2"):
                    core_sel = ui.select({n: f"ADC{n}" for n in range(1, 6)}, value=3,
                                         label="core (1..5)").props("dense outlined").classes("flex-grow")
                    input_in = ui.number("PINSEL (0..15, 6 = internal ref)", value=5, min=0, max=15,
                                         step=1, format="%d").props("dense outlined").classes("flex-grow")
                channel_info_lbl = ui.label().classes("text-xs text-slate-400")
                samc_in = ui.number("SAMC · sample time (0..31)", value=0, min=0, max=31, step=1, format="%d").props("dense outlined")
                interval_in = ui.number("grab interval, ms", value=500, min=50, max=5000, step=50, format="%d").props("dense outlined")
                with ui.row().classes("w-full gap-2"):
                    single_btn = ui.button("single", icon="camera").props("unelevated").classes("flex-grow")
                    live_btn = ui.button("live", icon="play_arrow").props("unelevated").classes("flex-grow")

            with ui.card().classes("tile w-full rounded-xl p-4 gap-2"):
                ui.label("fake target signal · custom input only").classes("card-title")
                sig_in = ui.number("frequency, kHz", value=100.0, min=0.1, max=20000.0, step=10).props("dense outlined")
                amp_in = ui.number("amplitude, counts (pk)", value=1500.0, min=0.0, max=2000.0, step=50).props("dense outlined")
                noise_in = ui.number("noise, counts (std dev, sets SNR)", value=6.0, min=0.0, max=500.0, step=1).props("dense outlined")
                harm2_in = ui.number("2nd harmonic, counts (pk)", value=150.0, min=0.0, max=1000.0, step=10).props("dense outlined")
                harm3_in = ui.number("3rd harmonic, counts (pk)", value=0.0, min=0.0, max=1000.0, step=10).props("dense outlined")

            # One card per DAC. Both units are the same hardware with
            # different registers (dac.c), and each has its own output pin:
            # DAC1 on RA1, DAC2 on RA8. Either can feed an ADC channel -
            # its own pin without a wire, any other pin with one, which is
            # what the board tile spells out. Switched on here, they are
            # applied automatically whenever the chain runs with a custom
            # (non-test) input.
            dac_ui = {}
            for _unit, _pin_name in ((1, "RA1"), (2, "RA8")):
                with ui.card().classes("tile w-full rounded-xl p-4 gap-2"):
                    ui.label(f"dac{_unit} · triangle ({_pin_name})").classes("card-title")
                    _on = ui.select({True: "on", False: "off"}, value=False,
                                    label=f"dac{_unit}").props("dense outlined")
                    with ui.row().classes("w-full gap-2"):
                        _low = ui.number("low (0..4095)", value=0x100, min=0, max=4095, step=16,
                                         format="%d").props("dense outlined").classes("flex-grow")
                        _high = ui.number("high (0..4095, > low)", value=0xF00, min=0, max=4095,
                                          step=16, format="%d").props("dense outlined").classes("flex-grow")
                    _slp = ui.number("SLPDAT · slope (1..255, counts/DAC clock; 8 = 22 kHz)",
                                     value=8, min=1, max=255, step=1, format="%d").props("dense outlined")
                    _freq = ui.label().classes("text-cyan-300 mono")
                    _btn = ui.button(f"apply dac{_unit}", icon="graphic_eq").props("unelevated").classes("w-full")
                    _msg = ui.label().classes("text-xs text-slate-400 mono")
                    # Shown while the test input is chosen: 'stream on <ksps>'
                    # starts DAC2's triangle itself (chaintest.c triangle_for()),
                    # so what is set here only acts on a custom input.
                    _note = ui.label("test input chosen: the firmware runs DAC2's triangle itself - "
                                     "these settings act on a custom input (e.g. core 5 / AN3 = RA8 "
                                     "for DAC2, core 5 / AN1 = RA1 for DAC1)").classes("text-xs text-amber-400")
                    dac_ui[_unit] = {"on": _on, "low": _low, "high": _high, "slp": _slp,
                                     "freq": _freq, "btn": _btn, "msg": _msg, "note": _note}

            with ui.card().classes("tile w-full rounded-xl p-4 gap-2"):
                ui.label("buffer").classes("card-title")
                with ui.row().classes("w-full gap-2 items-end"):
                    buf_in = ui.number("buffer size, total ('buf')", value=2048, min=16, max=8192, step=16,
                                       format="%d").props("dense outlined").classes("flex-grow")
                    buf_btn = ui.button("apply", icon="tune").props("unelevated dense")
                buf_lbl = ui.label("buf: not queried yet").classes("text-xs text-slate-400 mono")

        # ---- right: results ----
        with ui.column().classes("flex-grow gap-4"):
            with ui.row().classes("w-full items-center gap-2"):
                cyc_lbl = ui.label("no grab yet").classes("text-slate-300 mono")
                ui.space()
                COUNTER_TIPS = {
                    "overrun": "DMA overruns since the PREVIOUS grab, not a running total - a "
                               "lower bound, as always: OVERRUN is one bit in DMA0STAT and the "
                               "handler counts one per entry that found it set, so several losses "
                               "between two entries still move it by one.",
                    "late": "HALF and DONE both pending at once since the previous grab: the "
                            "handler ran a whole half late.",
                    "missed": "Halves the firmware's own main-loop processing skipped since the "
                              "previous grab - this page's own halt/grab/restart cycle does not "
                              "count against it.",
                }
                chips = {}
                for _k in ("overrun", "late", "missed"):
                    chips[_k] = ui.chip(f"{_k} –", color="grey-8").props("dense outline")
                    with chips[_k]:
                        ui.tooltip(COUNTER_TIPS[_k]).style("font-size: 14px; max-width: 24rem;")
                rate_chip = ui.chip("actual rate –", color="grey-8").props("dense outline")
                halves_chip = ui.chip("halves/xfer –", color="grey-8").props("dense outline")
                with rate_chip:
                    ui.tooltip("The frame's own 'ksps' - the nearest 160 MHz / N (CLKGEN13) the "
                               "chain actually runs at, not the number typed on the left. Used as "
                               "fs for the time axis and the FFT.").style("font-size: 14px; max-width: 24rem;")
                with halves_chip:
                    ui.tooltip("Buffer halves completed and DMA transfers since the previous grab "
                               "(chain_stream_grab_begin()'s per-cycle counters).")\
                        .style("font-size: 14px; max-width: 24rem;")
            with ui.card().classes("tile w-full rounded-xl p-2"):
                ui.label("time signal").classes("card-title px-2 pt-1")
                with ui.row().classes("w-full items-center gap-3 px-2"):
                    vref_in = ui.number("reference voltage, V", value=3.3, min=0.1, max=5.5, step=0.05,
                                        format="%.3f").props("dense outlined").style("width: 12rem")
                    ui.label("ADC full scale (4096 counts) = this voltage; the right axis and the "
                             "tooltip show volts from it").classes("text-xs text-slate-400")
                time_chart = chart("", "sample", "ADC counts", 0, 4096, ACCENT, second_x_name="time")
                # The right-hand axis in volts, on the same grid as the counts:
                # both span 0..full scale, so their ticks describe the same heights.
                time_chart.options["yAxis"] = [time_chart.options["yAxis"], {
                    "type": "value", "min": 0, "max": 3.3, "position": "right",
                    "axisLine": {"lineStyle": {"color": "#374151"}},
                    "axisLabel": {"color": DIM, "formatter": "{value} V"},
                    "splitLine": {"show": False}}]
                # room for the volt labels on the right, the time axis name
                # pinned to the top right end of its own (top) axis
                time_chart.options["grid"]["right"] = 72
                time_chart.options["xAxis"][1]["nameLocation"] = "end"
                time_chart.options["xAxis"][1]["nameGap"] = 8
                # with two y axes ECharts pulls an axis line "onZero" to the
                # other axis' zero - the top time axis (and its name) would
                # sit at the bottom, on top of "sample"
                time_chart.options["xAxis"][1]["axisLine"]["onZero"] = False
            with ui.card().classes("tile w-full rounded-xl p-2"):
                ui.label("spectrum · Hann window").classes("card-title px-2 pt-1")
                fft_chart = chart("", "kHz", "dBFS", -100, 0, ACCENT2)
            with ui.card().classes("tile w-full rounded-xl p-3"):
                ui.label("signal evaluation").classes("card-title")
                EVAL_TOOLTIPS = {
                    "fundamental": "Frequency of the strongest spectral line (DC excluded) -- the detected signal frequency.",
                    "level": "Amplitude of the fundamental, in dBFS (0 dB = full scale, 4096 ADC counts).",
                    "noise floor": "Median spectrum level outside the fundamental and its harmonics, in dBFS.",
                    "SNR": "Signal-to-noise ratio: fundamental level minus the noise floor, in dB. Higher is cleaner.",
                    "THD": "Total harmonic distortion: combined power of harmonics 2..5 relative to the fundamental, in percent.",
                    "H2": "2nd harmonic level relative to the fundamental, in dBc (dB below carrier).",
                    "H3": "3rd harmonic level relative to the fundamental, in dBc.",
                    "H4": "4th harmonic level relative to the fundamental, in dBc.",
                    "H5": "5th harmonic level relative to the fundamental, in dBc.",
                }
                with ui.row().classes("w-full gap-2 flex-wrap"):
                    eval_chips = {}
                    for k in ("fundamental", "level", "noise floor", "SNR", "THD", "H2", "H3", "H4", "H5"):
                        chip = ui.chip(f"{k} –", color="grey-8").props("dense outline")
                        with chip:
                            ui.tooltip(EVAL_TOOLTIPS[k]).style("font-size: 14px; max-width: 22rem;")
                        eval_chips[k] = chip

            # ---- the triangle verdict: only meaningful for the test input ----
            with ui.card().classes("tile w-full rounded-xl p-3 gap-2") as triangle_card:
                with ui.row().classes("w-full items-center gap-3 flex-wrap"):
                    ui.label("triangle verdict · test input only").classes("card-title")
                    triangle_verdict_chip = ui.chip("no grab yet", color="grey-8").props("dense outline")
                ui.label(
                    "Shown only when the grab's own frame says a test triangle was the signal "
                    "(slp > 0 in the GRAB header) - a custom input's grab has nothing to judge "
                    "against a model, and this card is hidden for it. Evaluated with the same "
                    "tri_eval()/grid_ok() the firmware's own chain test uses (tools/eval_chain.py, "
                    "imported, not re-implemented): a lost or repeated sample shifts a turning "
                    "point off the grid by a whole sample and fails it."
                ).classes("text-xs text-slate-400")
                with ui.row().classes("w-full gap-2 flex-wrap"):
                    TRIANGLE_TIPS = {
                        "turning points": "Coarse peaks and troughs tri_eval() found in the window.",
                        "up": "Complete rising slopes: count and mean length in samples.",
                        "down": "Complete falling slopes: count and mean length in samples.",
                        "slip": "The grid verdict's own number: a lost or repeated sample shifts "
                                "later turning points by a whole sample: PASS needs it under 0.5.",
                        "model": "Measured mean slope length over the model from slpdat and the "
                                 "DAC clock the frame reports (chaintest.c triangle_for()) - near "
                                 "1.0 on a clean grid.",
                        "steps": "Lost (zero) or repeated (dbl) single-sample steps, only checked "
                                 "once the slope is steep enough (>= 40 LSB/sample) to tell them "
                                 "from DAC/DNL noise.",
                    }
                    triangle_chips = {}
                    for _k in ("turning points", "up", "down", "slip", "model", "steps"):
                        chip = ui.chip(f"{_k} –", color="grey-8").props("dense outline")
                        with chip:
                            ui.tooltip(TRIANGLE_TIPS[_k]).style("font-size: 14px; max-width: 22rem;")
                        triangle_chips[_k] = chip

            # ---- the package: where the selected channel physically is ----
            # Full width, every pin labelled with its number and port name.
            # The eval kit decides which device, and so which package, is
            # drawn. Kit, core and channel are repeated on the board tile
            # below and on the sidebar; all of them are the same selection.
            board_ctrls, core_ctrls, chan_ctrls, dac_ctrls = [], [], [], []
            # custom: the user's own input (core, pinsel, samc) - kept apart
            # from the fields, which show the loopback while the test input
            # is chosen; dac_custom: the signal source shown for it.
            ui_state = {"board": BOARD_DEFAULT, "sync": False, "dac": 0, "mode": "test",
                        "custom": None, "dac_custom": 0}

            def selection_row(tag):
                """kit / core / channel, one row, for a tile header."""
                board = ui.select(BOARD_OPTIONS, value=BOARD_DEFAULT, label="eval kit") \
                    .props("dense outlined").style("min-width: 21rem")
                core = ui.select({n: f"ADC{n}" for n in range(1, 6)}, value=3, label="core") \
                    .props("dense outlined").style("min-width: 7.5rem")
                chan = ui.select({}, label="channel (PINSEL)") \
                    .props("dense outlined").style("min-width: 12rem")
                dac = ui.select(DAC_OPTIONS, value=0, label="signal source") \
                    .props("dense outlined").style("min-width: 10rem")
                lbl = ui.label().classes("text-cyan-300 mono text-sm")
                board_ctrls.append(board)
                core_ctrls.append(core)
                chan_ctrls.append(chan)
                dac_ctrls.append(dac)
                return lbl

            with ui.card().classes("tile w-full rounded-xl p-3 gap-2"):
                with ui.row().classes("w-full items-center gap-3 flex-wrap"):
                    ui.label("chip · pinout").classes("card-title")
                    chip_pin_lbl = selection_row("chip")
                chip_html = ui.html("").classes("w-full").style("max-width: 1100px; margin: 0 auto")
                ui.label("Cyan = the selected channel, dim cyan = the other inputs of that core, "
                         "violet = DAC outputs, red = supply, grey = ground. Hover a pin for all "
                         "its functions. Pin tables: data sheet DS70005591D, Table 11 for the "
                         "TQFP-128 and Table 5 for the Nano's 64-pin part.").classes("text-xs text-slate-400")

            # ---- the board: where that pin comes out on the kit ----------
            with ui.card().classes("tile w-full rounded-xl p-3 gap-2"):
                with ui.row().classes("w-full items-center gap-3 flex-wrap"):
                    ui.label("board · where to wire it").classes("card-title")
                    board_pin_lbl = selection_row("board")
                board_html = ui.html("").classes("w-full").style("max-width: 1100px; margin: 0 auto")
                board_note_lbl = ui.label().classes("text-xs text-slate-400")

    # ---- console: the CLI traffic with the target (real or fake) ----
    with ui.card().classes("tile w-full rounded-xl p-3 mx-4 mb-4"):
        ui.label("console").classes("card-title")
        console_log = ui.log(max_lines=1000).classes("w-full h-40").style(
            "background: #0b1220; color: #a7f3d0; font-family: ui-monospace, Consolas, monospace; "
            "font-size: 12px; white-space: pre;")

    def push_log(line: str):
        console_log.push(f"{time.strftime('%H:%M:%S')}  {line}")

    # Board limits from the last hardware run (HARDWARE-LOG.md), shown as
    # guidance only - the rate field does not enforce any of this.
    def update_rate_hint():
        try:
            ksps = float(rate_in.value or 0)
        except (TypeError, ValueError):
            ksps = 0.0
        if ksps <= 8000:
            txt, cls = "the board ran clean to about 8 MSPS with the CPU processing", "text-cyan-300 mono"
        elif ksps <= 10000:
            txt, cls = "guidance: close to where the board showed occasional DMA overruns (~10 MSPS)", "text-amber-400 mono"
        elif ksps <= 16000:
            txt, cls = "guidance: occasional DMA overruns reported in this range (~10-16 MSPS)", "text-amber-400 mono"
        elif ksps <= 20000:
            txt, cls = "guidance: lost triggers reported from ~16 MSPS; the chain measured up to ~18-20 MSPS", "text-amber-400 mono"
        else:
            txt, cls = "guidance: above the ~18-20 MSPS the triggered chain has reached so far", "text-red-400 mono"
        rate_hint_lbl.text = txt
        rate_hint_lbl.classes(replace=cls)
    rate_in.on_value_change(lambda e: update_rate_hint())
    update_rate_hint()

    # ---- tooltips ----------------------------------------------------
    # Every control says what it is and what it changes on the board. The
    # console command it maps to is named where there is one, so the page
    # can be read next to cli.c and next to a terminal log.
    TIPS = [
        (port_sel, "Which console to talk to. 'fake' is the built-in stand-in: it answers the "
                   "same commands and makes up a signal, so the page can be tried without a "
                   "board. A COMx entry is the board's USB-UART at 115200 baud."),
        (conn_btn, "Open or close that port. Everything else on this page needs it: each control "
                   "sends a console command and waits for the prompt before the next one."),
        (conn_chip, "Connection state. It also shows the firmware's build line once connected, "
                    "which carries the git revision it was built from."),
        (save_btn, "Write every setting on this page back to the settings file named above."),
        (save_as_btn, "Write the settings to a file you name, without changing which file the "
                      "page started from."),
        (load_as_btn, "Read settings from a file you name and put them on the page. Keys the "
                      "file does not carry keep their built-in default."),
        (rate_in, "The chain's sample rate: 'stream on <ksps> ...' - the nearest 160 MHz / N "
                  "(CLKGEN13) is what actually runs, and the frame's own 'ksps' (shown as 'actual "
                  "rate' once a grab has come in) is what the charts use as fs. The label "
                  "underneath is guidance from the last hardware run, not a limit."),
        (input_mode_sel, "The chain's input. The test triangle is core 5 / PINSEL 3 (RA8) with "
                         "the firmware's own DAC2 triangle started as the signal: 'stream on "
                         "<ksps>'. Any other input leaves the DAC alone - switch a DAC on below "
                         "if it should drive this pin - and sends 'stream on <ksps> <core> "
                         "<pinsel> <samc>'."),
        (core_sel, "ADC core (1..5), part of the chain's input when 'custom' is selected above - "
                   "fixed at 5 for the test triangle. Also the core shown in the chip and board "
                   "tiles below."),
        (input_in, "PINSEL: the analog input of that core - fixed at 3 (RA8) for the test "
                   "triangle. 6 is the internal 15/16 x VDD reference, 7 the internal UREF line."),
        (samc_in, "SAMC: how long the ADC samples before it converts, in steps of 2 x SAMC + 0.5 "
                  "TAD. Part of 'stream on ... <samc>' for a custom input; fixed at 0 for the "
                  "test triangle."),
        (interval_in, "How often this page halts the chain for one grab, in milliseconds (plus "
                      "however long the transfer itself takes at the current baud rate)."),
        (single_btn, "One grab: if the chain is not already streaming, start it first ('stream "
                     "on'), take one 'stream grab', then stop it again ('stream off'). Disabled "
                     "while LIVE is running - stop LIVE first."),
        (live_btn, "Start the chain if it is not already running at this rate/input, then repeat "
                   "'stream grab' at the interval above until stopped. A rate or input change "
                   "while LIVE is running is picked up before the next grab. STOP sends 'stream "
                   "off', which restores the boot configuration."),
        (sig_in, "Frequency of the fake target's sine, in kHz - only used with --fake and a "
                 "custom input. Only meaningful below half the sample rate - above that the FFT "
                 "shows the alias, which is itself worth seeing."),
        (amp_in, "Amplitude of the fake target's sine in ADC counts, peak. Full scale is 4096 "
                 "counts, so 2048 is the largest undistorted swing around mid scale."),
        (noise_in, "Gaussian noise the fake target adds, standard deviation in counts. This is "
                   "what sets the SNR the evaluation row reports."),
        (harm2_in, "Second harmonic the fake target adds, peak counts. Use it to see what the THD "
                   "and H2 figures do with a known distortion."),
        (harm3_in, "Third harmonic the fake target adds, peak counts."),
        (buf_in, "Total size of the ping-pong buffer in samples; each half is half of it - and "
                 "half of it is exactly what one 'stream grab' sends. Console: 'buf <n>'."),
        (buf_btn, "Send the buffer size. The firmware sets the DMA block to match at the next "
                  "'stream on'."),
    ]
    for _u, _c in sorted(dac_ui.items()):
        _pin = "RA1" if _u == 1 else "RA8"
        TIPS += [
            (_c["on"], f"Switch DAC{_u} on or off. It drives pin {_pin} with a triangle in "
                       "hardware, no CPU involved, and that pin is also an ADC input of core 5 - "
                       "so the ADC can read it back with no wire. Applied automatically whenever "
                       "the chain runs with a custom input, or directly with this button. "
                       f"Console: 'dac {_u} on|off ...'."),
            (_c["low"], "Lower end of the triangle, as a 12-bit DAC code. 0 is ground, 4095 is "
                        "VDD, and the DAC's own limits keep the usable range a little inside that."),
            (_c["high"], "Upper end of the triangle, as a 12-bit DAC code. Must be above the lower "
                         "end. The difference is the swing the ADC should see."),
            (_c["slp"], "SLPDAT: how many DAC codes the slope generator steps per DAC clock. "
                        "Larger is faster, so the period shown below shrinks."),
            (_c["btn"], f"Send these settings to DAC{_u} right now. The line underneath is the "
                        "board's own answer, including the period it computed."),
        ]
    for _sel in board_ctrls:
        TIPS.append((_sel, "Which evaluation kit is in front of you. It picks the device and so "
                           "the package drawn above, and it decides where a channel comes out: "
                           "the EV74H48A has mikroBUS and XPLAINED PRO headers, the Nano two rows "
                           "of edge pads."))
    for _sel in core_ctrls:
        TIPS.append((_sel, "ADC core, the same selection as in the acquisition card. Changing it "
                           "here changes it everywhere and redraws both tiles."))
    for _sel in chan_ctrls:
        TIPS.append((_sel, "Analog input of that core, with the pin it sits on. Internal inputs "
                           "are marked as such: they have no pin and cannot be wired to."))
    for _sel in dac_ctrls:
        TIPS.append((_sel, "Which DAC to show as the signal source. The drawings then light its "
                           "pin, and say whether it reaches the selected channel by itself or "
                           "what to wire to what."))
    for _el, _text in TIPS:
        _el.tooltip(_text)

    # ---- settings: the whole page in one dict, and back ----
    def settings_collect():
        return {
            "version": SETTINGS_VERSION,
            "board": ui_state["board"],
            "view": {"dac_source": int(ui_state["dac_custom"] if ui_state["mode"] == "test"
                                        else ui_state["dac"]),
                     "tooltips": bool(tips_cb.value),
                     "vref": float(vref_in.value or 3.3)},
            "acquisition": {
                "mode": input_mode_sel.value or "test",
                "ksps": int(rate_in.value or 8000),
                "core": custom_input()[0], "pinsel": custom_input()[1],
                "samc": custom_input()[2],
                "interval_ms": int(interval_in.value or 500),
            },
            "buffer": {"size": int(buf_in.value or 2048)},
            "dac": {str(u): {"on": bool(c["on"].value), "low": int(c["low"].value or 0),
                             "high": int(c["high"].value or 0), "slpdat": int(c["slp"].value or 0)}
                    for u, c in dac_ui.items()},
            "fake": {"signal_khz": float(sig_in.value or 0.0),
                     "amplitude": float(amp_in.value or 0.0), "noise": float(noise_in.value or 0.0),
                     "harmonic2": float(harm2_in.value or 0.0),
                     "harmonic3": float(harm3_in.value or 0.0)},
        }

    def settings_apply(cfg):
        """Write a settings dict onto the controls. Anything out of range
        for the current build is skipped rather than forced. Keys from an
        older settings file (pll/sweep/capture/chain, before 25.09.2026)
        are simply not read."""
        if cfg.get("board") in BOARDS:
            ui_state["board"] = cfg["board"]
        ui_state["dac"] = int(cfg.get("view", {}).get("dac_source", 0))
        ui_state["dac_custom"] = ui_state["dac"]
        tips_cb.value = bool(cfg.get("view", {}).get("tooltips", True))
        vref_in.value = float(cfg.get("view", {}).get("vref", 3.3))
        set_tooltips(bool(tips_cb.value))
        acq = cfg.get("acquisition", {})
        ui_state["mode"] = "custom"               # so that on_mode_change() below starts clean
        # the mode first: setting it can run on_mode_change() at once, which
        # keeps the fields as they are then - the file's input comes after
        input_mode_sel.value = acq.get("mode") if acq.get("mode") in ("test", "custom") else "test"
        ui_state["custom"] = (int(acq.get("core", 3)), int(acq.get("pinsel", 5)), int(acq.get("samc", 0)))
        rate_in.value = int(acq.get("ksps", 8000))
        core_sel.value, input_in.value, samc_in.value = ui_state["custom"]
        interval_in.value = int(acq.get("interval_ms", 500))
        buf_in.value = int(cfg.get("buffer", {}).get("size", 2048))
        for unit, c in dac_ui.items():
            d = cfg.get("dac", {}).get(str(unit), {})
            c["on"].value = bool(d.get("on", False))
            c["low"].value = int(d.get("low", 0x100))
            c["high"].value = int(d.get("high", 0xF00))
            c["slp"].value = int(d.get("slpdat", 8))
        fake = cfg.get("fake", {})
        sig_in.value = float(fake.get("signal_khz", 100.0))
        amp_in.value = float(fake.get("amplitude", 1500.0))
        noise_in.value = float(fake.get("noise", 6.0))
        harm2_in.value = float(fake.get("harmonic2", 150.0))
        harm3_in.value = float(fake.get("harmonic3", 0.0))
        update_dac_freq_label()
        update_rate_hint()
        on_mode_change()

    def show_settings_path():
        settings_path_lbl.text = state["settings_path"]

    def do_save(path=None):
        path = path or state["settings_path"]
        settings_msg_lbl.text = settings_write(path, settings_collect())
        state["settings_path"] = path
        show_settings_path()

    def do_load(path):
        cfg, msg = settings_read(path)
        settings_apply(cfg)
        state["settings_path"] = path
        settings_msg_lbl.text = msg
        show_settings_path()

    def ask_path(title, action, button):
        with ui.dialog() as dlg, ui.card().classes("rounded-xl p-4 gap-3").style("min-width: 28rem"):
            ui.label(title).classes("card-title")
            field = ui.input("file", value=state["settings_path"]).props("dense outlined").classes("w-full")
            with ui.row().classes("w-full justify-end gap-2"):
                ui.button("cancel", on_click=dlg.close).props("flat")

                def go():
                    dlg.close()
                    action(field.value.strip())
                ui.button(button, on_click=go).props("unelevated")
        dlg.open()

    save_btn.on_click(lambda e: do_save())
    save_as_btn.on_click(lambda e: ask_path("save settings as", do_save, "save"))
    load_as_btn.on_click(lambda e: ask_path("load settings from", do_load, "load"))

    # ---- the input mode: test (the DAC loopback, fixed) or custom ----
    def custom_input():
        """The user's own input: the fields while custom is chosen, the
        kept copy while the fields show the loopback."""
        if ui_state["mode"] == "custom" or ui_state["custom"] is None:
            return (int(core_sel.value), int(input_in.value or 0), int(samc_in.value or 0))
        return ui_state["custom"]

    def on_mode_change(e=None):
        is_test = input_mode_sel.value == "test"
        if is_test:
            if ui_state["mode"] == "custom":          # leaving custom: keep it
                ui_state["custom"] = (int(core_sel.value), int(input_in.value or 0),
                                      int(samc_in.value or 0))
                ui_state["dac_custom"] = int(ui_state["dac"])
            core_sel.value = LOOPBACK["core"]
            input_in.value = LOOPBACK["pinsel"]
            samc_in.value = LOOPBACK["samc"]
            ui_state["dac"] = LOOPBACK["dac"]
        elif ui_state["mode"] == "test":               # back to custom: restore it
            if ui_state["custom"] is not None:
                core_sel.value, input_in.value, samc_in.value = ui_state["custom"]
            ui_state["dac"] = int(ui_state["dac_custom"])
        ui_state["mode"] = "test" if is_test else "custom"
        for el in [core_sel, input_in, samc_in] + core_ctrls + chan_ctrls + dac_ctrls:
            el.set_enabled(not is_test)
        for _c in dac_ui.values():
            _c["note"].set_visibility(is_test)
        refresh_channel()
    input_mode_sel.on_value_change(on_mode_change)

    # One selection - eval kit, ADC core, channel - shown by the sidebar
    # and by both tiles. Any of them may change it; refresh_channel() writes
    # it back to all of them and redraws, with a flag so the writes do not
    # bounce back through the handlers they trigger.
    def channel_options(board_key, core):
        pkg = package_of(board_key)
        opts = {}
        for ps in pkg.channels(core):
            if ps in INTERNAL_INPUTS:
                opts[ps] = f"AN{ps} · internal"
            else:
                opts[ps] = f"AN{ps} · {pkg.adc_pin[(core, ps)].port}"
        return opts

    def refresh_channel():
        if ui_state["sync"]:
            return
        ui_state["sync"] = True
        try:
            board_key = ui_state["board"]
            core = int(core_sel.value)
            pinsel = int(input_in.value if input_in.value is not None else 0)
            chans = package_of(board_key).channels(core)
            if pinsel not in chans:                  # not on this package
                pinsel = chans[0]
                input_in.value = pinsel
            opts = channel_options(board_key, core)
            for sel in board_ctrls:
                sel.value = board_key
            for sel in core_ctrls:
                sel.value = core
            for sel in chan_ctrls:
                sel.set_options(opts, value=pinsel)
            unit = int(ui_state["dac"])
            dac_on = bool(dac_ui[unit]["on"].value) if unit in dac_ui else False
            if ui_state["mode"] == "test":
                unit, dac_on = LOOPBACK["dac"], True   # 'stream on <ksps>' starts DAC2 itself
            for sel in dac_ctrls:
                sel.value = unit
            info = channel_pin_info(board_key, core, pinsel)
            channel_info_lbl.text = info
            chip_html.content = chip_svg(board_key, core, pinsel, True, unit, dac_on)
            board_html.content = board_svg(board_key, core, pinsel, unit, dac_on)
            site = channel_site(board_key, core, pinsel)
            chip_pin_lbl.text = site[2] or site[1]
            needed, head, _sub = wire_hint(board_key, core, pinsel, unit)
            board_pin_lbl.text = site[1] + (f"   |   {head}" if head else "")
            board_note_lbl.text = (
                "Cyan = where the channel comes out. Grey pads carry another signal, dark pads are "
                "power, ground or not on the device. Hover a pad for its device pin. Sources: DIM "
                "information sheet DS70005563A for the EV74H48A, Curiosity Nano user guide "
                "DS70005634A for the EV17P63A.")
        except Exception as exc:                      # never take the page down
            channel_info_lbl.text = f"pin lookup failed: {exc}"
        finally:
            ui_state["sync"] = False

    def on_board_change(e):
        if ui_state["sync"]:
            return
        ui_state["board"] = e.value
        board = BOARDS[e.value]
        pkg = package_of(e.value)
        core, pinsel = int(core_sel.value), int(input_in.value or 0)
        if (core, pinsel) not in pkg.adc_pin and pinsel not in INTERNAL_INPUTS:
            core_sel.value = board["default_core"]
            input_in.value = board["default_pinsel"]
        refresh_channel()

    for sel in board_ctrls:
        sel.on_value_change(on_board_change)
    for sel in core_ctrls:
        sel.on_value_change(lambda e: (setattr(core_sel, "value", int(e.value)), refresh_channel()))
    for sel in chan_ctrls:
        sel.on_value_change(lambda e: (setattr(input_in, "value", int(e.value)), refresh_channel())
                            if e.value is not None else None)

    def on_dac_pick(e):
        if ui_state["sync"] or e.value is None:
            return
        ui_state["dac"] = int(e.value)
        refresh_channel()

    for sel in dac_ctrls:
        sel.on_value_change(on_dac_pick)
    for _u, _c in dac_ui.items():
        _c["on"].on_value_change(lambda e: refresh_channel())
    core_sel.on_value_change(lambda e: refresh_channel())
    input_in.on_value_change(lambda e: refresh_channel())
    refresh_channel()

    # The time chart's tooltip: time on top, "counts  (dot)  volts" in the
    # middle, the sample number below. Built as a JS function (NiceGUI turns
    # a ':'-prefixed key into one) with the current time step and reference
    # voltage baked in; rebuilt whenever either changes.
    time_axis = {"per_sample": 1e6 / 8e6, "unit": "µs"}

    def update_time_tooltip():
        vref = float(vref_in.value or 3.3)
        per, unit = time_axis["per_sample"], time_axis["unit"]
        time_chart.options["yAxis"][1]["max"] = vref
        time_chart.options["tooltip"][":formatter"] = (
            "p => { const s = p[0].data[0], c = p[0].data[1];"
            f" return (s * {per!r}).toFixed(3) + ' {unit}<br/>'"
            " + '<b>' + c + '</b>&nbsp; ' + p[0].marker + ' '"
            f" + (c * {vref!r} / 4096).toFixed(3) + ' V<br/>'"
            " + '<span style=\"color:#94a3b8\">sample ' + s + '</span>'; }")
        time_chart.update()
    vref_in.on_value_change(lambda e: update_time_tooltip())
    update_time_tooltip()

    def update_dac_freq_label():
        """The triangle's period from low/high/slpdat, for every DAC card."""
        for c in dac_ui.values():
            try:
                low = int(c["low"].value or 0)
                high = int(c["high"].value or 0)
                slp = int(c["slp"].value or 0)
                period_ns = dac_period_ns_of(low, high, slp)
                c["freq"].text = (f"period {period_ns/1e3:.2f} µs  ({1e9/period_ns/1e3:.2f} kHz)"
                                  if period_ns > 0 else "invalid: need high > low and slpdat > 0")
            except Exception:
                c["freq"].text = ""
    for _c in dac_ui.values():
        for _k in ("low", "high", "slp"):
            _c[_k].on_value_change(lambda e: update_dac_freq_label())
    update_dac_freq_label()

    # The file has the last word over the built-in defaults above.
    state["settings_path"] = args.settings
    _cfg, _msg = settings_read(args.settings)
    settings_apply(_cfg)
    settings_msg_lbl.text = _msg
    show_settings_path()

    def set_chip(name, value):
        c = chips[name]
        c.text = f"{name} {value}"
        c.props(f'color={"positive" if value == 0 else "negative"}')

    # ---- connection ----
    def do_connect():
        if state["target"]:
            push_log(f"--- disconnected: {state['target'].port} ---")
            state["live"] = False
            live_btn.text, live_btn.icon = "live", "play_arrow"
            single_btn.enable()
            state["target"].close()
            state["target"] = None
            state["acq_active"] = None
            conn_btn.text, conn_btn.icon = "connect", "usb"
            conn_chip.text, conn_chip.icon = "not connected", "link_off"
            conn_chip.props("color=grey-8")
            buf_lbl.text = "buf: not queried yet"
            return
        try:
            if port_sel.value == "fake":
                push_log("--- connecting: fake target ---")
                state["target"] = FakeTarget(board=args.fake_board,
                                             signal_khz=float(sig_in.value or 100.0),
                                             amplitude=float(amp_in.value or 1500.0),
                                             noise_std=float(noise_in.value or 6.0),
                                             harm2_amp=float(harm2_in.value or 150.0),
                                             harm3_amp=float(harm3_in.value or 0.0),
                                             on_log=push_log)
            else:
                push_log(f"--- connecting: {port_sel.value} ---")
                state["target"] = Target(port_sel.value, on_log=push_log)
            ok, lines = state["target"].cmd("version")
            state["acq_active"] = None
            # The firmware names its board: follow it, and start from that
            # board's default measurement input when it is a different one.
            found = detect_board(lines) if ok else None
            if found and found != ui_state["board"]:
                # A different board: start from the DAC loopback, which works
                # on both with no wire, and keep the board's own measurement
                # input as the custom one (Nano: core 1 / PINSEL 0 = RA2).
                push_log(f"--- board detected: {found} - profile switched, input set to the "
                         f"DAC2 loopback (core 5, AN3 = RA8) ---")
                ui_state["board"] = found
                input_mode_sel.value = "test"
                ui_state["custom"] = (BOARDS[found]["default_core"],
                                      BOARDS[found]["default_pinsel"], 0)
                ui_state["dac_custom"] = 0
                if ui_state["mode"] != "test":
                    on_mode_change()
                refresh_channel()
            conn_chip.text = (lines[0] if ok and lines else f"{port_sel.value}: connected")
            if found:
                conn_chip.text += f"   ·   {found}"
            conn_chip.icon = "link"
            conn_chip.props("color=positive")
            conn_btn.text, conn_btn.icon = "disconnect", "usb_off"
            state["buf_size"] = query_buf(state["target"])
            buf_in.value = state["buf_size"]
            buf_lbl.text = f"buf: {state['buf_size']} (half {state['buf_size'] // 2})"
        except Exception as ex:
            push_log(f"--- connect failed: {ex} ---")
            conn_chip.text = f"connect failed: {ex}"
            conn_chip.icon = "error"
            conn_chip.props("color=negative")
            state["target"] = None
    conn_btn.on_click(do_connect)

    async def apply_dac(unit):
        """cli.c: dac <1|2> <on|off> [low] [high] [slpdat]"""
        c = dac_ui[unit]
        t = state["target"]
        if not t:
            c["msg"].text = "not connected"
            return
        if not c["on"].value:
            ok, lines = await run.io_bound(t.cmd, f"dac {unit} off")
        else:
            low, high = int(c["low"].value or 0), int(c["high"].value or 0)
            slp = int(c["slp"].value or 0)
            ok, lines = await run.io_bound(t.cmd, f"dac {unit} on {low} {high} {slp}")
        c["msg"].text = "   ".join(lines) if lines else (f"dac{unit} applied" if ok else f"dac{unit} refused")
        refresh_channel()
    for _u in sorted(dac_ui):
        dac_ui[_u]["btn"].on_click(lambda e, u=_u: apply_dac(u))

    async def apply_active_dacs():
        """Every DAC switched on in its card, sent to the board - what a
        custom (non-test) input needs, since 'stream on <ksps> <core>
        <pinsel> <samc>' leaves the DAC alone on purpose (chaintest.c)."""
        for u in sorted(dac_ui):
            if dac_ui[u]["on"].value:
                await apply_dac(u)

    async def apply_buf():
        t = state["target"]
        if not t:
            buf_lbl.text = "not connected"
            return
        n = int(buf_in.value or state["buf_size"])
        async with port_lock:
            ok, lines = await run.io_bound(t.cmd, f"buf {n}")
        got = _parse_buf(lines)
        if ok and got is not None:
            state["buf_size"] = got
            buf_in.value = got
            buf_lbl.text = f"buf: {got} (half {got // 2})"
        else:
            buf_lbl.text = "buf refused: " + " ".join(lines)
        if ok and not state["live"]:
            await one_cycle()   # refresh the charts immediately against the new size
    buf_btn.on_click(apply_buf)

    # ---- acquisition: configure, start/keep the chain, grab, repeat ----
    def current_acq_cfg():
        mode = input_mode_sel.value or "test"
        ksps = int(rate_in.value or 8000)
        if mode == "test":
            return dict(mode="test", ksps=ksps, core=5, pinsel=3, samc=0)
        return dict(mode="custom", ksps=ksps, core=int(core_sel.value),
                    pinsel=int(input_in.value or 0), samc=int(samc_in.value or 0))

    def sync_fake_signal(t):
        if isinstance(t, FakeTarget):
            t.signal_khz = float(sig_in.value or 100.0)
            t.amplitude = float(amp_in.value or 0.0)
            t.noise_std = float(noise_in.value or 0.0)
            t.harm2_amp = float(harm2_in.value or 0.0)
            t.harm3_amp = float(harm3_in.value or 0.0)

    def cycle_failed(text, stop_live=True):
        cyc_lbl.text = text
        cyc_lbl.classes(replace="text-red-400 mono")
        if stop_live and state["live"]:
            state["live"] = False
            live_btn.text, live_btn.icon = "live", "play_arrow"
            single_btn.enable()

    async def ensure_streaming():
        """Send 'stream on ...' only when the chain is not already running
        at this rate/input - and resend it (which reconfigures cleanly:
        chain_stream_on_input() calls chain_stream_off() first) as soon as
        the rate or the input changes."""
        t = state["target"]
        if not t:
            return False
        cfg = current_acq_cfg()
        if state["acq_active"] == cfg:
            return True
        cmd = (f"stream on {cfg['ksps']}" if cfg["mode"] == "test"
               else f"stream on {cfg['ksps']} {cfg['core']} {cfg['pinsel']} {cfg['samc']}")
        async with port_lock:
            ok, lines = await run.io_bound(t.cmd, cmd)
        if not ok:
            cycle_failed("stream on refused: " + " ".join(lines))
            return False
        state["acq_active"] = cfg
        state["grabs"] = 0
        state["live_t0"] = None          # set at the first grab, see one_cycle()
        if cfg["mode"] == "custom":
            await apply_active_dacs()
        return True

    async def one_cycle():
        t = state["target"]
        if not t or state["busy"]:
            return
        state["busy"] = True
        try:
            sync_fake_signal(t)
            if not await ensure_streaming():
                return
            async with port_lock:
                ok, samples, meta = await run.io_bound(t.grab)
            if not ok:
                cycle_failed("grab failed: " + meta.get("error", "unknown"))
                state["acq_active"] = None        # the board says it is not streaming any more
                return
            state["cycles"] += 1
            state["grabs"] += 1
            fs = meta["ksps"] * 1e3
            f, db = spectrum(samples, fs)

            n_samp = max(len(samples) - 1, 1)
            duration_s = n_samp / fs if fs > 0 else 1.0
            t_scale, t_name = ((1e6, "time (µs)") if duration_s < 1e-3 else
                               (1e3, "time (ms)") if duration_s < 1.0 else (1.0, "time (s)"))
            time_chart.options["series"][0]["data"] = [[i, int(v)] for i, v in enumerate(samples)]
            time_chart.options["xAxis"][0]["max"] = n_samp
            time_chart.options["xAxis"][1]["max"] = duration_s * t_scale
            time_chart.options["xAxis"][1]["name"] = t_name
            time_axis["per_sample"] = t_scale / fs if fs > 0 else 0.0
            time_axis["unit"] = t_name.split("(")[-1].rstrip(")")
            update_time_tooltip()                 # also does time_chart.update()
            fft_chart.options["series"][0]["data"] = [[float(fx) / 1e3, float(d)] for fx, d in zip(f, db)]
            fft_chart.options["xAxis"][0]["max"] = float(f[-1]) / 1e3 if len(f) else 1
            fft_chart.update()

            # Grabs per second between the first grab and this one, while
            # LIVE runs only: counted from 'stream on' a SINGLE (one grab a
            # few ms after the start) showed a meaningless 50+ grabs/s.
            now = time.time()
            if state["grabs"] == 1 or not state["live_t0"]:
                state["live_t0"] = now
            rate_txt = ""
            if state["live"] and state["grabs"] >= 2 and now > state["live_t0"]:
                rate_txt = f"   {(state['grabs'] - 1) / (now - state['live_t0']):.2f} grabs/s"
            cyc_lbl.classes(replace="text-slate-300 mono")
            cyc_lbl.text = (f"grab {state['cycles']}   n={len(samples)}   from={meta['from_']}   "
                            f"{meta['ksps']} kSPS actual{rate_txt}")
            for k in ("overrun", "late", "missed"):
                set_chip(k, meta[k])
            rate_chip.text = f"actual rate {meta['ksps']} kSPS"
            rate_chip.props("color=grey-8")
            halves_chip.text = f"halves {meta['halves']} / xfer {meta['transfers']}"
            halves_chip.props("color=grey-8")

            metrics = analyze_spectrum(f, db)
            if metrics:
                eval_chips["fundamental"].text = f"fundamental {metrics['fund_freq']/1e3:.2f} kHz"
                eval_chips["level"].text = f"level {metrics['fund_db']:.1f} dBFS"
                eval_chips["noise floor"].text = f"noise floor {metrics['noise_db']:.1f} dBFS"
                eval_chips["SNR"].text = f"SNR {metrics['snr_db']:.1f} dB"
                eval_chips["THD"].text = f"THD {metrics['thd_pct']:.2f} %"
                eval_chips["SNR"].props(f'color={"positive" if metrics["snr_db"] >= 40 else "negative"}')
                harmonics = {h["k"]: h for h in metrics["harmonics"]}
                for k in (2, 3, 4, 5):
                    chip = eval_chips[f"H{k}"]
                    if k in harmonics:
                        chip.text = f"H{k}  {harmonics[k]['rel_db']:.1f} dBc"
                    else:
                        chip.text = f"H{k} –"
                    chip.props("color=grey-8")

            if meta["slpdat"] > 0:
                triangle_card.set_visibility(True)
                r = chain_tri_eval([int(v) for v in samples])
                verdict = chain_grid_ok(r)
                triangle_verdict_chip.text = "PASS" if verdict else "FAIL"
                triangle_verdict_chip.props(f'color={"positive" if verdict else "negative"}')
                triangle_chips["turning points"].text = f"turning points {r['tps']}"
                triangle_chips["up"].text = f"up {r['n_up']} · {r['l_up']:.2f} smp"
                triangle_chips["down"].text = f"down {r['n_dn']} · {r['l_dn']:.2f} smp"
                triangle_chips["slip"].text = f"slip {r['slip']:.2f} (k={r['slip_k']}, {r['slip_n']} spans)"
                model = chain_model_slope_samples(meta["slpdat"], meta["dac_hz"], meta["ksps"])
                mean_len = (r["l_up"] + r["l_dn"]) / 2.0 if (r["n_up"] or r["n_dn"]) else 0.0
                triangle_chips["model"].text = (f"slope/model {mean_len / model:.3f}"
                                                if model > 0 and mean_len > 0 else "slope/model –")
                triangle_chips["steps"].text = (f"zero {r['zero']} dbl {r['dbl']}" if r["step_checked"]
                                                else "steps not checked (slope < 40 LSB/sample)")
                triangle_chips["steps"].props(
                    f'color={"positive" if (not r["step_checked"]) or (r["zero"] == 0 and r["dbl"] == 0) else "negative"}')
            else:
                triangle_card.set_visibility(False)
        except Exception as ex:
            cycle_failed(f"cycle failed: {ex}")
            state["acq_active"] = None
        finally:
            state["busy"] = False

    async def stop_stream():
        t = state["target"]
        if t:
            async with port_lock:
                await run.io_bound(t.cmd, "stream off")
        state["acq_active"] = None

    async def do_single():
        """One grab: if nothing is streaming yet, start it, grab once, and
        stop it again - LIVE leaves the chain running between grabs, SINGLE
        does not. Disabled while LIVE is running (see the live_btn/
        single_btn enable/disable pairing below)."""
        if state["live"] or not state["target"]:
            return
        pre_active = state["acq_active"] is not None
        await one_cycle()
        if not pre_active:
            await stop_stream()
    single_btn.on_click(do_single)

    async def live_loop():
        while state["live"]:
            await one_cycle()
            await asyncio.sleep(max(0.05, float(interval_in.value or 500) / 1000.0))

    def toggle_live():
        if state["live"]:
            state["live"] = False
            live_btn.text, live_btn.icon = "live", "play_arrow"
            single_btn.enable()
            asyncio.create_task(stop_stream())
        else:
            if not state["target"]:
                return
            state["live"] = True
            live_btn.text, live_btn.icon = "stop", "stop"
            single_btn.disable()
            asyncio.create_task(live_loop())
    live_btn.on_click(toggle_live)

    if args.fake or args.port:
        ui.timer(0.5, do_connect, once=True)

    ui.run(title="ADC/DMA capture", port=args.http_port, show=not args.no_browser, reload=False, dark=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="COM port of the board's console")
    ap.add_argument("--fake", action="store_true", help="use the built-in stand-in instead of a board")
    ap.add_argument("--fake-board", default="EV74H48A", choices=["EV74H48A", "EV17P63A"],
                    help="which board the stand-in reports (its 'version' reply), for trying the "
                         "Curiosity Nano profile without one")
    ap.add_argument("--selftest", action="store_true", help="fake target through one cycle, no GUI")
    ap.add_argument("--settings", default=SETTINGS_FILE,
                    help="settings file, read at start and written by 'save' "
                         f"(default: {os.path.basename(SETTINGS_FILE)} next to this script)")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--no-browser", action="store_true", help="do not open a browser window")
    args = ap.parse_args()
    if args.selftest:
        sys.exit(selftest())
    main_gui(args)


if __name__ in {"__main__", "__mp_main__"}:
    main()
