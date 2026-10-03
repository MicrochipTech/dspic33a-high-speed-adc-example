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

"""
wavegen_model.py - the signal generator's table and playback, as a model
(docs/IMPLEMENTATION-PLAN.md SG.6, 29.09.2026).

One model for three users, the eval_chain.py rule ("one evaluator, one
place"): tests/ref/wavegen_ref.py (the host reference lib/wavegen.c is
tested against) imports wavegen() from here; tools/adc_gui.py draws the
table preview and the loop overlay with it; its FakeTarget plays the table
from it. Standard library only for wavegen()/snap_hz()/sccp2_rate(), numpy
for the playback and alignment helpers below them.

wavegen() is the formula of tab_wave_gen.py (generate_wave(), lines 35-63),
moved verbatim out of tests/ref/wavegen_ref.py - see that file's docstring
for the three deliberate deviations from the script (t_i = i / play_hz,
the out_min..out_max range, standard library math).
"""

import math


def wavegen(n, play_hz, f0_hz, harm, decay, amplitude, out_min, out_max):
    """The table as a list of n ints in out_min..out_max (script lines 35-63)."""
    if len(harm) != 6:
        raise ValueError("harm needs the 6 factors of the 2nd..7th harmonic")
    amplitudes = [amplitude * 1.0] + [amplitude * h for h in harm]
    y = []
    for i in range(n):
        t = i / play_hz
        env = math.exp(-decay * t)
        s = 0.0
        for k, a in enumerate(amplitudes):
            s += a * math.sin(2.0 * math.pi * (k + 1) * f0_hz * t)
        y.append(env * s)
    lo = min(y)
    shifted = [v - lo for v in y]
    hi = max(shifted)
    span = (out_max - out_min) * amplitude
    return [int(round(v / hi * span)) + out_min for v in shifted]


def snap_hz(f0_hz, n, play_hz):
    """lib/wavegen.c's wavegen_snap_hz(): the fundamental moved to the
    nearest whole number of periods in the table, at least one."""
    k = max(1, round(f0_hz * n / play_hz))
    return k * play_hz / n


# SCCP2's input clock on the board: the standard peripheral clock, CPU / 2
# (sccp.c, Table 26-2 p1756).
SCCP2_CLK_HZ = 100_000_000


def sccp2_rate(play_hz, clk_hz=SCCP2_CLK_HZ):
    """The rate sccp2_start() really produces for `play_hz` (siggen.c:
    ticks = round(clk / play_hz); TMR16 fits it into 16 bits with the
    1:4/16/64 prescaler, rounding the period to that step)."""
    ticks = int((clk_hz + play_hz // 2) // play_hz)
    div, t = 1, ticks
    while t > 65536 and div < 64:
        div *= 4
        t = (ticks + div // 2) // div
    period = t * div
    return (clk_hz + period // 2) // period


# ---------------------------------------------------------------------------
# Playback and alignment (numpy)
# ---------------------------------------------------------------------------

def playback(table, play_hz, fs_hz, n_samples, start_entry=0.0, tau_s=0.0):
    """What an ADC sampling at fs_hz reads from a DAC that holds each table
    entry for 1 / play_hz (zero-order hold), starting `start_entry` entries
    into the table, optionally through a first-order low-pass of time
    constant tau_s (the DAC's settling). Returns float samples."""
    import numpy as np
    tab = np.asarray(table, float)
    n = len(tab)
    pos = start_entry + np.arange(n_samples) * play_hz / fs_hz
    v = tab[np.floor(pos).astype(int) % n]
    if tau_s > 0.0:
        a = math.exp(-1.0 / (fs_hz * tau_s))
        out = np.empty_like(v)
        acc = v[0]
        for i, x in enumerate(v):
            acc = a * acc + (1.0 - a) * x
            out[i] = acc
        v = out
    return v


def align(samples, table, play_hz, fs_hz, steps_per_entry=10):
    """Where in the table a captured window lies, and how well it matches.

    The table played at play_hz (zero-order hold) is expanded at fs_hz over
    one whole table period, the captured window is cross-correlated with it
    (circularly, by FFT) at `steps_per_entry` phases per table entry, and
    at the best lag gain and offset are fitted by least squares. Returns a
    dict: rms (LSB, the residual after the fit), gain, offset, entry (the
    table entry the window starts at), and `expected` - the fitted model
    over the window, for the GUI's overlay. A window longer than the table
    period is fine; a table period over 2**21 samples is refused (None)."""
    import numpy as np
    s = np.asarray(samples, float)
    tab = np.asarray(table, float)
    n = len(tab)
    period = n * fs_hz / play_hz                 # samples per table period
    if period > 2 ** 21 or len(s) < 4:
        return None
    best = None
    sc = s - s.mean()
    for k in range(steps_per_entry):
        start = k / steps_per_entry
        m = int(math.ceil(period)) + len(s)
        ref = playback(tab, play_hz, fs_hz, m, start_entry=start)
        # circular correlation of the zero-mean window against every start
        # position of the reference within one period
        L = int(math.ceil(period))
        N = 1 << int(math.ceil(math.log2(L + len(s))))
        R = np.fft.rfft(ref[:L + len(s)] - ref.mean(), N)
        S = np.fft.rfft(sc[::-1], N)
        corr = np.fft.irfft(R * S, N)[len(s) - 1:len(s) - 1 + L]
        # the positive peak: the DAC does not invert, and a table with only
        # odd harmonics matches itself inverted half a period away
        lag = int(np.argmax(corr))
        x = ref[lag:lag + len(s)]
        A = np.vstack([x, np.ones_like(x)]).T
        coef, *_ = np.linalg.lstsq(A, s, rcond=None)
        e = s - A @ coef
        rms = float(np.sqrt(np.mean(e ** 2)))
        if best is None or rms < best["rms"]:
            best = dict(rms=rms, gain=float(coef[0]), offset=float(coef[1]),
                        entry=(start + lag * play_hz / fs_hz) % n, expected=A @ coef)
    return best


if __name__ == "__main__":
    # a quick self-check: a table played and aligned against itself
    import numpy as np
    tab = wavegen(1000, 100000, 1000, [0, 0.3, 0, 0, 0, 0], 0.0, 1.0, 800, 3500)
    v = playback(tab, 100000, 1e6, 1024, start_entry=123.4) + np.random.default_rng(1).normal(0, 3, 1024)
    r = align(v, tab, 100000, 1e6)
    print(f"rms {r['rms']:.2f} gain {r['gain']:.4f} offset {r['offset']:+.2f} entry {r['entry']:.1f} (123.4)")
