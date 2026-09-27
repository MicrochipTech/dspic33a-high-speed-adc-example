#!/usr/bin/env python3
r"""test_tri_eval_xcheck.py - the C triangle evaluator against its Python port.

    python tests\host\test_tri_eval_xcheck.py [path\to\test_tri_eval.exe]

P2.4 of docs/IMPLEMENTATION-PLAN.md. tools/eval_chain.py carries a Python
port of tri_eval() (src/lib/tri_eval.c) so that `chain all` logs can be
judged on the host; the two must agree, or a log could be judged
differently from the board. This script generates windows with
eval_chain.synth() (the same synthetic triangle the GUI's fake target
uses), runs the host-built C evaluator on them (`test_tri_eval --eval`,
built by tools\hosttest.bat, which also runs this script after the C
tests), runs eval_chain.tri_eval() on the very same windows, and compares
every field.

"Identical" means:

- every integer and boolean field is equal: mn, mx, pp, tps, n_up, n_dn,
  zero, dbl, slip_k, slip_n, step_checked, overflow, and the grid verdict
  (tri_grid_ok / grid_ok);
- every floating field agrees to within the precision the C side keeps.
  Both sides compute in IEEE double with the same operations in the same
  order (fit_line's sums, the intersections, the medians); Python keeps
  the double, C stores the result in a float (tri_t's l_up, l_dn, dev,
  step, slip are `float`) and prints it with %.9g, which round-trips a
  float exactly. So the Python double is rounded to float here and must
  then match the C float to within 1 float ulp for l_up, l_dn, step and
  slip (an ulp of slack for the one place the double arithmetic can
  legitimately differ: Python's sum() is compensated since 3.12, C adds
  sequentially - a last-bit difference in the double can flip the float
  rounding at a boundary). dev gets half a float ulp of the larger mean
  slope length on top: C computes `L - (double)r->l_up` with the ALREADY
  ROUNDED float mean, Python with the double mean, so dev can differ by
  up to half a float ulp of l_up/l_dn before its own rounding - with a
  single slope per direction Python's dev is exactly 0 and C's is that
  rounding residue (a few 1e-6 at slopes of ~128 samples).

The largest deviation seen per field is printed as a fraction of its
tolerance (1.00 = at the limit), for dev also in absolute samples. On the
first disagreement the window is written to build/host/xcheck_mismatch.txt
with both results, and the script exits 1 - the two evaluators are then
to be looked at, not "fixed" into agreement.

Needs only the standard library; runs with any Python 3.8+.
"""
import math
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import eval_chain  # noqa: E402

INT_FIELDS = ('mn', 'mx', 'pp', 'tps', 'n_up', 'n_dn', 'zero', 'dbl',
              'slip_k', 'slip_n', 'step_checked', 'overflow', 'grid_ok')
FLT_FIELDS = ('l_up', 'l_dn', 'dev', 'step', 'slip')
LINE_RE = re.compile(r'(\w+)=(\S+)')


def to_f32(v):
    """The double v rounded to the nearest float, as a double."""
    return struct.unpack('f', struct.pack('f', v))[0]


def ulp32(v):
    """One float ulp at |v| (the spacing of floats there)."""
    v = abs(v)
    if v == 0.0:
        return 2.0 ** -149
    e = math.frexp(v)[1]            # v = m * 2**e, 0.5 <= m < 1
    return 2.0 ** (e - 24)


def tolerance(field, p):
    """The allowed |C - float(Python)| for one float field, in absolute terms."""
    pv = to_f32(p[field])
    tol = ulp32(pv)
    if field == 'dev':
        tol += 0.5 * ulp32(max(p['l_up'], p['l_dn']))
    return tol


