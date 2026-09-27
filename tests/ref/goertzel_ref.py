#!/usr/bin/env python3
"""
goertzel_ref.py - reference model for lib/iir1.c (P3.2), lib/goertzel_f.c /
lib/goertzel_i.c (P3.3, P3.4) and lib/detect.c (P3.5).

Written from the template
  C:\\work\\Claas\\Goertzel\\goertzel\\firmware\\src\\goertzel.c  (+ goertzel.h, main.c)
Line numbers below refer to that goertzel.c.

IIR1 (exact integer model, the part P3.2 is tested against bit for bit)
  lines 169-207  iFLT_IIR1_Lowpass:  tap = tap - (tap >> k) + x;  y = tap >> k
  lines 210-234  iFLT_IIR1_Highpass: the same tap update;         y = x - (tap >> k)
  The template's k is the constant 4 (LP_SHIFT_VALUE_K / HP_SHIFT_VALUE_K);
  here it is a parameter. The template keeps the taps in the global
  iIIR_Tap[N_FILTER] (line 7) and FLT_vIIR_Init() (160-164) clears ALL of
  them; here each instance owns its tap (design 4.3). Python ints shift
  like C int32 arithmetic shifts (floor), so the model is exact as long as
  the C tap does not overflow: |x| <= 2^(31-k) - 1 keeps |tap| < 2^31.

Goertzel (float64)
  lines 14-16    omega = 2 pi f / fs, cos, sin;  coeff = 2 cos
  line 44        data_sample = x >> in_shift   (template: >> 2, 12 -> 10 bit)
  lines 54-57    feedback:  q0 = s + coeff * q1 * D - q2   (D = damping)
  lines 59-60    q2 = q1 - (q1 >> 8); q1 = q0 - (q0 >> 8)  ** LEFT OUT **
                 (the second damping stage, design decision of 26.09.2026)
  lines 63-66    real = q1 - q2 cos;  imag = q2 sin
  lines 69-71    magnitude = |re| + |im| - min(|re|, |im|) / 2
  line 74        low-pass IIR1, k = 4, on the magnitude (per sample)
  lines 78-87    every `window` samples: if lp > threshold -> q = 0, LP tap
                 = 0, counter++ (the detector; the template checks when
                 down_counter++ == WINDOW_SIZE, i.e. every WINDOW_SIZE + 1
                 samples - here `window` IS the period: window = 2 is the
                 template's WINDOW_SIZE = 1)
  line 47 and main.c lines 204-208: max_amplitude = max(data_sample);
                 adaptive threshold = max_amplitude * scale (1.5 in main.c)

  The float template (lines 118-155) applies D three times: in the feedback
  (130) AND on both states (131-132), and its magnitude >> 16 (140) is a
  scaling error that leaves it 0..3 - it is not followed.

  ** THE RECURRENCE FORM IS NOT DECIDED (P3.1 stop condition) **
  Dropping the shift stage and keeping D only in the feedback term gives
  the characteristic polynomial z^2 - 2 D cos w z + 1: the roots' product
  is 1, so the poles sit ON the unit circle and nothing decays - D only
  detunes the resonator (form "feedback-only"). The two forms that do
  damp are selectable too; `--form` is mandatory until the decision is
  taken, and no goertzel/detector vector is generated before that:
    feedback-only : q0 = s + 2 cos D q1 - q2;  q2 = q1;  q1 = q0    (design, literally)
    template-float: q0 = s + 2 cos D q1 - q2;  q2 = D q1; q1 = D q0 (goertzel.c 130-132,
                    pole radius D, centre detuned: cos(theta) = D cos(w))
    damped        : q0 = s + 2 D cos q1 - D^2 q2; q2 = q1; q1 = q0 (textbook damped
                    Goertzel, pole radius D, centre exactly w)
  In every form the magnitude is in units of the shifted input sample
  (data_sample); a fixed-point implementation picks its own internal
  scale and converts back, the test tolerance covers the quantisation.
  The low-pass on the magnitude is the same recurrence in float
  (tap = tap - tap / 2^k + m; y = tap / 2^k), not the integer one, so the
  float and the fixed-point variant are both measured against one model.

Usage (from the repo root; the first line of each CSV is the command):
  python tests/ref/goertzel_ref.py iir1 --k 4 --out tests/ref/vectors/iir1_step_k4.csv
  python tests/ref/goertzel_ref.py iir1 --k 4 --limit --out tests/ref/vectors/iir1_limit_k4.csv
  python tests/ref/goertzel_ref.py goertzel --form damped --fs 50000 --f 10000 \\
         --damping 0.995 --window 2 --threshold 2000 --in-shift 2 --k 4 \\
         --input tests/ref/vectors/pulses_10k_n5.csv --out tests/ref/vectors/goertzel_...csv
"""

import argparse
import math
import os
import sys


# ---------------------------------------------------------------- IIR1 (int)

def iir1_lp(tap, x, k):
    """goertzel.c lines 201-205. Returns (new_tap, y)."""
    tap = tap - (tap >> k) + x
    return tap, tap >> k


def iir1_hp(tap, x, k):
    """goertzel.c lines 226-231. Returns (new_tap, y)."""
    tap = tap - (tap >> k) + x
    return tap, x - (tap >> k)


def iir1_step_sequence(k, limit):
    """The test input: 8 x 0, 56 x +A, 56 x -A, 8 x 0. A = 1000, or with
    --limit the largest x the C int32 tap can take at this k
    (2^(31-k) - 1), so the test proves the tap does not overflow there."""
    a = (1 << (31 - k)) - 1 if limit else 1000
    return [0] * 8 + [a] * 56 + [-a] * 56 + [0] * 8


# ------------------------------------------------------------ Goertzel (f64)

