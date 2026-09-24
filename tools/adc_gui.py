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
  --settings F  settings file, read at start-up and written by "save"
                (default: adc_gui_settings.json next to this script). Every
                control on the page is in it, so a session survives a
                restart; "save as" and "load as" name a different file.

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

# The sample rate
#
# Back-to-back only, since the board answered: neither the ADC's repeat
# timer nor SCCP1 paces a burst, and the CLKGEN6 divider does not change
# the rate either (HARDWARE-LOG runs 5 to 10). What does change it is the
# ADC clock from PLL1 - cli.c's `pll <p1> <p2>`, PLL1 VCO / (p1 * p2) -
# and back-to-back needs 8 ADC clocks per conversion: a conversion is
# 2 TAD and TAD is 4 / f_adc (adc.c). Run 13 measured 3990 kSPS against a
# nominal 4081, 2.2 % out, so the formula holds on silicon.
PLL_INPUT_HZ = 8e6          # the FRC feeding PLL1
PLL_FBDIV_DEFAULT = 200     # PLL1DIV.PLLFBDIV as clock_init() leaves it
PLL_VCO_HZ = PLL_INPUT_HZ * PLL_FBDIV_DEFAULT
ADC_CLOCKS_PER_SAMPLE = 8
PLL_POSTDIV_MIN, PLL_POSTDIV_MAX = 1, 7


def adc_clock_hz(postdiv1: int, postdiv2: int, fbdiv: int = PLL_FBDIV_DEFAULT) -> float:
    """The ADC input clock: 8 MHz * PLLFBDIV / (POSTDIV1 * POSTDIV2).

    The post-dividers alone give only 21 rates and the steps near the top
    are 14 to 17 % apart. PLLFBDIV is the fine adjustment the firmware's
    'rate' command uses, so the clock is no longer a function of the two
    post-dividers alone - read it back from 'status' rather than assuming
    the boot value."""
    d = max(1, int(postdiv1)) * max(1, int(postdiv2))
    return PLL_INPUT_HZ * max(1, int(fbdiv)) / d


def rate_ksps(postdiv1: int, postdiv2: int, fbdiv: int = PLL_FBDIV_DEFAULT) -> float:
    """Nominal sample rate for a PLL setting, in kSPS."""
    return adc_clock_hz(postdiv1, postdiv2, fbdiv) / ADC_CLOCKS_PER_SAMPLE / 1e3


def rate_options(step_pct: float = 2.0, lo_ksps: float = 4000.0, hi_ksps: float = 40000.0):
    """The rates the firmware's 'rate' command can actually hit, thinned
    out to about `step_pct` apart so a dropdown stays usable. Mirrors the
    search in clock_adc_set_rate(): rate = PLLFBDIV / (p1*p2) MSPS with
    PLLFBDIV 63..200 (the VCO limits) and p1 >= p2."""
    seen = []
    for p1 in range(PLL_POSTDIV_MIN, PLL_POSTDIV_MAX + 1):
        for p2 in range(PLL_POSTDIV_MIN, p1 + 1):
            p = p1 * p2
            for fb in range(63, 201):
                r = fb * 1000.0 / p
                if lo_ksps <= r <= hi_ksps:
                    seen.append(round(r, 3))
    seen = sorted(set(seen))
    out, last = [], 0.0
    for r in seen:
        if last == 0.0 or (r - last) / last * 100.0 >= step_pct:
            out.append(r)
            last = r
    return out


