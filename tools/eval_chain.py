#!/usr/bin/env python3
"""Evaluate the UART log of the firmware's chain test ("chain all").

    python tools/eval_chain.py <log.txt>            report on the console
    python tools/eval_chain.py <log.txt> --png DIR  plus one plot per @DUMP window
    python tools/eval_chain.py --selftest           check the evaluator itself

The firmware prints every result as one line

    @S<stage>.<n> key=value ... -> PASS|FAIL|SKIP|INFO

and a window whose verdict is FAIL as "@DUMP ..." followed by "@D" lines of
64 samples in 3-digit hex. This script re-judges every verdict from the
fields the line carries (so that a threshold that turns out to be wrong in
the firmware does not cost another board run), re-evaluates every dumped
window with the same triangle method the firmware uses (tri_eval in
chaintest.c, ported below), and lists what the run says about the open
questions of docs/ANALYSIS.md.

Fields named *_x100 / *_x1000 are fixed point. Plan and stage list:
docs/CHAIN-TEST-PLAN.md.
"""
import argparse
import math
import os
import random
import re
import sys

LINE_RE = re.compile(r'^@S(\d+)\.(\d+)(.*?)\s*->\s*(PASS|FAIL|SKIP|INFO)\s*$')
FIELD_RE = re.compile(r'(\w+)=(\S+)')
DUMP_RE = re.compile(r'^@DUMP S(\d+)\.(\d+).*\bn=(\d+)')

STAGE_NAMES = {
    0: 'preconditions', 1: 'SCCP1 alone', 2: 'SCCP1 -> ADC (low rate)',
    3: 'ADC -> DMA -> buffer', 4: 'counting at rate', 5: 'grid in the data',
    6: 'stream with the CPU', 7: 'start / stop / rate change',
    8: 'old open questions', 9: 'the attempt',
}


def num(v):
    try:
        return int(v, 0)
    except ValueError:
        return v


# --------------------------------------------------------------------------
# Log parsing
# --------------------------------------------------------------------------
class Result:
    def __init__(self, stage, n, fields, verdict, text):
        self.stage, self.n, self.f, self.verdict, self.text = stage, n, fields, verdict, text

    def __repr__(self):
        return f'S{self.stage}.{self.n} {self.verdict}'


def parse(path):
    results, dumps, sums, recipe, other = [], [], [], [], []
    ended = False
    in_recipe = False
    cur_dump = None
    with open(path, encoding='utf-8', errors='replace') as fh:
        for raw in fh:
            line = raw.rstrip('\r\n')
            if line.startswith('@RECIPE'):
                in_recipe = 'end' not in line
                continue
            if in_recipe:
                recipe.append(line)
                continue
            m = LINE_RE.match(line)
            if m:
                fields = {k: num(v) for k, v in FIELD_RE.findall(m.group(3))}
                results.append(Result(int(m.group(1)), int(m.group(2)), fields, m.group(4), line))
                cur_dump = None
                continue
            m = DUMP_RE.match(line)
            if m:
                cur_dump = {'stage': int(m.group(1)), 'n': int(m.group(2)),
                            'len': int(m.group(3)), 'data': []}
                dumps.append(cur_dump)
                continue
            if line.startswith('@D ') and cur_dump is not None:
                hx = line[3:].strip()
                cur_dump['data'].extend(int(hx[i:i + 3], 16) for i in range(0, len(hx) - 2, 3))
                continue
            if line.startswith('@SUM'):
                sums.append(line)
            elif line.startswith('@END'):
                ended = True
            elif line.startswith('[boot]') or line.startswith('[TRAP]') or line.startswith('[CLKF]'):
                other.append(line)
    return results, dumps, sums, recipe, other, ended


# --------------------------------------------------------------------------
# The triangle evaluator - a port of tri_eval() in chaintest.c
# --------------------------------------------------------------------------
TP_MAX = 160
STEP_CHECK_LSB = 40.0
GRID_SLIP_MAX = 0.5