def windows():
    """(description, samples) pairs - the same list every run (fixed seeds)."""
    out = []
    # the chain test's slopes, clean / lost / repeated, several phases and seeds
    cases = [(127.5, 0.0), (126.8, 8.0), (87.0, 0.0), (64.0, 0.0), (29.0, 0.0),
             (200.0, 0.0), (45.5, 3.0), (300.0, 12.0)]
    k = 0
    for slope, tau in cases:
        for j in range(12):
            k += 1
            phase = j * 2.0 * slope / 12.0
            for fault in (None, 'drop', 'dup'):
                at = 400 + (k * 137 + j * 29) % 1200
                kw = dict(tau=tau, seed=k)
                if fault == 'drop':
                    kw['drop'] = at
                elif fault == 'dup':
                    kw['dup'] = at
                out.append((f'slope {slope} tau {tau} phase {phase:.1f} {fault or "clean"} seed {k}',
                            eval_chain.synth(2048, slope, phase, **kw)))
    # other lengths
    for n in (1024, 1536, 4096):
        out.append((f'n {n} slope 127.5 clean', eval_chain.synth(n, 127.5, 40.0, seed=n)))
        out.append((f'n {n} slope 29 drop', eval_chain.synth(n, 29.0, 3.0, drop=n // 2, seed=n)))
    # the early returns and the corners of the algorithm
    out.append(('n 10 (below 16)', list(range(100, 110))))
    out.append(('n 16 ramp', list(range(100, 116))))
    out.append(('constant', [2000] * 2048))
    out.append(('two values', [1000, 3000] * 1024))
    out.append(('tiny swing (pp < 32, h = 8)', [2000 + (i % 20) if (i // 20) % 2 == 0 else 2019 - (i % 20)
                                                for i in range(2048)]))
    out.append(('exact triangle 100', [500 + 30 * (i % 200) if (i % 200) < 100 else 3500 - 30 * ((i % 200) - 100)
                                       for i in range(2048)]))
    out.append(('exact triangle 100, one lost',
                [500 + 30 * (i % 200) if (i % 200) < 100 else 3500 - 30 * ((i % 200) - 100)
                 for i in range(2048) if i != 1000]))
    out.append(('overflow: 400 turning points', [500 + 600 * (i % 5) if (i // 5) % 2 == 0 else 2900 - 600 * (i % 5)
                                                 for i in range(2048)]))
    rnd = eval_chain.random.Random(7)
    out.append(('white noise', [rnd.randint(0, 4095) for _ in range(2048)]))
    out.append(('slow sine', [int(2048 + 1500 * math.sin(i / 300.0)) for i in range(2048)]))
    out.append(('ramp only, no turn', list(range(0, 2048))))
    out.append(('clipped triangle', [max(0, min(4095, int(-500 + 5000 * abs(((i / 400.0) % 2.0) - 1.0))))
                                     for i in range(2048)]))
    return out


def run_c(exe, wins):
    text = '\n'.join(' '.join(str(v) for v in w) for _, w in wins) + '\n'
    p = subprocess.run([exe, '--eval'], input=text, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f'{exe} --eval failed (rc {p.returncode}): {p.stderr.strip()}')
    lines = [ln for ln in p.stdout.splitlines() if ln.strip()]
    if len(lines) != len(wins):
        sys.exit(f'{exe} returned {len(lines)} results for {len(wins)} windows')
    res = []
    for ln in lines:
        d = {}
        for key, val in LINE_RE.findall(ln):
            # %.9g round-trips a float exactly once the decimal is rounded
            # back to float32 - the parsed double alone is off by up to
            # half a unit in the 9th digit (about 0.07 float ulp).
            d[key] = to_f32(float(val)) if key in FLT_FIELDS else int(val)
        res.append(d)
    return res


def run_py(wins):
    res = []
    for _, w in wins:
        r = dict(eval_chain.tri_eval(w))
        r['grid_ok'] = int(eval_chain.grid_ok(r))
        r['step_checked'] = int(r['step_checked'])
        r['overflow'] = int(r['overflow'])
        res.append(r)
    return res


def compare(c, p):
    """None if identical as defined above, else a list of (field, c, py, limit)."""
    bad = []
    for f in INT_FIELDS:
        if c[f] != p[f]:
            bad.append((f, c[f], p[f], 'equal'))
    worst = {}
    for f in FLT_FIELDS:
        diff = abs(c[f] - to_f32(p[f]))
        tol = tolerance(f, p)
        worst[f] = (diff / tol, diff)
        if diff > tol:
            bad.append((f, c[f], p[f], f'{tol:.3g} abs'))
    return (bad or None), worst


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'host', 'test_tri_eval.exe')
    if not os.path.exists(exe):
        sys.exit(f'no {exe} - run tools\\hosttest.bat first (it builds it, then runs this script)')
    wins = windows()
    cres = run_c(exe, wins)
    pres = run_py(wins)
    worst = {f: (0.0, 0.0) for f in FLT_FIELDS}
    verdicts = [0, 0]
    for i, ((desc, w), c, p) in enumerate(zip(wins, cres, pres)):
        bad, u = compare(c, p)
        for f in FLT_FIELDS:
            worst[f] = max(worst[f], u[f])
        verdicts[c['grid_ok']] += 1
        if bad:
            path = os.path.join(ROOT, 'build', 'host', 'xcheck_mismatch.txt')
            with open(path, 'w') as fh:
                fh.write(f'window {i}: {desc}\n')
                for f, cv, pv, lim in bad:
                    fh.write(f'  {f}: C {cv!r}  Python {pv!r}  (limit {lim})\n')
                fh.write('C:      ' + ' '.join(f'{k}={c[k]}' for k in INT_FIELDS + FLT_FIELDS) + '\n')
                fh.write('Python: ' + ' '.join(f'{k}={p[k]}' for k in INT_FIELDS + FLT_FIELDS) + '\n')
                fh.write(' '.join(str(v) for v in w) + '\n')
            print(f'MISMATCH on window {i} ({desc}):')
            for f, cv, pv, lim in bad:
                print(f'  {f}: C {cv!r}  Python {pv!r}  (limit {lim})')
            print(f'window and both results written to {path}')
            print('xcheck FAIL')
            return 1
    print(f'{len(wins)} windows compared ({verdicts[1]} grid ok, {verdicts[0]} not), '
          'every integer field equal; largest float deviation as a fraction of its '
          'tolerance: ' + ', '.join(f'{f} {worst[f][0]:.2f}' for f in FLT_FIELDS)
          + f'; dev at most {worst["dev"][1]:.3g} samples absolute')
    print('xcheck PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