def pll_options():
    """Every (p1, p2) the firmware takes, best rate first, deduplicated by
    the rate they produce. cli.c requires p1 >= p2."""
    seen, out = set(), []
    for p1 in range(PLL_POSTDIV_MIN, PLL_POSTDIV_MAX + 1):
        for p2 in range(PLL_POSTDIV_MIN, p1 + 1):
            r = round(rate_ksps(p1, p2), 3)
            if r in seen:
                continue
            seen.add(r)
            out.append((p1, p2, r))
    return sorted(out, key=lambda e: -e[2])


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
SETTINGS_VERSION = 1
SETTINGS_DEFAULTS = {
    "version": SETTINGS_VERSION,
    "board": "EV74H48A",
    "view": {"dac_source": 0},
    "adc": {"core": 3, "pinsel": 5, "samc": 0},
    "pll": {"postdiv1": 5, "postdiv2": 1},
    "buffer": {"size": 2048},
    "capture": {"count": 1024, "interval_ms": 500},
    "dac": {
        "1": {"on": False, "low": 0x100, "high": 0xF00, "slpdat": 8},
        "2": {"on": False, "low": 0x100, "high": 0xF00, "slpdat": 8},
    },
    "fake": {"waveform": "sine", "signal_khz": 100.0, "amplitude": 1500.0,
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
DAC_OPTIONS = {0: "no DAC", 1: "DAC1 \u00b7 RA1", 2: "DAC2 \u00b7 RA8"}


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
        out.append(_text(500, cy, f'DAC{dac_unit} ({"on" if dac_on else "off"}) = pin {dpin.n} \u00b7 {dpin.port}',
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
DAC_CLK_HZ = 320e6


def dac_period_ns_of(low: int, high: int, slp: int) -> float:
    """Mirrors dac.c's dac_period_ns(): two slopes of (high-low)*16 DAC
    clocks each, so a full triangle period is (high-low)*32 DAC clocks."""
    if slp <= 0 or high <= low:
        return 0.0
    clocks = (high - low) * 32
    return clocks * 1e9 / (DAC_CLK_HZ * slp)


# ---------------------------------------------------------------------------
# Binary block transfer ('blk'), see docs/PLAN-BINARY-TRANSFER.md.
# Frame: "BIN n=<count> pace=<> per=<> samc=<> in=<>\r\n" + 2*count bytes
# (uint16 LE, 12 bit) + "\r\nCRC <hex4>\r\n" + the usual prompt and ACK/NAK.
# ---------------------------------------------------------------------------
_BIN_HEADER_RE = re.compile(r"BIN n=(\d+) p1=(\d+) p2=(\d+) samc=(\d+) in=(\d+)")
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


def parse_blk_frame(header_line: str, payload: bytes, tail: bytes):
    """Decode one 'blk' frame from its three pieces (header text line, the
    payload bytes, everything after the payload up to and including the
    ACK/NAK). Used by both Target.blk() (read from serial) and
    FakeTarget.blk() (built in memory), so the same check runs against a real
    board and against the stand-in. Returns (ok, samples, meta); on any
    problem meta['error'] is set and ok is False."""
    m = _BIN_HEADER_RE.match(header_line.strip())
    if not m:
        raise RuntimeError(f"blk: no BIN header, got {header_line!r}")
    n = int(m.group(1))
    meta = dict(postdiv1=int(m.group(2)), postdiv2=int(m.group(3)),
                samc=int(m.group(4)), pinsel=int(m.group(5)))
    m2 = _CRC_LINE_RE.search(tail)
    if not m2:
        raise RuntimeError(f"blk: no CRC line, got {tail!r}")
    crc_frame = int(m2.group(1), 16)
    crc_calc = crc16_ccitt_false(payload)
    samples = (np.frombuffer(payload, dtype="<u2").astype(int) & 0x0FFF) if n else np.zeros(0, dtype=int)
    ok = n > 0 and len(payload) == 2 * n and crc_frame == crc_calc and tail.endswith(ACK)
    if n == 0:
        meta["error"] = "NAK: block not produced"
    elif len(payload) != 2 * n:
        meta["error"] = f"short block: got {len(payload)} of {2 * n} bytes"
    elif crc_frame != crc_calc:
        meta["error"] = f"CRC mismatch: frame {crc_frame:04X}, computed {crc_calc:04X}"
    elif not tail.endswith(ACK):
        meta["error"] = "NAK after block"
    return ok, samples, meta


def probe_blk(target) -> bool:
    """Does this target understand 'blk'? Ask 'help' once, as the plan says,
    instead of trying 'blk' itself and guessing at a NAK's cause."""
    try:
        ok, lines = target.cmd("help")
    except Exception:
        return False
    return ok and any("blk" in l for l in lines)


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
        raise TimeoutError(f"blk: no line within {timeout} s, got {buf!r}")

    def _read_exact(self, n: int, timeout: float) -> bytes:
        buf = b""
        t0 = time.time()
        while len(buf) < n and time.time() - t0 < timeout:
            chunk = self.ser.read(n - len(buf))
            if chunk:
                buf += chunk
        if len(buf) < n:
            raise TimeoutError(f"blk: expected {n} bytes, got {len(buf)} within {timeout} s")
        return buf

    def blk(self, count: int, timeout: float = 10.0):
        """'blk <n>': a contiguous binary block. Reads the header first to
        learn the byte count before looking for ACK/NAK -- a sample byte can
        equal 0x06 or 0x15 by chance, so the generic ACK/NAK scan in
        _read_until_ready() must not run over the payload. See
        docs/PLAN-BINARY-TRANSFER.md. Only the ASCII framing is logged to the
        console transcript (on_log); the sample bytes themselves are not."""
        self._log(f"> blk {count}")
        self.ser.reset_input_buffer()
        self.ser.write(f"blk {count}\r".encode("ascii"))
        echo = self._read_line(timeout)
        self._log(f"< {echo.rstrip()}")
        header_line = self._read_line(timeout)
        self._log(f"< {header_line.rstrip()}")
        m = _BIN_HEADER_RE.match(header_line.strip())
        if not m:
            raise RuntimeError(f"blk: unsupported or no BIN header, got {header_line!r}")
        n = int(m.group(1))
        payload = self._read_exact(2 * n, timeout) if n else b""
        if payload:
            self._log(f"< [binary payload, {len(payload)} bytes, not shown]")
        tail = self._read_until_ready(timeout)  # "...\r\nCRC xxxx\r\n> " + ACK/NAK
        ok = tail.endswith(ACK)
        tail_text = tail[:-1].decode("ascii", "replace") if tail else ""
        for l in tail_text.split("\n"):
            if l.strip():
                self._log(f"< {l.rstrip()}")
        self._log(f"< {'[ACK]' if ok else '[NAK]'}")
        return parse_blk_frame(header_line, payload, tail)


class FakeTarget:
    """Answers like cli.c would, delivers a synthetic signal at the configured
    rate: a sine at `signal_khz` with a small second harmonic and noise,
    12-bit around mid scale. Used with --fake and --selftest."""

    def __init__(self, signal_khz: float = 100.0, amplitude: float = 1500.0,
                 noise_std: float = 6.0, harm2_amp: float = 150.0, harm3_amp: float = 0.0,
                 waveform: str = "sine", on_log=None):
        self.on_log = on_log  # optional callable(str): the console transcript
        self.port = "fake"
        self.pll1, self.pll2 = 5, 1              # 320 MHz ADC clock = 40 MSPS
        self.fbdiv = PLL_FBDIV_DEFAULT           # the "rate" command moves this too
        self.core = 3                            # ADC core 1..5, 'core'; 3+PINSEL 5 = mikroBUS A default
        self.samc, self.input = 0, 5
        # Both DACs, as dac.c has them: unit -> its triangle settings.
        # The stand-in's ADC sees whichever one is on (DAC2 first, the same
        # order dac_active() uses in the firmware).
        self.dac = {1: {"on": False, "low": 0x100, "high": 0xF00, "slp": 8},
                    2: {"on": False, "low": 0x100, "high": 0xF00, "slp": 8}}
        self.running = False
        self.signal_khz = signal_khz
        self.amplitude = amplitude               # fundamental (or triangle) peak, ADC counts
        self.noise_std = noise_std               # noise, ADC counts (sets the SNR)
        self.harm2_amp = harm2_amp               # 2nd harmonic peak, ADC counts (sine only)
        self.harm3_amp = harm3_amp               # 3rd harmonic peak, ADC counts (sine only)
        self.waveform = waveform                 # "sine" (piezo-like) or "triangle" (DAC2 -> ADC5 self-test)
        self.clkdiv = 100                        # CLKGEN6 ratio x 100, does not set the rate
        self.buf_size = 2048                     # total ping-pong buffer, 'buf'
        self.t0 = None                            # wall-clock anchor, lazy
        self.counters = dict(overrun=0, late=0, missed=0, addr_err=0, bus_err=0, blocks=0)
        self.rng = np.random.default_rng(1)

    def close(self):
        pass

    def _log(self, line: str):
        if self.on_log:
            try:
                self.on_log(line)
            except Exception:
                pass

    def _fs_hz(self) -> float:
        """The rate _samples() actually generates at, scaled by the
        configured CLKGEN6 divider like the real repeat timer is (adc.c:
        TAD = 4/Fadc). Real hardware in back-to-back mode has no documented
        rate: back-to-back at the ADC clock, 8 clocks per conversion,
        the same formula the firmware uses since the rate became the PLL's
        job alone (cli.c 'pll')."""
        return rate_ksps(self.pll1, self.pll2, self.fbdiv) * 1e3

    def _samples(self, n: int) -> np.ndarray:
        """A window of the last `n` samples of an ongoing background signal,
        as of *now* -- not simply the next `n` samples after the previous
        call. Models the real board: the piezo signal runs continuously, but
        115200 baud is far too slow to carry a real stream, so every dump or
        blk only ever shows a snippet of wherever the signal is at the
        moment it is asked for. Two calls close together in wall-clock time
        overlap almost completely; calls far apart show it having moved on."""
        fs = self._fs_hz()
        if self.t0 is None:
            self.t0 = time.time()
        end_idx = (time.time() - self.t0) * fs
        start_idx = max(end_idx - n, 0.0)
        t = (start_idx + np.arange(n)) / fs
        f = self.signal_khz * 1e3
        if self.waveform == "triangle":
            # DAC2 Triangle Wave mode looped back onto ADC5 (dac.c/dactest.c):
            # a pure triangle, odd harmonics only -- arcsin(sin(x)) is the
            # standard closed form, no modulo edge cases. Driven by the DAC's
            # own settings ('dac'), not the sine controls: DAC and ADC codes
            # share the same 12-bit domain, so DAC low/high map ~1:1 to the
            # ADC counts ADC5 would read back.
            d = self.dac_active()
            if d and d["high"] > d["low"] and d["slp"] > 0:
                period_ns = dac_period_ns_of(d["low"], d["high"], d["slp"])
                f_dac = 1e9 / period_ns if period_ns > 0 else 0.0
                mid = (d["high"] + d["low"]) / 2.0
                amp = (d["high"] - d["low"]) / 2.0
                tri = (2.0 / np.pi) * np.arcsin(np.sin(2 * np.pi * f_dac * t))
                v = mid + amp * tri
            else:
                v = np.full(n, 2048.0)  # DAC off: DACOUT2/AD5AN3 floats, no defined level
        else:
            v = (2048 + self.amplitude * np.sin(2 * np.pi * f * t)
                 + self.harm2_amp * np.sin(2 * np.pi * 2 * f * t + 0.7)
                 + self.harm3_amp * np.sin(2 * np.pi * 3 * f * t + 1.3))
        v += self.rng.normal(0, self.noise_std, n)
        if self.input == 6:                       # the internal reference
            v = np.full(n, 3840.0) + self.rng.normal(0, 2, n)
        return np.clip(np.round(v), 0, 4095).astype(int)

    def dac_active(self):
        """The DAC the stand-in's ADC sees: DAC2 first, the order
        dac_active() uses in dac.c, then DAC1, else None."""
        for unit in (2, 1):
            if self.dac[unit]["on"]:
                return self.dac[unit]
        return None

    def dac_active_or(self, unit):
        return self.dac_active() or self.dac[unit]

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
        try:
            if c == "start":
                self.running = True;  return True, ["running: 1"]
            if c == "stop":
                self.running = False; return True, ["running: 0"]
            if c == "pll":
                if len(args) < 2:
                    return False, ["usage: pll <postdiv1 1..7> <postdiv2 1..7>"]
                p1, p2 = int(args[0]), int(args[1])
                if not (PLL_POSTDIV_MIN <= p2 <= p1 <= PLL_POSTDIV_MAX):
                    return False, ["usage: pll <postdiv1 1..7> <postdiv2 1..7>, p1 >= p2"]
                self.pll1, self.pll2 = p1, p2
                return True, [f"postdiv1: {p1}", f"postdiv2: {p2}",
                              f"adc clock Hz: {int(adc_clock_hz(p1, p2))}",
                              f"ksps nominal: {int(rate_ksps(p1, p2))}"]
            if c == "samc":
                self.samc = int(args[0]);  return True, [f"samc: {self.samc}"]
            if c == "input":
                self.input = int(args[0]); return True, [f"input: {self.input}"]
            if c == "core":
                if not args:
                    return False, ["usage: core <1..5> [pinsel]"]
                core = int(args[0])
                pinsel = int(args[1]) if len(args) > 1 else self.input
                if not (1 <= core <= 5) or not (0 <= pinsel <= 15):
                    return False, ["usage: core <1..5> [pinsel]"]
                self.core, self.input = core, pinsel
                return True, [f"core: {core}", f"input: {pinsel}"]
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
            if c == "clk":
                # The firmware keeps this one, and says outright that it
                # does NOT change the rate (run 10: the ADC ignores the
                # CLKGEN6 divider). The stand-in behaves the same way.
                if not args:
                    return False, ["usage: clk <100..1000> - CLKGEN6 ratio x 100"]
                n = int(args[0])
                if not (100 <= n <= 1000):
                    return False, ["usage: clk <100..1000>"]
                self.clkdiv = n
                return True, [f"ratio asked for: {n}", f"ratio read back: {n}",
                              f"adc clock Hz: {int(adc_clock_hz(self.pll1, self.pll2, self.fbdiv))}",
                              "ok (and the rate does not change)"]
            if c == "status":
                # 'start' streams without anyone counting bursts; keep the
                # two in step so the ratio stays the 1:2 a sound chain has.
                self.counters["blocks"] += 34
                self.counters["bursts"] = self.counters.get("bursts", 0) + 17
                return True, [f"running: {int(self.running)}", f"blocks: {self.counters['blocks']}", f"bursts: {self.counters.get('bursts', 0)}",
                              "overrun: 0", "late: 0", "missed: 0", "addr_err: 0", "bus_err: 0",
                              f"core: {self.core}", f"input: {self.input}", f"samc: {self.samc}",
                              f"postdiv1: {self.pll1}", f"postdiv2: {self.pll2}",
                              f"adc clock Hz: {int(adc_clock_hz(self.pll1, self.pll2, self.fbdiv))}",
                              f"ksps nominal: {int(rate_ksps(self.pll1, self.pll2, self.fbdiv))}",
                              f"pll1 fbdiv: {self.fbdiv}",
                              f"fs_hz: {round(self._fs_hz())}",
                              f"buf: {self.buf_size}", f"half: {self.buf_size // 2}",
                              f"clkdiv (x100, read back): {self.clkdiv}",
                              f"dac1_on: {int(self.dac[1]['on'])}", f"dac2_on: {int(self.dac[2]['on'])}",
                              f"dac_low: {self.dac_active_or(1)['low']}",
                              f"dac_high: {self.dac_active_or(1)['high']}",
                              f"dac_slp: {self.dac_active_or(1)['slp']}",
                              "last: 1798", "selftest_mean: 3840", "fail_code: 0"]
            if c == "version":
                return True, ["adc_dma_40msps (fake target)", "board: none, synthetic signal"]
            if c == "help":
                return True, ["commands: start stop snap rate pll clk core samc input dac buf status version dump blk"]
            if c == "snap":
                # One buffer, and the DMA interrupt ends the stream. The
                # window that follows is contiguous; that is the whole
                # point of the command (see capture_cycle).
                n = self.buf_size
                fs = self._fs_hz()
                window_ns = int(n / fs * 1e9) if fs > 0 else 0
                self.running = False
                # One burst is one buffer: two halves, so two blocks. The
                # stand-in models a chain without the double-booking the
                # board is suspected of, which is what makes it the
                # reference the GUI's blocks/bursts chip is read against.
                self.counters["bursts"] = self.counters.get("bursts", 0) + 1
                self.counters["blocks"] = self.counters.get("bursts", 0) * 2
                return True, [f"samples: {n}", f"window ns: {window_ns}",
                              f"ksps measured: {int(fs / 1e3)}",
                              f"ksps nominal: {int(rate_ksps(self.pll1, self.pll2, self.fbdiv))}",
                              "overrun during the burst: 0",
                              f"input: {self.input}", f"adc core: {self.core}"]
            if c == "rate":
                # Mirrors clock_adc_set_rate(): the closest PLLFBDIV and
                # post-divider pair, not a free number.
                if not args:
                    return False, ["usage: rate <4000..40000 ksps>"]
                want = int(args[0])
                if not (4000 <= want <= 40000):
                    return False, ["usage: rate <4000..40000 ksps>"]
                best = None
                for p1 in range(1, 8):
                    for p2 in range(1, p1 + 1):
                        pp = p1 * p2
                        fb = (want * pp + 500) // 1000
                        if not (63 <= fb <= 200):
                            continue
                        got = fb * 1000 // pp
                        if not (4000 <= got <= 40000):
                            continue
                        d = abs(got - want)
                        if best is None or d < best[0]:
                            best = (d, p1, p2, fb, got)
                if best is None:
                    return False, ["rate: nothing reachable"]
                _, self.pll1, self.pll2, self.fbdiv, got = best
                return True, [f"ksps asked for: {want}", f"ksps set: {got}",
                              f"pll1 fbdiv: {self.fbdiv}",
                              f"pll1 postdiv1: {self.pll1}", f"pll1 postdiv2: {self.pll2}",
                              f"adc clock Hz: {int(adc_clock_hz(self.pll1, self.pll2, self.fbdiv))}",
                              "the configuration arrived"]
            if c == "dump":
                total = self.buf_size
                count = int(args[0]) if args else 64
                offset = int(args[1]) if len(args) > 1 else 0
                if not (1 <= count <= total) or not (0 <= offset <= total - 1):
                    return False, [f"usage: dump [count 1..{total}] [offset 0..{total - 1}]"]
                count = min(count, total - offset)
                v = self._samples(count)
                lines = []
                for i in range(0, count, 8):
                    lines.append(f"{offset + i:04d}:" + "".join(f" {x}" for x in v[i:i + 8]))
                return True, lines
        except (ValueError, IndexError):
            return False, ["usage error"]
        return False, ["unknown command"]

    def blk(self, count: int, timeout: float = 10.0, corrupt_payload: bool = False):
        """Builds the identical frame bytes a board would send, then decodes
        them with parse_blk_frame() -- the exact function Target.blk() uses
        for a real board -- so the wire format is exercised end to end
        without hardware. `corrupt_payload` flips a byte after the CRC is
        computed, to prove a damaged block is reported, not accepted. Only
        the ASCII framing goes to the console transcript (on_log); the
        sample bytes themselves are not logged."""
        self._log(f"> blk {count}")
        n = count if 1 <= count <= self.buf_size else 0
        header_line = (f"BIN n={n} p1={self.pll1} p2={self.pll2} "
                        f"samc={self.samc} in={self.input}\r\n")
        self._log(f"< {header_line.rstrip()}")
        payload = self._samples(n).astype("<u2").tobytes() if n else b""
        if payload:
            self._log(f"< [binary payload, {len(payload)} bytes, not shown]")
        crc = crc16_ccitt_false(payload)
        if corrupt_payload and payload:
            payload = bytes([payload[0] ^ 0xFF]) + payload[1:]
        self._log(f"< CRC {crc:04X}")
        ack = ACK if n else NAK
        self._log(f"< {'[ACK]' if ack == ACK else '[NAK]'}")
        tail = f"\r\nCRC {crc:04X}\r\n> ".encode("ascii") + ack
        return parse_blk_frame(header_line, payload, tail)


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
    """The firmware prints "key: value" pairs, and since the back-to-back
    rework the keys carry digits, spaces and a parenthesised note
    ("postdiv1: 5", "ksps nominal: 40000", "clkdiv (x100, read back): 100").
    Normalise each to snake_case without the note, so a lookup is stable:
    "ksps nominal" -> ksps_nominal, "clkdiv (x100, read back)" -> clkdiv."""
    d = {}
    for l in lines:
        m = re.match(r"\s*([A-Za-z][\w ()/,.-]*?)\s*:\s*(-?\d+)\s*$", l)
        if not m:
            continue
        key = re.sub(r"\(.*?\)", "", m.group(1))
        key = re.sub(r"[^A-Za-z0-9]+", "_", key).strip("_").lower()
        if key:
            d[key] = int(m.group(2))
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


# ---------------------------------------------------------------------------
# One capture cycle: start, run a moment, stop, dump, status
# ---------------------------------------------------------------------------
def capture_cycle(target, count: int, settle_s: float = 0.02, blk: bool = False):
    """One capture: 'snap' fills the buffer exactly once and the DMA
    interrupt itself ends the stream, then 'dump' reads the window out.

    It does NOT do start / wait / stop / dump any more, and that is not a
    style question. At the rates this board runs, the main loop is tens of
    milliseconds behind the DMA: the half being read has been overwritten
    a thousand times in the meantime and the result is a mixture of old
    and new data - 8552 halves missed between eight copies in run 11, with
    single steps of 3126 counts in what should have been a smooth ramp. A
    spectrum of that is meaningless and looks perfectly plausible. On top
    of it, at a rate with overruns the 'stop' never arrives at all,
    because the console's receive interrupt sits below the DMA channel in
    priority and never gets the CPU.

    After 'snap' the whole buffer stands still and nothing is writing it,
    so the window is contiguous by construction. `settle_s` is kept for
    call compatibility and is no longer used."""
    del settle_s
    if blk:
        ok, samples, meta = target.blk(count)
        if not ok:
            raise RuntimeError("blk refused: " + meta.get("error", "unknown"))
        ok, st = target.cmd("status")
        return samples, parse_status(st) if ok else {}
    ok, snap_lines = target.cmd("snap", timeout=10.0)
    if not ok:
        raise RuntimeError("snap refused: " + " ".join(snap_lines))
    ok, lines = target.cmd(f"dump {count} 0", timeout=20.0)
    if not ok:
        raise RuntimeError("dump refused: " + " ".join(lines))
    samples = parse_dump(lines)
    ok, st = target.cmd("status")
    status = parse_status(st) if ok else {}
    # 'snap' measures the window with Timer1, so it knows the delivered
    # rate better than any nominal figure, and it also reports the
    # overruns of that one burst. Carry all of it: none of its keys
    # collide with 'status', which owns blocks and bursts.
    status.update(parse_status(snap_lines))
    return samples, status


def selftest() -> int:
    ok_all = True
    t = FakeTarget(signal_khz=250.0)
    for c in ("pll 5 1", "samc 0", "input 5"):
        ok, r = t.cmd(c)
        assert ok, (c, r)
    fs = rate_ksps(5, 1) * 1e3

    samples, status = capture_cycle(t, 1024, settle_s=0.0, blk=False)
    f, db = spectrum(samples, fs)
    peak = f[np.argmax(db[1:]) + 1]
    print(f"dump: {len(samples)} samples, min {samples.min()} max {samples.max()} mean {samples.mean():.0f}")
    print(f"fs {fs/1e6:.3f} MHz, FFT peak at {peak/1e3:.1f} kHz (expect 250.0), status keys {sorted(status)[:5]}...")
    ok = len(samples) == 1024 and abs(peak - 250e3) < fs / 1024 and status.get("postdiv1") == 5
    ok_all &= ok
    print("dump selftest", "PASS" if ok else "FAIL")

    # 'blk' path (docs/PLAN-BINARY-TRANSFER.md): probe, full 2048-sample block, CRC.
    assert probe_blk(t), "fake target must advertise 'blk' in help"
    samples, status = capture_cycle(t, 2048, blk=True)
    f, db = spectrum(samples, fs)
    peak = f[np.argmax(db[1:]) + 1]
    print(f"blk: {len(samples)} samples, FFT peak at {peak/1e3:.1f} kHz (expect 250.0)")
    ok = len(samples) == 2048 and abs(peak - 250e3) < fs / 2048
    ok_all &= ok
    print("blk selftest", "PASS" if ok else "FAIL")

    # A damaged block must be reported, never silently accepted.
    ok, _, meta = t.blk(64, corrupt_payload=True)
    ok_corrupt = (not ok) and "CRC mismatch" in meta.get("error", "")
    ok_all &= ok_corrupt
    print("blk corruption caught:", "PASS" if ok_corrupt else "FAIL", "-", meta.get("error"))

    ok_all &= crc16_ccitt_false(b"123456789") == 0x29B1
    print("selftest", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


# ---------------------------------------------------------------------------
# The GUI
# ---------------------------------------------------------------------------
def main_gui(args):
    from nicegui import ui, run

    state = dict(target=None, live=False, busy=False, cycles=0, use_blk=False,
                 buf_size=2048, settings_path=args.settings)

    def update_count_options():
        """The selectable capture sizes ('windows') follow the buffer size:
        never offer more samples than 'buf' currently holds."""
        buf = state["buf_size"]
        opts = sorted({n for n in (64, 128, 256, 512, 1024, 2048, 4096, 8192) if n <= buf} | {buf})
        count_sel.options = opts
        if count_sel.value not in opts:
            count_sel.value = opts[-1]
        count_sel.update()

    # Every control that shows the selection registers itself here, so
    # refresh_channel() can write one change back to all of them.
    board_ctrls, core_ctrls, chan_ctrls, dac_ctrls = [], [], [], []
    dac_ui = {}                       # unit -> its card's controls
    ui_state = {"board": BOARD_DEFAULT, "sync": False, "dac": 0}

    # ---- look: dark, one accent colour, rounded cards ----
    ACCENT, ACCENT2, DIM = "#22d3ee", "#a78bfa", "#94a3b8"
    ui.colors(primary=ACCENT, secondary=ACCENT2, accent=ACCENT, dark="#0b1220", positive="#34d399", negative="#f87171")
    ui.add_head_html("""<style>
      body { background: #0b1220; }
      .q-card { background: #111827 !important; border: 1px solid #1f2937; }
      .q-field__label, .q-field__native, .q-field__control { color: #e5e7eb; }
      .card-title { color: #94a3b8; font-size: 0.75rem; letter-spacing: .12em; text-transform: uppercase; }
      .mono { font-family: ui-monospace, Consolas, monospace; }
    </style>""")

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

    def ports():
        try:
            from serial.tools import list_ports
            return [p.device for p in list_ports.comports()]
        except Exception:
            return []

    # ---- header ----
    with ui.header().classes("items-center gap-4 px-6").style("background: #0f172a; border-bottom: 1px solid #1f2937"):
        ui.icon("show_chart", size="md").classes("text-cyan-400")
        with ui.column().classes("gap-0"):
            ui.label("dsPIC33A ADC / DMA").classes("text-lg font-medium leading-tight")
            ui.label("capture · plot · FFT over the console").classes("text-xs text-slate-400 leading-tight")
        ui.space()
        port_sel = ui.select(options=["fake"] + ports(), value=args.port or ("fake" if args.fake else None),
                             label="port").classes("w-44").props("dense outlined")
        conn_btn = ui.button("connect", icon="usb").props("unelevated")
        conn_chip = ui.chip("not connected", icon="link_off", color="grey-8").props("outline")
        xfer_chip = ui.chip("transfer: -", icon="swap_horiz", color="grey-8").props("outline")

    with ui.row().classes("w-full p-4 gap-4 items-start no-wrap"):
        # ---- left: settings ----
        with ui.column().classes("gap-4").style("width: 22rem; min-width: 22rem"):
            with ui.card().classes("w-full rounded-xl p-4 gap-2"):
                ui.label("settings file").classes("card-title")
                settings_path_lbl = ui.label().classes("text-xs text-slate-400 mono break-all")
                with ui.row().classes("w-full gap-2"):
                    save_btn = ui.button("save", icon="save").props("unelevated dense").classes("flex-grow")
                    save_as_btn = ui.button("save as", icon="save_as").props("outline dense").classes("flex-grow")
                    load_as_btn = ui.button("load as", icon="folder_open").props("outline dense").classes("flex-grow")
                settings_msg_lbl = ui.label().classes("text-xs text-slate-400 mono")
            with ui.card().classes("w-full rounded-xl p-4 gap-2"):
                ui.label("sample rate · PLL1").classes("card-title")
                # The rate is the ADC clock and nothing else: back-to-back
                # conversions at 8 clocks each (cli.c 'pll').
                pll_sel = ui.select({f"{p1},{p2}": f"{r/1000:.3f} MSPS  (p1 {p1}, p2 {p2})"
                                     for p1, p2, r in pll_options()},
                                    value="5,1", label="PLL1 post-dividers").props("dense outlined")
                rate_lbl = ui.label().classes("text-cyan-300 mono")
                ui.label("back-to-back only; the repeat timer, SCCP1 and the CLKGEN6 divider "
                         "were all measured to leave the rate untouched")\
                    .classes("text-xs text-slate-500")
            with ui.card().classes("w-full rounded-xl p-4 gap-2"):
                ui.label("adc core & channel").classes("card-title")
                with ui.row().classes("w-full gap-2"):
                    core_sel = ui.select({n: f"ADC{n}" for n in range(1, 6)}, value=3,
                                         label="core (1..5)").props("dense outlined").classes("flex-grow")
                    input_in = ui.number("PINSEL (0..15, 6 = internal ref)", value=5, min=0, max=15,
                                         step=1, format="%d").props("dense outlined").classes("flex-grow")
                channel_info_lbl = ui.label().classes("text-xs text-slate-400")
                samc_in = ui.number("SAMC · sample time (0..31)", value=0, min=0, max=31, step=1, format="%d").props("dense outlined")
                ui.label("fake target signal only").classes("text-xs text-slate-500")
                waveform_sel = ui.select(
                    {"sine": "sine (piezo-like)", "triangle": "triangle (DAC2 → ADC5 self-test)"},
                    value="sine", label="fake waveform").props("dense outlined")
                waveform_hint_lbl = ui.label().classes("text-xs text-amber-300")
                sig_in = ui.number("fake signal, kHz (sine only)", value=100.0, min=0.1, max=20000.0, step=10).props("dense outlined")
                amp_in = ui.number("amplitude, counts (pk, sine only)", value=1500.0, min=0.0, max=2000.0, step=50).props("dense outlined")
                noise_in = ui.number("noise, counts (std dev, sets SNR)", value=6.0, min=0.0, max=500.0, step=1).props("dense outlined")
                harm2_in = ui.number("2nd harmonic, counts (pk, sine only)", value=150.0, min=0.0, max=1000.0, step=10).props("dense outlined")
                harm3_in = ui.number("3rd harmonic, counts (pk, sine only)", value=0.0, min=0.0, max=1000.0, step=10).props("dense outlined")
                apply_btn = ui.button("apply to board", icon="upload").props("unelevated").classes("w-full")
                apply_lbl = ui.label().classes("text-xs text-slate-400 mono")
            # One card per DAC. Both units are the same hardware with
            # different registers (dac.c), and each has its own output pin:
            # DAC1 on RA1, DAC2 on RA8. Either can feed an ADC channel -
            # its own pin without a wire, any other pin with one, which is
            # what the board tile spells out.
            for _unit, _pin_name in ((1, "RA1"), (2, "RA8")):
                with ui.card().classes("w-full rounded-xl p-4 gap-2"):
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
                    dac_ui[_unit] = {"on": _on, "low": _low, "high": _high, "slp": _slp,
                                     "freq": _freq, "btn": _btn, "msg": _msg}
            with ui.card().classes("w-full rounded-xl p-4 gap-2"):
                ui.label("capture").classes("card-title")
                with ui.row().classes("w-full gap-2 items-end"):
                    buf_in = ui.number("buffer size, total ('buf')", value=2048, min=16, max=8192, step=16,
                                       format="%d").props("dense outlined").classes("flex-grow")
                    buf_btn = ui.button("apply", icon="tune").props("unelevated dense")
                buf_lbl = ui.label("buf: not queried yet").classes("text-xs text-slate-400 mono")
                count_sel = ui.select([64, 128, 256, 512, 1024, 2048], value=1024,
                                      label="samples per capture (limited by buffer size)").props("dense outlined")
                interval_in = ui.number("live interval, ms", value=500, min=100, max=5000, step=100, format="%d").props("dense outlined")
                with ui.row().classes("w-full gap-2"):
                    single_btn = ui.button("single", icon="camera").props("unelevated").classes("flex-grow")
                    live_btn = ui.button("live", icon="play_arrow").props("unelevated").classes("flex-grow")

        # ---- right: results ----
        with ui.column().classes("flex-grow gap-4"):
            with ui.row().classes("w-full items-center gap-2"):
                cyc_lbl = ui.label("no capture yet").classes("text-slate-300 mono")
                ui.space()
                chips = {k: ui.chip(f"{k} –", color="grey-8").props("dense outline")
                         for k in ("overrun", "late", "missed", "addr_err", "bus_err")}
                # A burst is CNT = 2 * half_len conversions, one whole
                # buffer: HALF at the middle, DONE at the end. So blocks
                # must be exactly twice the bursts - arithmetic, not an
                # assumption about the silicon. Anything else means the
                # handler books the same event more than once.
                ratio_chip = ui.chip("blocks/bursts –", color="grey-8").props("dense outline")
                rate_chip = ui.chip("rate –", color="grey-8").props("dense outline")
                with ratio_chip:
                    ui.tooltip("Completed buffer halves against started bursts. One burst fills "
                               "one buffer, so it raises HALF once and DONE once: blocks must be "
                               "exactly 2 x bursts. More than that means an event was counted "
                               "again - the interrupt saw a flag that had not cleared, and that "
                               "grows with the overrun rate while a clean burst stays at 1:2. "
                               "Grey while the firmware does not report 'bursts' in status.")\
                        .style("font-size: 14px; max-width: 24rem;")
                with rate_chip:
                    ui.tooltip("What the burst really delivered, timed with Timer1 by 'snap', "
                               "against the rate the PLL setting asks for. This is the other half "
                               "of the same question: if the counters stay at 1:2 and the rate "
                               "still falls short, the conversions are producing more DMA "
                               "transfers than results - the silicon's trigger fault - rather "
                               "than the handler counting one event twice.")\
                        .style("font-size: 14px; max-width: 24rem;")
            with ui.card().classes("w-full rounded-xl p-2"):
                time_chart = chart("time signal", "sample", "ADC counts", 0, 4096, ACCENT, second_x_name="time")
            with ui.card().classes("w-full rounded-xl p-2"):
                fft_chart = chart("spectrum · Hann window", "kHz", "dBFS", -100, 0, ACCENT2)
            with ui.card().classes("w-full rounded-xl p-3"):
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

            # ---- the package: where the selected channel physically is ----
            # Full width, every pin labelled with its number and port name.
            # The eval kit decides which device, and so which package, is
            # drawn. Kit, core and channel are repeated on the board tile
            # below and on the sidebar; all of them are the same selection.
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

            with ui.card().classes("w-full rounded-xl p-3 gap-2"):
                with ui.row().classes("w-full items-center gap-3 flex-wrap"):
                    ui.label("chip · pinout").classes("card-title")
                    chip_pin_lbl = selection_row("chip")
                chip_html = ui.html("").classes("w-full").style("max-width: 1100px; margin: 0 auto")
                ui.label("Cyan = the selected channel, dim cyan = the other inputs of that core, "
                         "violet = DAC outputs, red = supply, grey = ground. Hover a pin for all "
                         "its functions. Pin tables: data sheet DS70005591D, Table 11 for the "
                         "TQFP-128 and Table 5 for the Nano's 64-pin part.").classes("text-xs text-slate-400")

            # ---- the board: where that pin comes out on the kit ----------
            with ui.card().classes("w-full rounded-xl p-3 gap-2"):
                with ui.row().classes("w-full items-center gap-3 flex-wrap"):
                    ui.label("board · where to wire it").classes("card-title")
                    board_pin_lbl = selection_row("board")
                board_html = ui.html("").classes("w-full").style("max-width: 1100px; margin: 0 auto")
                board_note_lbl = ui.label().classes("text-xs text-slate-400")

    # ---- console: the CLI traffic with the target (real or fake) ----
    with ui.card().classes("w-full rounded-xl p-3 mx-4 mb-4"):
        ui.label("console").classes("card-title")
        console_log = ui.log(max_lines=1000).classes("w-full h-40").style(
            "background: #0b1220; color: #a7f3d0; font-family: ui-monospace, Consolas, monospace; "
            "font-size: 12px; white-space: pre;")

    def push_log(line: str):
        console_log.push(f"{time.strftime('%H:%M:%S')}  {line}")

    def pll_values():
        p1, p2 = (pll_sel.value or "5,1").split(",")
        return int(p1), int(p2)

    def update_rate_label():
        try:
            p1, p2 = pll_values()
            rate_lbl.text = (f"{rate_ksps(p1, p2)/1000:.3f} MSPS nominal   "
                             f"(ADC clock {adc_clock_hz(p1, p2)/1e6:.1f} MHz)")
        except Exception:
            rate_lbl.text = ""
    pll_sel.on_value_change(lambda e: update_rate_label())
    update_rate_label()

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
        (xfer_chip, "How samples are fetched. 'dump' is the text listing every firmware has; "
                    "'blk' is the binary block with a CRC, used when the board offers it - "
                    "same data, far fewer bytes."),
        (save_btn, "Write every setting on this page back to the settings file named above."),
        (save_as_btn, "Write the settings to a file you name, without changing which file the "
                      "page started from."),
        (load_as_btn, "Read settings from a file you name and put them on the page. Keys the "
                      "file does not carry keep their built-in default."),
        (pll_sel, "The sample rate, and the only thing that sets it. The ADC clock is PLL1's "
                  "1600 MHz divided by the two post-dividers, and back-to-back conversion takes "
                  "8 ADC clocks. Console: 'pll <p1> <p2>'. The repeat timer, SCCP1 and the "
                  "CLKGEN6 divider were all measured on the board to leave the rate untouched."),
        (core_sel, "Which of the five ADC cores converts. They are independent and have different "
                   "pins, so this also changes which pins the channel list below can reach. "
                   "Console: 'core <1..5> [pinsel]'."),
        (input_in, "PINSEL: the analog input of that core. 6 is the internal 15/16 x VDD "
                   "reference used by the self-test, 7 the internal UREF line. The chip and board "
                   "tiles show which pin the pair lands on. Console: 'input <0..15>'."),
        (samc_in, "SAMC: how long the ADC samples before it converts, in steps of 2 x SAMC + 0.5 "
                  "TAD. It sets the aperture, not the rate - a high source impedance needs more "
                  "of it. Console: 'samc <0..31>'."),
        (waveform_sel, "What the stand-in target generates. 'sine' uses the three fields below; "
                       "'triangle' mirrors a DAC driving the ADC pin and needs a DAC switched on."),
        (sig_in, "Frequency of the stand-in's sine, in kHz. Only meaningful below half the sample "
                 "rate - above that the FFT shows the alias, which is itself worth seeing."),
        (amp_in, "Amplitude of the stand-in's sine in ADC counts, peak. Full scale is 4096 counts, "
                 "so 2048 is the largest undistorted swing around mid scale."),
        (noise_in, "Gaussian noise the stand-in adds, standard deviation in counts. This is what "
                   "sets the SNR the evaluation row reports."),
        (harm2_in, "Second harmonic the stand-in adds, peak counts. Use it to see what the THD and "
                   "H2 figures do with a known distortion."),
        (harm3_in, "Third harmonic the stand-in adds, peak counts."),
        (apply_btn, "Send the rate, sample time, core and channel above to the board, one command "
                    "at a time. The line underneath reports each one."),
        (buf_in, "Total size of the ping-pong buffer in samples; each half is half of it. The DMA "
                 "fills one half while the CPU works on the other. Console: 'buf <n>'."),
        (buf_btn, "Send the buffer size. The firmware sets the ADC burst length and the DMA block "
                  "to match at the next start."),
        (count_sel, "How many samples one capture fetches. At most one buffer half over 'dump', "
                    "a whole buffer over 'blk'."),
        (interval_in, "How often 'live' repeats the capture cycle, in milliseconds."),
        (single_btn, "One cycle: start, let it run briefly, stop, fetch the samples, plot and "
                     "transform them."),
        (live_btn, "Repeat that cycle at the interval above until stopped."),
    ]
    for _u, _c in sorted(dac_ui.items()):
        _pin = "RA1" if _u == 1 else "RA8"
        TIPS += [
            (_c["on"], f"Switch DAC{_u} on or off. It drives pin {_pin} with a triangle in "
                       "hardware, no CPU involved, and that pin is also an ADC input of core 5 - "
                       "so the ADC can read it back with no wire. Console: "
                       f"'dac {_u} on|off ...'."),
            (_c["low"], "Lower end of the triangle, as a 12-bit DAC code. 0 is ground, 4095 is "
                        "VDD, and the DAC's own limits keep the usable range a little inside that."),
            (_c["high"], "Upper end of the triangle, as a 12-bit DAC code. Must be above the lower "
                         "end. The difference is the swing the ADC should see."),
            (_c["slp"], "SLPDAT: how many DAC codes the slope generator steps per DAC clock. "
                        "Larger is faster, so the period shown below shrinks."),
            (_c["btn"], f"Send these settings to DAC{_u}. The line underneath is the board's own "
                        "answer, including the period it computed."),
        ]
    for _sel in board_ctrls:
        TIPS.append((_sel, "Which evaluation kit is in front of you. It picks the device and so "
                           "the package drawn above, and it decides where a channel comes out: "
                           "the EV74H48A has mikroBUS and XPLAINED PRO headers, the Nano two rows "
                           "of edge pads."))
    for _sel in core_ctrls:
        TIPS.append((_sel, "ADC core, the same selection as in the sidebar. Changing it here "
                           "changes it everywhere and redraws both tiles."))
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
            "view": {"dac_source": int(ui_state["dac"])},
            "adc": {"core": int(core_sel.value), "pinsel": int(input_in.value or 0),
                    "samc": int(samc_in.value or 0)},
            "pll": {"postdiv1": pll_values()[0], "postdiv2": pll_values()[1]},
            "buffer": {"size": int(buf_in.value or 2048)},
            "capture": {"count": int(count_sel.value), "interval_ms": int(interval_in.value or 500)},
            "dac": {str(u): {"on": bool(c["on"].value), "low": int(c["low"].value or 0),
                             "high": int(c["high"].value or 0), "slpdat": int(c["slp"].value or 0)}
                    for u, c in dac_ui.items()},
            "fake": {"waveform": waveform_sel.value, "signal_khz": float(sig_in.value or 0.0),
                     "amplitude": float(amp_in.value or 0.0), "noise": float(noise_in.value or 0.0),
                     "harmonic2": float(harm2_in.value or 0.0),
                     "harmonic3": float(harm3_in.value or 0.0)},
        }

    def settings_apply(cfg):
        """Write a settings dict onto the controls. Anything out of range
        for the current build is skipped rather than forced."""
        if cfg.get("board") in BOARDS:
            ui_state["board"] = cfg["board"]
        ui_state["dac"] = int(cfg.get("view", {}).get("dac_source", 0))
        adc = cfg.get("adc", {})
        core_sel.value = int(adc.get("core", 3))
        input_in.value = int(adc.get("pinsel", 5))
        samc_in.value = int(adc.get("samc", 0))
        pll = cfg.get("pll", {})
        key = f"{int(pll.get('postdiv1', 5))},{int(pll.get('postdiv2', 1))}"
        if key in pll_sel.options:
            pll_sel.value = key
        buf_in.value = int(cfg.get("buffer", {}).get("size", 2048))
        cap = cfg.get("capture", {})
        if int(cap.get("count", 1024)) in count_sel.options:
            count_sel.value = int(cap.get("count", 1024))
        interval_in.value = int(cap.get("interval_ms", 500))
        for unit, c in dac_ui.items():
            d = cfg.get("dac", {}).get(str(unit), {})
            c["on"].value = bool(d.get("on", False))
            c["low"].value = int(d.get("low", 0x100))
            c["high"].value = int(d.get("high", 0xF00))
            c["slp"].value = int(d.get("slpdat", 8))
        fake = cfg.get("fake", {})
        if fake.get("waveform") in waveform_sel.options:
            waveform_sel.value = fake["waveform"]
        sig_in.value = float(fake.get("signal_khz", 100.0))
        amp_in.value = float(fake.get("amplitude", 1500.0))
        noise_in.value = float(fake.get("noise", 6.0))
        harm2_in.value = float(fake.get("harmonic2", 150.0))
        harm3_in.value = float(fake.get("harmonic3", 0.0))
        update_dac_freq_label()
        refresh_channel()

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
            # A triangle needs a DAC running - on the board and in the
            # stand-in alike, an idle DAC leaves the pin at whatever it
            # floats to, which is a flat trace and looks like a bug.
            any_dac_on = any(bool(c["on"].value) for c in dac_ui.values())
            waveform_hint_lbl.text = (
                "no DAC is on, so nothing drives the pin and the trace stays flat - "
                "switch DAC1 or DAC2 on and press its apply button"
                if (waveform_sel.value == "triangle" and not any_dac_on) else "")
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
    waveform_sel.on_value_change(lambda e: refresh_channel())
    for _u, _c in dac_ui.items():
        _c["on"].on_value_change(lambda e: refresh_channel())
    core_sel.on_value_change(lambda e: refresh_channel())
    input_in.on_value_change(lambda e: refresh_channel())
    refresh_channel()

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
            state["target"].close()
            state["target"] = None
            conn_btn.text, conn_btn.icon = "connect", "usb"
            conn_chip.text, conn_chip.icon = "not connected", "link_off"
            conn_chip.props("color=grey-8")
            xfer_chip.text = "transfer: -"
            xfer_chip.props("color=grey-8")
            buf_lbl.text = "buf: not queried yet"
            return
        try:
            if port_sel.value == "fake":
                push_log("--- connecting: fake target ---")
                state["target"] = FakeTarget(signal_khz=float(sig_in.value or 100.0), on_log=push_log)
            else:
                push_log(f"--- connecting: {port_sel.value} ---")
                state["target"] = Target(port_sel.value, on_log=push_log)
            ok, lines = state["target"].cmd("version")
            conn_chip.text = (lines[0] if ok and lines else f"{port_sel.value}: connected")
            conn_chip.icon = "link"
            conn_chip.props("color=positive")
            conn_btn.text, conn_btn.icon = "disconnect", "usb_off"
            # docs/PLAN-BINARY-TRANSFER.md: probe once with 'help', use 'blk'
            # (contiguous, up to 2048 samples) when the target has it, else
            # fall back to the legacy 'dump' (last completed half, max 1024).
            state["use_blk"] = probe_blk(state["target"])
            xfer_chip.text = "transfer: blk (binary)" if state["use_blk"] else "transfer: dump (text)"
            xfer_chip.props(f'color={"positive" if state["use_blk"] else "grey-8"}')
            state["buf_size"] = query_buf(state["target"])
            buf_in.value = state["buf_size"]
            buf_lbl.text = f"buf: {state['buf_size']} (half {state['buf_size'] // 2})"
            update_count_options()
        except Exception as ex:
            push_log(f"--- connect failed: {ex} ---")
            conn_chip.text = f"connect failed: {ex}"
            conn_chip.icon = "error"
            conn_chip.props("color=negative")
            state["target"] = None
    conn_btn.on_click(do_connect)

    async def apply_settings():
        t = state["target"]
        if not t:
            apply_lbl.text = "not connected"
            return
        if isinstance(t, FakeTarget):
            t.signal_khz = float(sig_in.value or 100.0)
            t.amplitude = float(amp_in.value or 0.0)
            t.noise_std = float(noise_in.value or 0.0)
            t.harm2_amp = float(harm2_in.value or 0.0)
            t.harm3_amp = float(harm3_in.value or 0.0)
            t.waveform = waveform_sel.value or "sine"
        msgs = []
        _p1, _p2 = pll_values()
        for c in (f"pll {_p1} {_p2}",
                  f"samc {int(samc_in.value or 0)}",
                  f"core {int(core_sel.value)} {int(input_in.value or 0)}"):
            ok, lines = await run.io_bound(t.cmd, c)
            msgs.append(("✓ " if ok else "✗ ") + c)
        apply_lbl.text = "   ".join(msgs)
    apply_btn.on_click(apply_settings)

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

    async def apply_buf():
        t = state["target"]
        if not t:
            buf_lbl.text = "not connected"
            return
        n = int(buf_in.value or state["buf_size"])
        ok, lines = await run.io_bound(t.cmd, f"buf {n}")
        got = _parse_buf(lines)
        if ok and got is not None:
            state["buf_size"] = got
            buf_in.value = got
            buf_lbl.text = f"buf: {got} (half {got // 2})"
        else:
            buf_lbl.text = "buf refused: " + " ".join(lines)
        update_count_options()  # the capture 'windows' follow the new buffer size
        if ok:
            await one_cycle()  # refresh the charts immediately against the new size
    buf_btn.on_click(apply_buf)

    # ---- capture ----
    async def one_cycle():
        t = state["target"]
        if not t or state["busy"]:
            return
        state["busy"] = True
        try:
            count = int(count_sel.value)
            buf = state["buf_size"]
            # Both paths deliver the whole buffer now: "blk" always did,
            # and "dump" follows a "snap", after which all of it is valid.
            count = min(count, buf)
            samples, status = await run.io_bound(capture_cycle, t, count, 0.02, state["use_blk"])
            # The board reports its own clock; fall back to the setting.
            fs = float(status.get("ksps_nominal", 0)) * 1e3
            if fs <= 0:
                fs = rate_ksps(int(status.get("postdiv1", pll_values()[0])),
                               int(status.get("postdiv2", pll_values()[1]))) * 1e3
            if not math.isfinite(fs) and "fs_hz" in status:
                # Back-to-back has no documented rate on real hardware (only a
                # 'sweep' measurement could tell); the fake target reports its
                # stand-in rate here instead, so the FFT does not go blank.
                fs = float(status["fs_hz"])
            f, db = spectrum(samples, fs)

            n_samp = max(len(samples) - 1, 1)
            duration_s = n_samp / fs if math.isfinite(fs) and fs > 0 else 1.0
            t_scale, t_name = ((1e6, "time (µs)") if duration_s < 1e-3 else
                               (1e3, "time (ms)") if duration_s < 1.0 else (1.0, "time (s)"))
            time_chart.options["series"][0]["data"] = [[i, int(v)] for i, v in enumerate(samples)]
            time_chart.options["xAxis"][0]["max"] = n_samp
            time_chart.options["xAxis"][1]["max"] = duration_s * t_scale
            time_chart.options["xAxis"][1]["name"] = t_name
            time_chart.update()
            fft_chart.options["series"][0]["data"] = [[float(fx) / 1e3, float(d)] for fx, d in zip(f, db)]
            fft_chart.options["xAxis"][0]["max"] = float(f[-1]) / 1e3 if len(f) else 1
            fft_chart.update()
            state["cycles"] += 1

            metrics = analyze_spectrum(f, db)
            peak = metrics.get("fund_freq", float("nan")) / 1e3 if metrics else float("nan")
            fs_text = f"fs {fs/1e6:.3f} MHz" if math.isfinite(fs) else "fs unknown (back-to-back, real hardware only)"
            cyc_lbl.text = (f"cycle {state['cycles']}   {len(samples)} samples   "
                            f"{fs_text}" + (f"   peak {peak:.1f} kHz" if math.isfinite(peak) else ""))
            for k in chips:
                if k in status:
                    set_chip(k, status[k])
            blocks = status.get("blocks")
            bursts = status.get("bursts", status.get("burst_starts"))
            if blocks is not None and bursts:
                clean = (blocks == 2 * bursts)
                ratio_chip.text = (f"blocks/bursts {blocks}/{bursts}"
                                   + ("" if clean else f"  NOT 1:2 ({blocks / (2 * bursts):.1f}x)"))
                ratio_chip.props(f'color={"positive" if clean else "negative"}')
            else:
                ratio_chip.text = "blocks/bursts –"
                ratio_chip.props("color=grey-8")
            meas, nom = status.get("ksps_measured"), status.get("ksps_nominal")
            if meas and nom:
                dev = (meas - nom) / nom * 100.0
                rate_chip.text = f"rate {meas/1000:.2f} of {nom/1000:.2f} MSPS ({dev:+.1f} %)"
                rate_chip.props(f'color={"positive" if abs(dev) <= 5.0 else "negative"}')
            else:
                rate_chip.text = "rate –"
                rate_chip.props("color=grey-8")

            if metrics:
                eval_chips["fundamental"].text = f"fundamental {metrics['fund_freq']/1e3:.2f} kHz"
                eval_chips["level"].text = f"level {metrics['fund_db']:.1f} dBFS"
                eval_chips["noise floor"].text = f"noise floor {metrics['noise_db']:.1f} dBFS"
                eval_chips["SNR"].text = f"SNR {metrics['snr_db']:.1f} dB"
                eval_chips["THD"].text = f"THD {metrics['thd_pct']:.2f} %"
                for k in ("SNR",):
                    eval_chips[k].props(f'color={"positive" if metrics["snr_db"] >= 40 else "negative"}')
                harmonics = {h["k"]: h for h in metrics["harmonics"]}
                for k in (2, 3, 4, 5):
                    chip = eval_chips[f"H{k}"]
                    if k in harmonics:
                        chip.text = f"H{k}  {harmonics[k]['rel_db']:.1f} dBc"
                        chip.props("color=grey-8")
                    else:
                        chip.text = f"H{k} –"
                        chip.props("color=grey-8")
        except Exception as ex:
            cyc_lbl.text = f"cycle failed: {ex}"
            state["live"] = False
            live_btn.text, live_btn.icon = "live", "play_arrow"
        finally:
            state["busy"] = False

    single_btn.on_click(one_cycle)

    async def live_loop():
        while state["live"]:
            await one_cycle()
            await asyncio.sleep(max(0.05, float(interval_in.value or 500) / 1000.0))

    def toggle_live():
        state["live"] = not state["live"]
        live_btn.text = "stop" if state["live"] else "live"
        live_btn.icon = "stop" if state["live"] else "play_arrow"
        if state["live"]:
            asyncio.create_task(live_loop())
    live_btn.on_click(toggle_live)

    if args.fake or args.port:
        ui.timer(0.5, do_connect, once=True)

    ui.run(title="ADC/DMA capture", port=args.http_port, show=not args.no_browser, reload=False, dark=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="COM port of the board's console")
    ap.add_argument("--fake", action="store_true", help="use the built-in stand-in instead of a board")
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
