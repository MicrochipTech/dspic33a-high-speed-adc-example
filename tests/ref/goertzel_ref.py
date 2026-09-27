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

Goertzel (resonator in float64, everything after it in exact integers)
  lines 14-16    omega = 2 pi f / fs, cos, sin
  line 44        s = x >> in_shift   (template: >> 2, 12 -> 10 bit)
  lines 54-57    the feedback with the damping factor D - in the form the
                 user decided on 27.09.2026 (DESIGN-MULTICHANNEL.md 4.3):
                     q0 = s + 2 D cos(w) q1 - D^2 q2;  q2 = q1;  q1 = q0
                 the textbook damped Goertzel: poles at D e^(+-jw), radius
                 D, centre frequency exactly w. (The template's line 57 has
                 D only on the q1 term and lines 59-60 damp the states with
                 q - (q >> 8); with 59-60 dropped as the design asked, the
                 poles would sit ON the unit circle - see the note at
                 Goertzel.step below.)
  lines 59-60    q2 = q1 - (q1 >> 8); q1 = q0 - (q0 >> 8)  ** LEFT OUT **
  lines 63-66    real = q1 - q2 cos;  imag = q2 sin
  lines 69-71    magnitude = |re| + |im| - min(|re|, |im|) / 2   (float64)
  line 74        m = int(magnitude) (truncation, as (int32_t) in C and as
                 the template's >> 16 of its Q16 magnitude), then the IIR1
                 low-pass above, k = 4, in exact integers - so lib/iir1 is
                 what both C variants use and the model is bit-exact there.
  lines 78-87    `window`: the low-pass runs every sample, its output is
                 EMITTED every `window` samples (the template consulted the
                 detector every WINDOW_SIZE + 1 samples; window = 1 emits
                 every sample). The detector sees the emitted values.
                 The template's reset of the Goertzel state and the LP tap
                 on a detection (81-85) is NOT carried over - the detector
                 below re-arms by hysteresis instead (user decision
                 27.09.2026).
  line 47 and main.c lines 204-208: max_amplitude = max(s); adaptive
                 threshold = max_amplitude * scale (1.5 in main.c), Q16.

  The float template (lines 118-155) applies D three times (130-132) and
  its magnitude >> 16 (140) is a scaling error that leaves it 0..3 - not
  followed.

Detector (per instance; lib/detect.c, P3.5), user decision 27.09.2026:
  fire once when an emitted lp value crosses ABOVE threshold while armed
  (counter++, det = 1, disarm); re-arm only once lp has fallen BELOW
  threshold * hyst (hyst < 1, default 0.5). One pulse = one detection,
  however long the pulse stays above threshold. The template counted every
  window in which lp > threshold after resetting and refilling the
  resonator - two or more counts per pulse.

Magnitudes are in units of the shifted input sample s; a fixed-point
implementation picks its own internal scale and converts back before the
low-pass, the test tolerance covers its quantisation.

Usage (from the repo root; the first line of each CSV is the command):
  python tests/ref/goertzel_ref.py iir1 --k 4 --out tests/ref/vectors/iir1_step_k4.csv
  python tests/ref/goertzel_ref.py goertzel --input tests/ref/vectors/pulses_10k_n5.csv \\
         --fs 50000 --f 10000 --damping 0.995 --in-shift 2 --window 1 \\
         --threshold 2000 --hyst 0.5 --out tests/ref/vectors/goertzel_10k_n5.csv
Columns of a goertzel vector: s (shifted input), mag (float64 magnitude
before truncation), lp (integer low-pass output, every sample), emit (1
where the C block function writes lp to mag_out), det (1 where the
detector fires; only at emitted samples). The header line ends with
"# detections=N max_amplitude=M".
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

LP_K = 4    # the low-pass shift, template LP_SHIFT_VALUE_K


def magnitude_approx(re, im):
    """goertzel.c lines 69-71."""
    a, b = abs(re), abs(im)
    return a + b - min(a, b) / 2.0


class Goertzel:
    """One damped Goertzel resonator with its low-pass and window counter.

    form: "damped" is THE form (user decision 27.09.2026). The two others
    are kept only so the decision can be re-checked; they are NOT USED by
    any vector or test:
      feedback-only : q0 = s + 2 cos D q1 - q2;  q2 = q1;  q1 = q0
                      (the design text literally; poles on the unit circle,
                      nothing decays, D only detunes)
      template-float: q0 = s + 2 cos D q1 - q2;  q2 = D q1; q1 = D q0
                      (goertzel.c 130-132; radius D, centre detuned to
                      cos(theta) = D cos(w))
    """

    def __init__(self, fs_hz, f_hz, damping, window=1, form="damped"):
        w = 2.0 * math.pi * f_hz / fs_hz
        self.cos, self.sin = math.cos(w), math.sin(w)
        self.d = damping
        self.window = window
        self.form = form
        self.reset()

    def reset(self):
        self.q1 = self.q2 = 0.0
        self.tap = 0
        self.win_cnt = 0

    def step(self, s):
        """One shifted sample s. Returns (mag_f64, lp_int, emit)."""
        c, d, q1, q2 = self.cos, self.d, self.q1, self.q2
        if self.form == "damped":
            q0 = s + 2.0 * d * c * q1 - d * d * q2
            self.q2, self.q1 = q1, q0
        elif self.form == "feedback-only":
            q0 = s + 2.0 * c * d * q1 - q2
            self.q2, self.q1 = q1, q0
        elif self.form == "template-float":
            q0 = s + 2.0 * c * d * q1 - q2
            self.q2, self.q1 = d * q1, d * q0
        else:
            raise ValueError("unknown form " + self.form)
        re = self.q1 - self.q2 * c
        im = self.q2 * self.sin
        mag = magnitude_approx(re, im)
        self.tap, lp = iir1_lp(self.tap, int(mag), LP_K)
        self.win_cnt += 1
        emit = 0
        if self.win_cnt >= self.window:
            self.win_cnt = 0
            emit = 1
        return mag, lp, emit


class Detector:
    """lib/detect.c: threshold with hysteresis re-arm, counter,
    max_amplitude and the adaptive threshold of main.c 204-208."""

    def __init__(self, threshold, hyst=0.5):
        self.threshold = threshold
        self.hyst = hyst
        self.reset()

    def reset(self):
        self.counter = 0
        self.max_amplitude = 0
        self.armed = True

    def amplitude(self, s):
        """goertzel.c line 47, every input sample."""
        if self.max_amplitude < s:
            self.max_amplitude = s

    def magnitude(self, lp):
        """One emitted low-pass value. True when a detection fires."""
        if self.armed:
            if lp > self.threshold:
                self.counter += 1
                self.armed = False
                return True
        elif lp < self.threshold * self.hyst:
            self.armed = True
        return False

    def adapt(self, scale):
        """main.c 204-208: threshold = max_amplitude * scale in Q16, then
        max_amplitude and counter start over."""
        self.threshold = (self.max_amplitude * int(scale * 65536)) >> 16
        self.max_amplitude = 0
        self.counter = 0


def goertzel_run(g, det, samples, in_shift):
    """Per sample: (s, mag, lp, emit, det)."""
    out = []
    for x in samples:
        s = x >> in_shift
        det.amplitude(s)
        mag, lp, emit = g.step(float(s))
        fired = 1 if (emit and det.magnitude(lp)) else 0
        out.append((s, mag, lp, emit, fired))
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
    g.add_argument("--fs", type=float, default=50000.0)
    g.add_argument("--f", type=float, default=10000.0)
    g.add_argument("--damping", type=float, default=0.995)
    g.add_argument("--window", type=int, default=1,
                   help="emit the low-pass output every `window` samples")
    g.add_argument("--threshold", type=int, default=2000)
    g.add_argument("--hyst", type=float, default=0.5,
                   help="re-arm below threshold * hyst")
    g.add_argument("--in-shift", type=int, default=2)
    g.add_argument("--form", default="damped",
                   choices=["damped", "feedback-only", "template-float"],
                   help="NOT USED except 'damped' - see class Goertzel")
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
    gz = Goertzel(a.fs, a.f, a.damping, a.window, a.form)
    det = Detector(a.threshold, a.hyst)
    rows = goertzel_run(gz, det, samples, a.in_shift)
    write_csv(a.out, cmd + " # detections=%d max_amplitude=%d" %
              (det.counter, det.max_amplitude),
              ["s", "mag", "lp", "emit", "det"],
              [(s, "%.6g" % m, lp, e, d) for s, m, lp, e, d in rows])
    print("%s: %d rows, %d emitted, %d detections, max lp %d, max_amplitude %d" %
          (a.out, len(rows), sum(r[3] for r in rows), det.counter,
           max(r[2] for r in rows), det.max_amplitude))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
