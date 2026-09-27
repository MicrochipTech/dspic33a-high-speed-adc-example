#!/usr/bin/env python3
"""
wavegen_ref.py - reference model for lib/wavegen.c (P3.1 / P3.6) and the
pulse trains lib/detect.c is tested with (P3.5).

The formula is the one in
  C:\\work\\Claas\\waveform_generator\\firmware\\src\\tab_wave_gen.py
(generate_wave(), lines 35-63 of that file), with the GUI, the WAV export
and the plot left out:

  lines 36-44   amplitudes[n] = amplitude * harm[n], harm[0] = 1.0
  lines 51-52   t_i = i / play_hz,  envelope = exp(-decay * t_i)
  lines 55-58   y_i = envelope_i * sum_n amplitudes[n] * sin(2 pi (n+1) f0 t_i)
  lines 61-63   y -= min(y);  y = y / max(y) * 1023 * amplitude;  round()

Deviations, all deliberate:
  * The script takes a duration and derives the sample count
    (round(duration * fs), line 48) and the time axis as
    linspace(0, duration, N, endpoint=False) - i.e. t_i = i * duration / N,
    which equals i / fs only when duration * fs is an integer. Here the
    table size n IS the parameter (DESIGN-MULTICHANNEL.md 4.2), so
    t_i = i / play_hz, always.
  * The script's fixed 0 .. 1023 * amplitude becomes
    out_min .. out_min + (out_max - out_min) * amplitude (the design's
    formula). With out_min = 0, out_max = 1023 it is the script's formula.
  * Standard library only (math, no numpy): sin/exp are IEEE double in both,
    the last-ulp differences between libm and numpy are far below the
    +-1 LSB the host test allows. Rounding is round-half-to-even in both
    (Python's round() and numpy.round()). The C implementation may round
    half away from zero; the test tolerance covers that too.
  * amplitude multiplies every harmonic (lines 37-43) and then the scale
    (line 62); the first cancels in the min/max normalisation, the second
    does not. Both are kept as in the script, so amplitude = 0 divides by
    zero here exactly as it would there - not a supported input.

Usage (from the repo root; every vector file's first line is the command
that made it):
  python tests/ref/wavegen_ref.py table  --out tests/ref/vectors/wavegen_0_1023.csv \\
         --n 512 --play-hz 500000 --f0 10000 --harm 0.2 0.4 0.1 0 0 0 \\
         --decay 1000 --amplitude 1.0 --out-min 0 --out-max 1023
  python tests/ref/wavegen_ref.py pulses --out tests/ref/vectors/pulses_10k_n5.csv \\
         --fs 50000 --f0 10000 --pulses 5 --pulse-len 128 --gap 128 ...

`table` writes one wavegen table: columns i,value.
`pulses` writes a detector test signal: N pulses, each one a wavegen table
(so each pulse decays with `decay`), separated by `gap` samples at the
mid value, as 12-bit ADC-like samples out_min..out_max; columns i,value.
The pulse count is in the header line, so a test can read it back.
"""

import argparse
import math
import os
import sys


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


def write_csv(path, cmdline, columns, rows):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write("# " + cmdline + "\n")
        f.write(",".join(columns) + "\n")
        for r in rows:
            f.write(",".join(str(v) for v in r) + "\n")


def cmdline_of(argv):
    return "python tests/ref/wavegen_ref.py " + " ".join(argv)


def add_wave_args(p):
    p.add_argument("--f0", type=float, default=10000.0, help="fundamental, Hz")
    p.add_argument("--harm", type=float, nargs=6, default=[0.2, 0.4, 0.1, 0.0, 0.0, 0.0],
                   help="factors of the 2nd..7th harmonic (script defaults)")
    p.add_argument("--decay", type=float, default=1000.0, help="envelope exp(-decay t)")
    p.add_argument("--amplitude", type=float, default=1.0)
    p.add_argument("--out", required=True, help="CSV to write")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    sub = ap.add_subparsers(dest="cmd", required=True)

    t = sub.add_parser("table", help="one wavegen table")
    t.add_argument("--n", type=int, default=512, help="table size")
    t.add_argument("--play-hz", type=float, default=500000.0, help="playback rate")
    t.add_argument("--out-min", type=int, default=0)
    t.add_argument("--out-max", type=int, default=1023)
    add_wave_args(t)

    p = sub.add_parser("pulses", help="N decaying pulses with gaps, 12-bit")
    p.add_argument("--fs", type=float, default=50000.0, help="sample rate of the train, Hz")
    p.add_argument("--pulses", type=int, default=5, help="number of pulses N")
    p.add_argument("--pulse-len", type=int, default=128, help="samples per pulse")
    p.add_argument("--gap", type=int, default=128, help="silent samples after each pulse")
    p.add_argument("--lead", type=int, default=64, help="silent samples before the first pulse")
    p.add_argument("--out-min", type=int, default=1048, help="pulse minimum (2048 - 1000)")
    p.add_argument("--out-max", type=int, default=3048, help="pulse maximum (2048 + 1000)")
    add_wave_args(p)

    a = ap.parse_args(argv)
    cmd = cmdline_of(argv)

    if a.cmd == "table":
        tab = wavegen(a.n, a.play_hz, a.f0, a.harm, a.decay, a.amplitude,
                      a.out_min, a.out_max)
        write_csv(a.out, cmd, ["i", "value"], enumerate(tab))
        print("%s: %d values, min %d, max %d" % (a.out, len(tab), min(tab), max(tab)))
        return 0

    # pulses: each pulse is the same wavegen table at the train's sample rate
    pulse = wavegen(a.pulse_len, a.fs, a.f0, a.harm, a.decay, a.amplitude,
                    a.out_min, a.out_max)
    mid = (a.out_min + a.out_max) // 2
    sig = [mid] * a.lead
    for _ in range(a.pulses):
        sig += pulse
        sig += [mid] * a.gap
    write_csv(a.out, cmd + " # pulses=%d" % a.pulses, ["i", "value"], enumerate(sig))
    print("%s: %d samples, %d pulses of %d + gap %d" %
          (a.out, len(sig), a.pulses, a.pulse_len, a.gap))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