def fit_line(x, a, b):
    if b <= a or (b - a) < 5:
        return None
    c = 0.5 * (a + b)
    n = b - a + 1
    sx = sy = sxx = sxy = 0.0
    for i in range(a, b + 1):
        u = i - c
        y = float(x[i])
        sx += u; sy += y; sxx += u * u; sxy += u * y
    den = n * sxx - sx * sx
    if den == 0.0:
        return None
    m = (n * sxy - sx * sy) / den
    return m, (sy - m * sx) / n - m * c


def tri_eval(x):
    n = len(x)
    r = dict(mn=min(x), mx=max(x), tps=0, n_up=0, n_dn=0, l_up=0.0, l_dn=0.0, dev=0.0,
             step=0.0, zero=0, dbl=0, step_checked=False, overflow=False,
             slip=99.0, slip_k=0, slip_n=0, tp_pos=[])
    r['pp'] = r['mx'] - r['mn']
    if n < 16:
        return r
    h = max(r['pp'] // 4, 8)
    direction = 0
    hi_v = lo_v = x[0]; hi_i = lo_i = 0
    ext_v, ext_i = x[0], 0
    tp = []
    for i in range(1, n):
        v = x[i]
        if direction == 0:
            if v > hi_v: hi_v, hi_i = v, i
            if v < lo_v: lo_v, lo_i = v, i
            if v + h < hi_v:
                if hi_i > 2 and len(tp) < TP_MAX: tp.append(hi_i)
                direction, ext_v, ext_i = -1, v, i
            elif v > lo_v + h:
                if lo_i > 2 and len(tp) < TP_MAX: tp.append(lo_i)
                direction, ext_v, ext_i = 1, v, i
        elif direction > 0:
            if v >= ext_v: ext_v, ext_i = v, i
            elif ext_v - v > h:
                if len(tp) < TP_MAX: tp.append(ext_i)
                else: r['overflow'] = True
                direction, ext_v, ext_i = -1, v, i
        else:
            if v <= ext_v: ext_v, ext_i = v, i
            elif v - ext_v > h:
                if len(tp) < TP_MAX: tp.append(ext_i)
                else: r['overflow'] = True
                direction, ext_v, ext_i = 1, v, i
    ntp = len(tp)
    r['tps'] = ntp
    if ntp < 2:
        return r
    d = sorted(tp[j] - tp[j - 1] for j in range(1, ntp))
    med = d[len(d) // 2] if d else n
    seg = []
    steps = []
    for s in range(ntp + 1):
        a = 0 if s == 0 else tp[s - 1]
        b = n - 1 if s == ntp else tp[s]
        if s == 0 and b > med: a = b - med
        if s == ntp and b - a > med: b = a + med
        length = b - a
        m = max(length // 8, 2)
        f = fit_line(x, a + m, b - m) if length > 2 * m + 5 else None
        seg.append(f)
        if f is not None and 0 < s < ntp:
            steps.append(abs(f[0]))
    r['step'] = sum(steps) / len(steps) if steps else 0.0
    if r['step'] >= STEP_CHECK_LSB:
        r['step_checked'] = True
        for s in range(1, ntp):
            if seg[s] is None:
                continue
            a, b = tp[s - 1], tp[s]
            m = max((b - a) // 8, 2)
            sl = seg[s][0]
            for i in range(a + m, b - m):
                dd = (x[i + 1] - x[i]) * (1 if sl >= 0 else -1)
                if dd < 0.5 * abs(sl): r['zero'] += 1
                elif dd > 1.5 * abs(sl): r['dbl'] += 1
    pos = []
    for j in range(ntp):
        f1, f2 = seg[j], seg[j + 1]
        if f1 and f2 and f1[0] != f2[0]:
            pos.append((f2[1] - f1[1]) / (f1[0] - f2[0]))
        else:
            pos.append(None)
    r['tp_pos'] = pos
    up, dn = [], []
    for s in range(1, ntp):
        if pos[s - 1] is None or pos[s] is None or seg[s] is None:
            continue
        (up if seg[s][0] > 0 else dn).append(pos[s] - pos[s - 1])
    r['n_up'], r['n_dn'] = len(up), len(dn)
    r['l_up'] = sum(up) / len(up) if up else 0.0
    r['l_dn'] = sum(dn) / len(dn) if dn else 0.0
    devs = [abs(L - r['l_up']) for L in up] + [abs(L - r['l_dn']) for L in dn]
    r['dev'] = max(devs) if devs else 0.0
    first, last = 1, ntp - 2
    inner = last - first + 1 if ntp >= 3 else 0
    k = 4 if inner >= 5 else 2
    per = sorted(pos[j + 2] - pos[j] for j in range(first, last - 1)
                 if inner and pos[j] is not None and pos[j + 2] is not None)
    if not per:
        return r
    T = per[len(per) // 2]
    spans = [abs((pos[j + k] - pos[j]) - (k // 2) * T) for j in range(first, last - k + 1)
             if pos[j] is not None and pos[j + k] is not None]
    if spans:
        r['slip'], r['slip_k'], r['slip_n'] = max(spans), k, len(spans)
        r['T'] = T
    return r


def grid_ok(r):
    return (r['n_up'] >= 1 and r['n_dn'] >= 1 and not r['overflow'] and r['slip_n'] != 0
            and r['slip'] < GRID_SLIP_MAX and r['zero'] == 0 and r['dbl'] == 0)


# --------------------------------------------------------------------------
# Re-judging the lines
# --------------------------------------------------------------------------
def rejudge(res):
    """The verdict recomputed from the fields, or None if the line is not one
    this script knows how to judge. Returns (verdict, reason)."""
    f, s, n = res.f, res.stage, res.n
    g = f.get
    if s == 0 and 4 <= n <= 8 and 'hz' in f:
        if f['hz'] == 0:
            return 'INFO', 'clock monitor counted nothing'
        ok = abs(f['hz'] - f['expect']) <= f['expect'] / 200
        return ('PASS' if ok else 'FAIL'), f"{f['hz'] / 1e6:.3f} MHz vs {f['expect'] / 1e6:.3f}"
    if s == 1 and n == 1:
        ok = abs(f['sccp_hz'] - 160000000) <= 800000
        return ('PASS' if ok else 'FAIL'), f"SCCP1 clock {f['sccp_hz'] / 1e6:.3f} MHz"
    if s == 1 and 'mode' in f:
        got = f['cct1'] if f['mode'] == 'timer' else f['ccp1']
        return ('PASS' if abs(got - f['expect']) <= 2 else 'FAIL'), f"{got} events vs {f['expect']}"
    if s == 2 and 'adc_results' in f:
        ok = f['sccp_events'] > 0 and abs(f['sccp_events'] - f['adc_results']) <= 1
        ratio = f['adc_results'] / f['sccp_events'] if f['sccp_events'] else 0
        return ('PASS' if ok else 'FAIL'), f"ADC results per SCCP event {ratio:.3f}"
    if s == 4 and 'xfer' in f:
        ok = abs(f['xfer'] - f['expect']) <= f['tol'] and f['overrun'] == 0 and f['brake'] == 0
        ratio = f['xfer'] / f['expect'] if f['expect'] else 0
        return ('PASS' if ok else 'FAIL'), f"transfers/trigger {ratio:.4f}, overrun {f['overrun']}"
    if s == 5 and 'slip_x100' in f:
        ok = (g('up', 0) >= 1 and g('dn', 0) >= 1 and g('slip_n', 0) > 0 and f['slip_x100'] < 50
              and g('zero', 0) == 0 and g('dbl', 0) == 0 and 'overflow' not in f)
        extra = ''
        if 'ratio_x1000' in f:
            extra = f", slope/model {f['ratio_x1000'] / 1000:.3f}"
        return ('PASS' if ok else 'FAIL'), f"slip {f['slip_x100'] / 100:.2f} samples{extra}"
    if s in (6, 9) and 'load_max_x10' in f:
        ok = (f['overrun'] == 0 and f['late'] == 0 and f['missed'] == 0 and f['brake'] == 0
              and f['guard_ok'] == 1 and abs(f['xfer'] - f['expect']) <= f['tol']
              and f['isr'] == f['half'] + f['done'])
        return ('PASS' if ok else 'FAIL'), (f"load {f['load_max_x10'] / 10:.1f} %, "
                                             f"free {f['free_cyc_per_sample']} cycles/sample")
    return None


# --------------------------------------------------------------------------
# Report
# --------------------------------------------------------------------------
def by(results, stage, n=None):
    return [r for r in results if r.stage == stage and (n is None or r.n == n)]


def report(path, png_dir=None):
    results, dumps, sums, recipe, other, ended = parse(path)
    out = []
    p = out.append
    p(f'Log: {path}')
    p(f'  {len(results)} result lines, {len(dumps)} dumps, recipe {"yes" if recipe else "no"}, '
      f'{"ended with @END" if ended else "NO @END - the run stopped early"}')
    for o in other:
        if 'WARNING' in o or 'TRAP' in o or 'CLKF' in o:
            p(f'  boot: {o}')
    build = [r for r in results if 'build' in r.text]
    if build:
        p('  ' + build[0].text.split('build=', 1)[-1].split(' ->')[0])

    p('')
    p('Stages')
    for s in range(10):
        rs = by(results, s)
        if not rs:
            continue
        cnt = {v: sum(1 for r in rs if r.verdict == v) for v in ('PASS', 'FAIL', 'SKIP', 'INFO')}
        p(f'  S{s} {STAGE_NAMES[s]:<28} pass {cnt["PASS"]:2}  fail {cnt["FAIL"]:2}  '
          f'skip {cnt["SKIP"]:2}  info {cnt["INFO"]:2}')

    p('')
    p('Re-judged from the fields (differences from the firmware marked !!)')
    for r in results:
        j = rejudge(r)
        if j is None:
            continue
        v, why = j
        mark = '  ' if v == r.verdict or r.verdict in ('INFO', 'SKIP') else '!!'
        p(f' {mark} S{r.stage}.{r.n:<2} fw {r.verdict:<4} here {v:<4}  {why}')

    fails = [r for r in results if r.verdict == 'FAIL']
    p('')
    p(f'Failures ({len(fails)})')
    for r in fails:
        p('  ' + r.text)

    p('')
    p('What the run says about the open questions')
    s0 = {r.f.get('cm'): r for r in by(results, 0) if 'cm' in r.f}
    if 'clkgen6_adc' in s0:
        f = s0['clkgen6_adc'].f
        p(f"  ADC clock (CLKGEN6) measured by the clock monitor: {f['hz'] / 1e6:.3f} MHz")
    if 'clkgen7_dac' in s0:
        f = s0['clkgen7_dac'].f
        p(f"  DAC clock (CLKGEN7): {f['hz'] / 1e6:.3f} MHz (spec 400..500)")
    for r in by(results, 1, 1):
        p(f"  SCCP1 clock: {r.f['sccp_hz'] / 1e6:.3f} MHz (spec <= 200)")
    for r in by(results, 8):
        if 'clk6div_x100' in r.f:
            p(f"  CLK6DIV {r.f['clk6div_x100'] / 100:.1f}: CLKGEN6 {r.f['clkgen6_hz'] / 1e6:.3f} MHz, "
              f"expected {r.f['expect'] / 1e6:.3f} -> {r.verdict}")
        if 'cm_hz' in r.f:
            p(f"  CLKGEN6 switched off: monitor {r.f['cm_hz']} Hz, probe rc {r.f['probe_rc']}, "
              f"{r.f['samples_written']} samples written, pp {r.f['buffer_pp']}")
        if r.f.get('bursts') is not None and 'Lup_x100' in r.f:
            p(f"  back-to-back pll {r.f['pll']} bursts {r.f['bursts']:>3}: slope "
              f"{r.f['Lup_x100'] / 100:.2f}/{r.f['Ldn_x100'] / 100:.2f} samples, model "
              f"{r.f['model_x100'] / 100:.2f}, measured {r.f['ksps_measured']} kSPS")
    ratios = [(r.f['ksps'], r.f['ratio_x1000'] / 1000) for r in by(results, 5) if 'ratio_x1000' in r.f]
    if ratios:
        p('  triangle slope, measured / model (open question 4): ' +
          ', '.join(f'{k} kSPS {q:.3f}' for k, q in ratios))

    p('')
    p('Summary lines of the firmware')
    for sline in sums:
        p('  ' + sline)

    if dumps:
        p('')
        p('Dumped windows, re-evaluated here')
        for d in dumps:
            # The firmware judged the window from its "from" index on: at a
            # high rate the first samples of the buffer are newer ones that
            # arrived before the trigger was off (grab_window in chaintest.c).
            src = [r for r in results if r.stage == d['stage'] and r.n == d['n']]
            start = src[0].f.get('from', 0) if src else 0
            x = d['data'][start:d['len']]
            r = tri_eval(x)
            p(f"  S{d['stage']}.{d['n']} from={start} n={len(x)} pp={r['pp']} tp={r['tps']} "
              f"up={r['n_up']} {r['l_up']:.2f} dn={r['n_dn']} {r['l_dn']:.2f} "
              f"slip={r['slip']:.2f} (k={r['slip_k']}, spans {r['slip_n']}) "
              f"zero={r['zero']} dbl={r['dbl']} -> {'PASS' if grid_ok(r) else 'FAIL'}")
            if png_dir:
                plot(d, x, r, png_dir)
    print('\n'.join(out))


def plot(d, x, r, png_dir):
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except ImportError:
        print('  (matplotlib missing - no plots)')
        return
    os.makedirs(png_dir, exist_ok=True)
    fig, ax = plt.subplots(figsize=(12, 4))
    ax.plot(x, lw=0.7)
    for pos in r['tp_pos']:
        if pos is not None:
            ax.axvline(pos, color='tab:red', lw=0.4)
    ax.set_title(f"S{d['stage']}.{d['n']}  slip {r['slip']:.2f}  pp {r['pp']}")
    ax.set_xlabel('sample'); ax.set_ylabel('ADC count')
    fn = os.path.join(png_dir, f"dump_S{d['stage']}_{d['n']}.png")
    fig.tight_layout(); fig.savefig(fn, dpi=110); plt.close(fig)
    print(f'  plot: {fn}')


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------
def synth(n, slope, phase, drop=None, dup=None, tau=0.0, seed=1):
    rnd = random.Random(seed)
    dnl = [rnd.randint(-5, 5) for _ in range(4096)]
    lo, hi = 300.0, 3800.0
    al = 1.0 - math.exp(-1.0 / tau) if tau > 0 else 1.0

    def tri(t):
        p = math.fmod(t + 4000 * slope, 2 * slope)
        return lo + (hi - lo) * p / slope if p < slope else hi - (hi - lo) * (p - slope) / slope

    yf, t = lo, phase - 200.0
    while t < phase:
        yf += al * (tri(t) - yf); t += 1.0
    out = []
    while len(out) < n:
        if drop is not None and len(out) == drop:
            t += 1.0; drop = None
        code = int(math.floor(tri(t) + 0.5))
        yf += al * ((code + dnl[code & 4095]) - yf)
        out.append(max(0, min(4095, int(yf + rnd.gauss(0, 1.5) + 0.5))))
        if dup is not None and len(out) == dup:
            out.append(out[-1]); dup = None
        t += 1.0
    return out[:n]


def selftest():
    cases = [
        ('clean 128', dict(slope=127.5, phase=40.0), True),
        ('clean 29', dict(slope=29.0, phase=3.0), True),
        ('clean 40 MSPS, filter', dict(slope=126.8, phase=77.0, tau=8.0), True),
        ('lost sample', dict(slope=127.5, phase=40.0, drop=1000), False),
        ('repeated sample', dict(slope=127.5, phase=40.0, dup=1000), False),
        ('lost, short slopes', dict(slope=29.0, phase=3.0, drop=700), False),
    ]
    bad = 0
    for name, kw, want in cases:
        r = tri_eval(synth(2048, **kw))
        got = grid_ok(r)
        ok = got == want
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'} {name:<24} slip {r['slip']:.3f} "
              f"(k={r['slip_k']}, {r['slip_n']} spans) -> {'PASS' if got else 'FAIL'}")
    print('selftest', 'PASS' if bad == 0 else f'FAIL ({bad})')
    return bad == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('log', nargs='?')
    ap.add_argument('--png', metavar='DIR')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        sys.exit(0 if selftest() else 1)
    if not a.log:
        ap.error('a log file, or --selftest')
    report(a.log, a.png)


if __name__ == '__main__':
    main()