def magnitude_approx(re, im):
    """goertzel.c lines 69-71."""
    a, b = abs(re), abs(im)
    return a + b - min(a, b) / 2.0


class Goertzel:
    def __init__(self, fs_hz, f_hz, damping, form, k):
        w = 2.0 * math.pi * f_hz / fs_hz
        self.cos, self.sin = math.cos(w), math.sin(w)
        self.d = damping
        self.form = form
        self.k = k
        self.reset()

    def reset(self):
        self.q1 = self.q2 = 0.0
        self.tap = 0.0

    def step(self, s):
        """One sample s (already shifted). Returns the low-passed magnitude."""
        c, d, q1, q2 = self.cos, self.d, self.q1, self.q2
        if self.form == "feedback-only":
            q0 = s + 2.0 * c * d * q1 - q2
            self.q2, self.q1 = q1, q0
        elif self.form == "template-float":
            q0 = s + 2.0 * c * d * q1 - q2
            self.q2, self.q1 = d * q1, d * q0
        elif self.form == "damped":
            q0 = s + 2.0 * d * c * q1 - d * d * q2
            self.q2, self.q1 = q1, q0
        else:
            raise ValueError("unknown form " + self.form)
        re = self.q1 - self.q2 * c
        im = self.q2 * self.sin
        m = magnitude_approx(re, im)
        self.tap = self.tap - self.tap / (1 << self.k) + m
        return self.tap / (1 << self.k)


class Detector:
    """goertzel.c lines 78-87 + main.c 204-208, per instance."""

    def __init__(self, threshold, window):
        self.threshold = threshold
        self.window = window
        self.reset()

    def reset(self):
        self.win_cnt = 0
        self.counter = 0
        self.max_amplitude = 0

    def sample(self, s, lp):
        """Returns True when this sample fires a detection (the caller then
        resets the Goertzel state, as lines 81-85 do)."""
        if self.max_amplitude < s:
            self.max_amplitude = s
        self.win_cnt += 1
        if self.win_cnt < self.window:
            return False
        self.win_cnt = 0
        if lp > self.threshold:
            self.counter += 1
            return True
        return False

    def adapt(self, scale):
        """main.c 204-208: threshold = max_amplitude * scale, then max and
        counter start over. The template does the multiply in Q16."""
        self.threshold = (self.max_amplitude * int(scale * 65536)) >> 16
        self.max_amplitude = 0
        self.counter = 0


def goertzel_block(g, det, samples, in_shift):
    """Per sample: shifted input, low-passed magnitude, detection flag."""
    out = []
    for x in samples:
        s = x >> in_shift
        lp = g.step(float(s))
        fired = det.sample(s, lp)
        if fired:
            g.reset()
        out.append((s, lp, 1 if fired else 0))
    return out


# --------------------------------------------------------------------- I/O

def write_csv(path, cmdline, columns, rows):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write("# " + cmdline + "\n")
        f.write(",".join(columns) + "\n")
        for r in rows:
            f.write(",".join(str(v) for v in r) + "\n")


def read_csv_values(path, column="value"):
    """Reads one integer column of a vector CSV written by these scripts."""
    with open(path) as f:
        lines = [ln.rstrip("\n") for ln in f if not ln.startswith("#")]
    cols = lines[0].split(",")
    idx = cols.index(column)
    return [int(ln.split(",")[idx]) for ln in lines[1:] if ln]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    sub = ap.add_subparsers(dest="cmd", required=True)

    i = sub.add_parser("iir1", help="IIR1 step response, LP and HP, exact integers")
    i.add_argument("--k", type=int, default=4, help="shift k (template: 4)")
    i.add_argument("--limit", action="store_true",
                   help="drive with 2^(31-k)-1 instead of 1000")
    i.add_argument("--out", required=True)

    g = sub.add_parser("goertzel", help="Goertzel + LP + detector over an input CSV")
    g.add_argument("--form", required=True,
                   choices=["feedback-only", "template-float", "damped"],
                   help="recurrence form - undecided, see the file header")
    g.add_argument("--fs", type=float, default=50000.0)
    g.add_argument("--f", type=float, default=10000.0)
    g.add_argument("--damping", type=float, default=0.995)
    g.add_argument("--window", type=int, default=2,
                   help="detector period in samples (2 = template WINDOW_SIZE 1)")
    g.add_argument("--threshold", type=int, default=2000)
    g.add_argument("--in-shift", type=int, default=2)
    g.add_argument("--k", type=int, default=4)
    g.add_argument("--input", required=True, help="CSV with a `value` column")
    g.add_argument("--out", required=True)

    a = ap.parse_args(argv)
    cmd = "python tests/ref/goertzel_ref.py " + " ".join(argv)

    if a.cmd == "iir1":
        xs = iir1_step_sequence(a.k, a.limit)
        rows, tl, th = [], 0, 0
        for x in xs:
            tl, yl = iir1_lp(tl, x, a.k)
            th, yh = iir1_hp(th, x, a.k)
            rows.append((x, yl, yh))
        write_csv(a.out, cmd, ["x", "lp", "hp"], rows)
        print("%s: %d rows, k = %d" % (a.out, len(rows), a.k))
        return 0

    samples = read_csv_values(a.input)
    gz = Goertzel(a.fs, a.f, a.damping, a.form, a.k)
    det = Detector(a.threshold, a.window)
    rows = goertzel_block(gz, det, samples, a.in_shift)
    write_csv(a.out, cmd + " # detections=%d" % det.counter,
              ["s", "lp", "det"], [(s, repr(lp), d) for s, lp, d in rows])
    print("%s: %d rows, %d detections, max_amplitude %d" %
          (a.out, len(rows), det.counter, det.max_amplitude))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
