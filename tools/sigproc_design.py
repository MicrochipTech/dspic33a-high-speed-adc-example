"""sigproc_design.py - the filter coefficients in src/core/sigproc.c, derived.

    python tools/sigproc_design.py      the #defines for sigproc.c and a check
                                        of each design against its analog
                                        Butterworth magnitude

Three 4th-order Butterworth filters, each as two biquad sections, all by the
bilinear transform with pre-warping (s -> (z-1)/(z+1), analog frequency
W = tan(pi f / fs), f in units of fs below):

  low-pass   cut-off fs/8 (-3 dB)            numerator per section (1, 2, 1)
  high-pass  cut-off fs/8 (-3 dB)            numerator per section (1, -2, 1)
  band-pass  centre fs/8, one octave wide:   numerator per section (1, 0, -1)
             -3 dB at fs/8/sqrt(2) and fs/8*sqrt(2) (a 2nd-order Butterworth
             prototype, low-pass to band-pass, which doubles the order)

Each section is written y = g (b0 x + b1 x1 + b2 x2) - a1 y1 - a2 y2 with b
the numerator above; sigproc.c runs the sections without g and multiplies the
cascade's output by g1 * g2 once. The low-pass's numbers are the ones sigproc.c
has carried since 02.10.2026 (fs/8); this script reproduces them.
No scipy needed (numpy for the band-pass's quartic only).
"""
import math

import numpy as np

FC = 1.0 / 8.0                       # cut-off / centre, in units of fs


def butter4_sections():
    """The two analog sections of a 4th-order Butterworth low-pass at W = 1:
    s^2 + 2 sin(theta) s + 1 with theta = pi/8, 3pi/8 (1/Q = 2 sin theta)."""
    return [2.0 * math.sin(math.pi / 8.0), 2.0 * math.sin(3.0 * math.pi / 8.0)]


def lp_hp(kind):
    """(g, a1, a2) per section. With W = tan(pi fc): s -> s/W, bilinear."""
    w = math.tan(math.pi * FC)
    out = []
    for c in butter4_sections():
        # (s/W)^2 + c (s/W) + 1, s = (z-1)/(z+1), times W^2 (z+1)^2:
        # (z-1)^2 + c W (z-1)(z+1) + W^2 (z+1)^2
        a0 = 1.0 + c * w + w * w
        a1 = (2.0 * w * w - 2.0) / a0
        a2 = (1.0 - c * w + w * w) / a0
        if kind == "lp":
            g = (1.0 + a1 + a2) / 4.0          # unity at DC, numerator (1, 2, 1)
        else:
            g = (1.0 - a1 + a2) / 4.0          # unity at Nyquist, numerator (1, -2, 1)
        out.append((g, a1, a2))
    return out


def bp():
    """(g, a1, a2) per section, numerator (1, 0, -1) each."""
    w1 = math.tan(math.pi * FC / math.sqrt(2.0))
    w2 = math.tan(math.pi * FC * math.sqrt(2.0))
    w0sq, b = w1 * w2, w2 - w1
    # prototype 1/(p^2 + sqrt2 p + 1), p = (s^2 + w0^2)/(b s):
    # denominator (s^2 + w0^2)^2 + sqrt2 b s (s^2 + w0^2) + b^2 s^2
    den = np.array([1.0, math.sqrt(2.0) * b, 2.0 * w0sq + b * b, math.sqrt(2.0) * b * w0sq, w0sq * w0sq])
    roots = np.roots(den)
    pairs, used = [], set()
    for i, r in enumerate(roots):          # complex-conjugate pairs -> s^2 + p s + q
        if i in used or r.imag < 0:
            continue
        j = min((k for k in range(len(roots)) if k not in used and k != i),
                key=lambda k: abs(roots[k] - r.conjugate()))
        used |= {i, j}
        pairs.append((-2.0 * r.real, abs(r) ** 2))
    out = []
    for p, q in sorted(pairs):
        # section b s / (s^2 + p s + q), bilinear: numerator b (z^2 - 1),
        # denominator (z-1)^2 + p (z-1)(z+1) + q (z+1)^2
        a0 = 1.0 + p + q
        a1 = (2.0 * q - 2.0) / a0
        a2 = (1.0 - p + q) / a0
        out.append((b / a0, a1, a2))
    return out


NUM = {"lp": (1.0, 2.0, 1.0), "hp": (1.0, -2.0, 1.0), "bp": (1.0, 0.0, -1.0)}


def response(sections, kind, f):
    z = np.exp(1j * 2.0 * math.pi * f)
    h = 1.0 + 0j
    b0, b1, b2 = NUM[kind]
    for g, a1, a2 in sections:
        h *= g * (b0 + b1 / z + b2 / z / z) / (1.0 + a1 / z + a2 / z / z)
    return abs(h)


def analog(kind, f):
    """|H| of the analog prototype at the pre-warped frequency."""
    w, wc = math.tan(math.pi * f), math.tan(math.pi * FC)
    if kind == "lp":
        return 1.0 / math.sqrt(1.0 + (w / wc) ** 8)
    if kind == "hp":
        return 1.0 / math.sqrt(1.0 + (wc / w) ** 8)
    w1 = math.tan(math.pi * FC / math.sqrt(2.0))
    w2 = math.tan(math.pi * FC * math.sqrt(2.0))
    x = (w * w - w1 * w2) / ((w2 - w1) * w)
    return 1.0 / math.sqrt(1.0 + x ** 4)


def main():
    designs = {"lp": lp_hp("lp"), "hp": lp_hp("hp"), "bp": bp()}
    ok = True
    for kind, secs in designs.items():
        name = kind.upper()
        for i, (g, a1, a2) in enumerate(secs, 1):
            print(f"#define {name}_G{i}  {g: .9f}f")
            print(f"#define {name}_C{i}  {a1: .9f}f")
            print(f"#define {name}_D{i}  {a2: .9f}f")
        row = []
        for f in (0.02, 0.0625, FC / math.sqrt(2.0), 0.1, FC, 0.15, FC * math.sqrt(2.0), 0.2, 0.25, 0.4):
            d, a = response(secs, kind, f), analog(kind, f)
            ok &= abs(d - a) < 1e-6
            row.append(f"{f:.4f}:{d:.4f}")
        print(f"/* {name} |H|: " + "  ".join(row) + " */\n")
    print("matches the analog Butterworth magnitude:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
