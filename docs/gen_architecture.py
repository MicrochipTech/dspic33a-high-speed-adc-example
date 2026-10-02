"""Generate the two architecture diagrams embedded in docs/ARCHITECTURE.md.

    python docs/gen_architecture.py

writes architecture_layers.svg and architecture_datapath.svg next to this file.
Standalone SVGs: colours and fonts are inside each file (a light palette plus a
prefers-color-scheme dark one), no web font is loaded, so they render the same
through an <img> on GitHub/Bitbucket and in the VS Code Markdown preview.

When a module is added, moved or renamed (CLAUDE.md's module table), change the
box here and regenerate; do not edit the SVGs by hand.

The dot in a box (docs/test_status.json) says one thing: green = the module was
functionally tested on silicon with the current version, grey ring = not tested
with this version yet (no verdict). Green needs evidence with a revision - a
board run, or a dated docs/HARDWARE-LOG.md entry that exercised the module - and
turns grey again by itself when one of the box's files changes after it.

    python docs/gen_architecture.py --apply-run <board-run session zip>

turns every box green whose board-run blocks all passed in B (tools/eval_board.py)
and that has nothing in scope left open;

    python docs/gen_architecture.py --mark-tested REV "EVIDENCE" "BOX" ["BOX" ...]

records a manual board test (HARDWARE-LOG) for the named boxes. Both write
docs/test_status.json and regenerate. See its "_comment" for the fields.
"""
import argparse
import html
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
E = html.escape

STYLE = """<style>
svg{--bg:#f4f6f8;--ink:#17202b;--muted:#566273;--line:#b9c2cd;--surface:#fff;--accent:#0d7a6b;
--b-host:#eceef1;--s-host:#8b95a3;--b-cli:#e5edf9;--s-cli:#4f74b3;--b-test:#f0e9f8;--s-test:#8062ad;
--b-app:#e2f2ee;--s-app:#2f8a78;--b-lib:#f8efe0;--s-lib:#b07a2a;--s-diag:#b0574d;
--b-port:#ebeae6;--s-port:#7c7768;--b-drv:#e7f0e1;--s-drv:#5c8a3f;--b-hw:#e3e7ec;--s-hw:#4a5868;
--st-tested:#2e9d57;--st-pending:#9aa3ad}
@media (prefers-color-scheme:dark){svg{--bg:#11161c;--ink:#e3e8ee;--muted:#9aa6b4;--line:#3a4552;
--surface:#1a2129;--accent:#3cc2ab;--b-host:#1a1f25;--s-host:#6f7b89;--b-cli:#172236;--s-cli:#7fa3e0;
--b-test:#221b2e;--s-test:#a88bd6;--b-app:#12251f;--s-app:#52b8a2;--b-lib:#2a2215;--s-lib:#d6a352;
--s-diag:#d98277;--b-port:#22211d;--s-port:#a8a292;--b-drv:#18231a;--s-drv:#86b865;--b-hw:#1b2027;--s-hw:#8c9aab;
--st-tested:#4cc47a;--st-pending:#6f7b89}}
.bg{fill:var(--bg)} text{fill:var(--ink)}
.b-host{fill:var(--b-host)}.b-cli{fill:var(--b-cli)}.b-test{fill:var(--b-test)}.b-app{fill:var(--b-app)}
.b-lib{fill:var(--b-lib)}.b-port{fill:var(--b-port)}.b-drv{fill:var(--b-drv)}.b-hw{fill:var(--b-hw)}
.bx{fill:var(--surface);stroke-width:1.3}
.s-host{stroke:var(--s-host)}.s-cli{stroke:var(--s-cli)}.s-test{stroke:var(--s-test)}.s-app{stroke:var(--s-app)}
.s-lib{stroke:var(--s-lib)}.s-diag{stroke:var(--s-diag)}.s-port{stroke:var(--s-port)}.s-drv{stroke:var(--s-drv)}.s-hw{stroke:var(--s-hw)}
.dash{stroke-dasharray:5 4}
.t{font:600 12.5px Consolas,"Cascadia Mono","DejaVu Sans Mono",monospace}
.s{font:400 11px "Segoe UI",Arial,"Helvetica Neue",sans-serif;fill:var(--muted)}
.ln{font:600 13px "Segoe UI",Arial,"Helvetica Neue",sans-serif}
.lf{font:400 10.5px Consolas,"Cascadia Mono","DejaVu Sans Mono",monospace;fill:var(--muted)}
.an{font:500 11px "Segoe UI",Arial,"Helvetica Neue",sans-serif;fill:var(--accent)}
.ar,.ar2{fill:none;stroke:var(--accent);stroke-width:1.6;marker-end:url(#arr)}
.ar2{marker-start:url(#arrs)} .ar.dash{stroke-dasharray:5 4}
.ln1{stroke:var(--line);stroke-width:1.3} .mk{fill:var(--accent)}
.st-tested{fill:var(--st-tested)}.st-pending{fill:none;stroke:var(--st-pending);stroke-width:1.6}
</style>
<defs>
<marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path class="mk" d="M0 0 L10 5 L0 10 z"/></marker>
<marker id="arrs" viewBox="0 0 10 10" refX="1" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path class="mk" d="M10 0 L0 5 L10 10 z"/></marker>
</defs>"""


# Test status per box: docs/test_status.json (its "_comment" explains every
# field). Green means fully tested on silicon with the current code; a box
# turns green through --apply-run <session zip> (below) once every board-run
# block that covers it passed in B and nothing in scope is left open, and
# drops back to amber on its own as soon as one of its files changes after
# the revision it was tested at. docs/TEST-COVERAGE.md is the prose next to it.
STATUS_FILE = os.path.join(HERE, "test_status.json")
REPO = os.path.dirname(HERE)


def load_status(path=STATUS_FILE):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def changed_since(rev, files):
    """True when any of `files` differs between `rev` and the working tree
    (committed or not) - the code a green box was tested with is gone."""
    if not rev or not files:
        return False
    # The whole tree with rename detection, not a pathspec: a file moved
    # unchanged (CORE.4, 02.10.2026: src/app -> src/core, ...) is an R100 and
    # keeps its box green; a pathspec of new paths would see it as added.
    r = subprocess.run(["git", "-C", REPO, "diff", "-M", "--name-status", rev],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return True
    wanted = set(files)
    for line in r.stdout.splitlines():
        parts = line.split("\t")
        if parts[0] == "R100":
            continue
        if wanted.intersection(parts[1:]):
            return True
    return False


def effective(box):
    """(status, note) as drawn. Green ("tested") only with a revision it was
    tested at and no change to its files since - otherwise it is drawn as
    "pending" (grey ring): not tested with this version yet, which says
    nothing about whether it works."""
    st = box["status"]
    if st == "tested":
        if not box.get("rev"):
            return "pending", "no tested revision recorded"
        if changed_since(box.get("rev"), box.get("files", [])):
            return "pending", f"changed since it was tested at {box['rev']}"
        return "tested", f"at {box['rev']} - {box.get('evidence', '')}".rstrip(" -")
    return "pending", box.get("evidence", "")


STATUS_DATA = load_status()
STATUS_AS_OF = STATUS_DATA["as_of"]
STATUS = STATUS_DATA["boxes"]
LABEL = {"tested": "functionally tested on silicon with this version",
         "pending": "not tested with this version yet"}


def marks(x, y, w, title):
    box = STATUS.get(title)
    if not box:
        return ""
    status, note = effective(box)
    tip = LABEL[status] + (f" ({note})" if note else "")
    if box.get("open"):
        tip += "; open: " + "; ".join(box["open"])
    cx, cy = x + w - 11, y + 11
    return f'<circle class="st-{status}" cx="{cx}" cy="{cy}" r="5"><title>{E(tip)}</title></circle>'


def apply_run(zip_path, data):
    """Turn every box green whose blocks all passed in B with no deviation.
    Returns (promoted, held) - held = [(title, reason)] for the boxes that
    stay as they are, so the caller can print why."""
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import eval_board  # noqa: E402  (tools/ on the path above)
    log_a, log_b = eval_board.load_session_zip_logs(zip_path)
    try:
        expected = eval_board.load_expected()
    except (OSError, ValueError):
        expected = None
    result = eval_board.evaluate(log_a, log_b, expected)
    bad = {d["block"] for d in result["deviations"]}
    if result["expectation_deviations"]:
        bad.add("R2")          # every expectation in expected.json is a chain-all line
    passed = {b for b, v in result["verdicts_b"].items() if v == "ok" and b not in bad}
    rev = None       # the B image's revision: what the firmware boxes were tested at
    tools_rev = None  # the runner's own checkout: what the host-tool boxes were tested at
    try:
        import zipfile
        with zipfile.ZipFile(zip_path) as z:
            session = json.loads(z.read("session.json"))
        tools_rev = (session.get("git") or {}).get("head")
        for h in session.get("hex", []):
            m = re.match(r"B-[^-]+-([0-9a-f]{7,40})\.hex$", os.path.basename(h.get("path") or ""))
            if m:
                rev = m.group(1)
    except (KeyError, ValueError, OSError):
        pass
    promoted, held = [], []
    for title, box in data["boxes"].items():
        if effective(box)[0] == "tested":
            continue
        if not box["blocks"]:
            held.append((title, "no board-run block reaches it"))
        elif box["open"]:
            held.append((title, "open: " + "; ".join(box["open"])))
        elif not set(box["blocks"]) <= passed:
            held.append((title, "not passed in B: " + ", ".join(sorted(set(box["blocks"]) - passed))))
        else:
            host = all(f.startswith("tools/") for f in box.get("files", [])) and box.get("files")
            box.update(status="tested", rev=tools_rev if host else rev,
                       evidence=f"board run {os.path.basename(zip_path)}")
            promoted.append(title)
    data["as_of"] = f"after {os.path.basename(zip_path)}"
    return promoted, held


def legend(x, y):
    items = [("st-tested", LABEL["tested"]),
             ("st-pending", LABEL["pending"])]
    s = [f'<text class="s" x="{x}" y="{y+4}">Test status, {E(STATUS_AS_OF)} (docs/TEST-COVERAGE.md):</text>']
    y += 20
    cx = x + 6
    for cls, label in items:
        s.append(f'<circle class="{cls}" cx="{cx}" cy="{y}" r="5"/>')
        s.append(f'<text class="s" x="{cx+10}" y="{y+4}">{E(label)}</text>')
        cx += 34 + int(len(label) * 5.4)
    return "\n".join(s)


def box(x, y, w, h, title, lines=(), cls="app", dashed=False):
    s = [f'<rect class="bx s-{cls}{" dash" if dashed else ""}" x="{x}" y="{y}" width="{w}" height="{h}" rx="4"/>']
    s.append(marks(x, y, w, title))
    lines = [l for l in lines if l]
    lh = 14
    top = y + h / 2 - ((1 + len(lines)) * lh) / 2 + 11
    s.append(f'<text class="t" x="{x+10}" y="{top:.0f}">{E(title)}</text>')
    for i, l in enumerate(lines):
        s.append(f'<text class="s" x="{x+10}" y="{top+(i+1)*lh:.0f}">{E(l)}</text>')
    return "\n".join(s)


def band(y, h, cls, name, folder, x=10, w=1140):
    s = [f'<rect class="b-{cls}" x="{x}" y="{y}" width="{w}" height="{h}" rx="6"/>',
         f'<text class="ln" x="24" y="{y+24}">{E(name)}</text>']
    for i, f in enumerate(folder):
        s.append(f'<text class="lf" x="24" y="{y+40+i*13}">{E(f)}</text>')
    return "\n".join(s)


def svg(w, h, label, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
            f'role="img" aria-label="{E(label)}">\n{STYLE}\n'
            f'<rect class="bg" x="0" y="0" width="{w}" height="{h}"/>\n{body}\n</svg>\n')


def layers():
    d = []
    d.append(band(16, 64, "host", "Host", ["tools/", "off-target"]))
    for i, (t, l) in enumerate([
            ("adc_gui.py", ["NiceGUI: stream on / grab, time + FFT"]),
            ("board_run.py · eval_board.py", ["board run A/B, list of deviations"]),
            ("remote.py → bench_client", ["relay: flash, tunnel (socket://)"]),
            ("terminal", ["console by hand"])]):
        d.append(box(170 + i * 245, 26, 233, 44, t, l, "host", True))
    d.append('<path class="ar2" d="M300 72 V120"/>')
    d.append('<text class="an" x="312" y="100">UART2 · text commands with ACK/NAK, binary frames (blk, GRAB) with CRC-16</text>')

    d.append(band(112, 86, "cli", "Console", ["src/core/"]))
    d.append(box(170, 124, 400, 62, "cli.c · console.h", ["14 core commands + help; the lab adds 15 through", "cli_register_lab() (weak) · 32 parser slots"], "cli"))
    d.append(box(582, 124, 250, 62, "cmd_parser.c", ["upstream zabooh/cmd_parser", "do not edit (MAX_COMMANDS only)"], "cli"))
    d.append(box(844, 124, 296, 62, "gui_link.c", ["stream grab: a frozen pair →", "GRAB frame, the stream runs on"], "cli"))

    d.append(band(214, 86, "test", "Lab", ["src/lab/", "src/sim/", "not in the core build"]))
    for i, (t, l) in enumerate([
            ("chaintest.c", ["chain all S0..S9", "chain run"]),
            ("bench.c", ["sweep · test", "(back-to-back)"]),
            ("dactest.c", ["captured halves vs", "the DAC triangle"]),
            ("meter.c", ["selftest · oneshot", "measure_rate"]),
            ("tri_eval.c", ["triangle evaluator", "(chain test)"]),
            ("cli_lab.c", ["15 lab commands:", "start … chain"]),
            ("b2b_link.c", ["snap · rate · blk", "(back-to-back)"])]):
        d.append(box(170 + i * 140, 226, 130, 62, t, l, "test"))

    d.append(band(316, 176, "app", "Core · app glue", ["src/core/", "glue: src/app/,", "src/boards/"]))
    d.append(box(170, 328, 120, 48, "main.c", ["glue · the lab's"], "app"))
    d.append(box(302, 328, 150, 48, "example_main.c", ["core · customer start"], "app"))
    d.append(box(464, 328, 270, 48, "acquisition.c", ["rate (PLL1), stream on/off, chain setup"], "app"))
    d.append(box(746, 328, 200, 48, "routing.c", ["route_t, resources, apply()"], "app"))
    d.append(box(958, 328, 182, 48, "board.h · board_cfg", ["glue · EV74H48A | EV17P63A"], "app"))
    d.append(box(170, 384, 290, 44, "capture.c", ["pairs A/B + guards · dma0_event() · counters"], "app"))
    d.append(box(472, 384, 218, 44, "sigproc.c", ["lp | hp | bp at fs/8, Goertzel fs/16"], "app"))
    d.append(box(702, 384, 160, 44, "pingpong.c", ["half bookkeeping"], "app"))
    d.append(box(874, 384, 266, 44, "port_impl.c", ["glue · port_* → console / fail()"], "app"))
    d.append(box(170, 440, 970, 44, "siggen.c", ["src/core/ · wavegen table (8192 x 16 bit, .dma_buffer) → dma.c ch. 2 → dac.c, paced by sccp.c (SCCP2); claim in routing.c; no register"], "app"))

    d.append(band(508, 86, "lib", "Libraries", ["src/lib/", "diag.c: src/core/"]))
    d.append(box(170, 520, 380, 62, "frame · crc16 · fmt · stats", ["hardware-free, tested on the host", "(tests/host, Python cross-checks)"], "lib"))
    d.append(box(562, 520, 300, 62, "iir1 · goertzel_f/i · detect · wavegen", ["wavegen: called by siggen.c", "rest linked, not called yet (N+4)"], "lib"))
    d.append(box(874, 520, 266, 62, "diag.c", ["fail() codes, trap, boot record,", "reg_print(), stack high-water mark"], "diag"))

    d.append(band(610, 50, "port", "Port layer", ["src/port/ · headers"]))
    for i, t in enumerate(["log.h · port_log/trace", "panic.h · port_panic()",
                           "wait.h · PORT_WAIT_WHILE()", "regs.h · reg_visit_t"]):
        d.append(box(170 + i * 245, 618, 233, 34, t, (), "port"))

    d.append(band(676, 86, "drv", "Drivers", ["src/drivers/"]))
    drv = [("clock.c", ["PLL1, PLL2", "CLKGEN6/7/13"]), ("adc.c", ["core 5, burst", "trigger, calib."]),
           ("dma.c", ["ch. 0+1 ping-pong,", "ch. 2 tx; sim_dma.c"]), ("sccp.c", ["SCCP1 trigger,", "SCCP2 play clock"]),
           ("dac.c", ["DAC1/2 triangle", "UREF route"]), ("uart.c", ["UART2, PPS", "RX ISR, TX ring"]),
           ("timebase.c", ["Timer1", "stopwatch"]), ("led.c", ["LED0"])]
    hw = [("PLL1 / PLL2", ["PLL1 → ADC path", "PLL2 → CPU"]), ("ADC", ["core 5, AD5AN3", "(pin RA8)"]),
          ("DMA0/1 · DMA2", ["0+1: ADC → A/B", "2: RAM → DAC"]), ("SCCP1 · SCCP2", ["CLKGEN13 160 MHz,", "peripheral 100 MHz"]),
          ("DAC2", ["on CLKGEN7", "400 MHz → RA8"]), ("UART2", ["console", "(COM port)"]),
          ("Timer1", ["12.5 MHz", "on PLL2"]), ("LED0", ["heartbeat"])]
    d.append(band(778, 86, "hw", "Silicon", ["dsPIC33AK512", "MPS512"]))
    for i, ((t, l), (ht, hl)) in enumerate(zip(drv, hw)):
        x = 170 + i * 122
        d.append(box(x, 688, 110, 62, t, l, "drv"))
        d.append(f'<path class="ln1" d="M{x+55} 750 V790"/>')
        d.append(box(x, 790, 110, 62, ht, hl, "hw"))
    d.append('<text class="s" x="20" y="884">Core (CORE, 02.10.2026; docs/CORE.md): src/drivers/, src/port/, src/lib/, src/core/ - what a customer '
             'takes over, with their own glue in place of src/app/ and src/boards/. Lab: src/lab/, src/sim/.</text>')
    d.append(legend(20, 906))
    return svg(1160, 944, "Firmware layers and modules", "\n".join(d))


def datapath():
    g = []
    g.append(band(10, 222, "hw", "", [], w=1170))
    g.append('<text class="ln" x="24" y="32">Silicon: clock and data</text>')
    g.append(band(248, 222, "app", "", [], w=1170))
    g.append('<text class="ln" x="24" y="270">Firmware</text>')
    g.append(box(20, 90, 120, 56, "PLL1", ["VCO 1600 MHz", "÷ p1 · p2"], "hw"))
    g.append(box(180, 40, 140, 44, "CLKGEN13", ["PLL1 / 2 = 160 MHz"], "hw"))
    g.append(box(180, 100, 140, 44, "CLKGEN6", ["ADC clock"], "hw"))
    g.append(box(180, 160, 140, 44, "CLKGEN7", ["VCO divider, 400 MHz"], "hw"))
    g.append(box(360, 40, 150, 44, "SCCP1", ["period = sample rate"], "hw"))
    g.append(box(360, 160, 150, 44, "DAC2", ["triangle, test signal"], "hw"))
    g.append(box(560, 80, 170, 84, "ADC core 5", ["single mode", "PINSEL 3 = AD5AN3", "trigger 0x20 = SCCP1"], "hw"))
    g.append(box(780, 80, 150, 84, "DMA0 + DMA1", ["hardware ping-pong,", "1 transfer / trigger", "ch. 0 ping · ch. 1 pong"], "hw"))
    g.append('<text class="an" x="980" y="52">2 ping-pong pairs in RAM</text>')
    for row, (pair, y) in enumerate((("A", 60), ("B", 112))):
        for col, half in enumerate(("ping", "pong")):
            x = 980 + col * 85
            g.append(f'<rect class="bx s-hw" x="{x}" y="{y}" width="85" height="52" rx="4"/>'
                     f'<text class="t" x="{x + 10}" y="{y + 31}">{pair} {half}</text>')
    g.append('<rect class="bx s-diag" x="980" y="164" width="170" height="20" rx="3"/><text class="s" x="990" y="178">guard words</text>')
    g.append('<text class="an" x="980" y="200">the stream runs in one pair;</text>')
    g.append('<text class="an" x="980" y="214">a grab freezes the other</text>')
    for p in ["M140 118 H160 V62 H180", "M140 118 H160 V122 H180", "M140 118 H160 V182 H180",
              "M320 62 H360", "M320 182 H360", "M320 122 H560", "M510 62 H535 V96 H560",
              "M510 182 H535 V148 H560", "M730 122 H780", "M930 122 H980"]:
        g.append(f'<path class="ar" d="{p}"/>')
    g.append('<text class="an" x="392" y="116">ADC clock</text>')
    g.append('<text class="an" x="516" y="54">trigger</text>')
    g.append('<text class="an" x="516" y="200">RA8</text>')
    g.append('<text class="an" x="738" y="114">result</text>')
    g.append(box(200, 290, 200, 56, "cli.c", ["stream on <ksps> [core pinsel]"], "cli"))
    g.append(box(450, 290, 280, 56, "routing_apply()", ["→ acq_chain_setup_input()", "DMA off, cores off, clock, cores on, DMA anew"], "app"))
    g.append(box(780, 290, 150, 56, "_DMA0Interrupt", ["dma.c · + _DMA1Interrupt", "46 / 41 instr."], "drv"))
    g.append(box(980, 290, 170, 56, "dma0_event()", ["capture.c · pair move", "→ pingpong_on_half()"], "app"))
    g.append(box(200, 390, 200, 56, "adc_gui.py (host)", ["time signal, FFT, tri_eval"], "host", True))
    g.append(box(450, 390, 200, 56, "uart.c · UART2", ["GRAB header, data, CRC"], "drv"))
    g.append(box(700, 390, 250, 56, "gui_link_stream_grab()", ["freeze a pair → frame_send()", "→ release; the stream runs on"], "cli"))
    g.append(box(980, 390, 170, 56, "capture_service()", ["sigproc_block() if on:", "filter fs/8, Goertzel fs/16"], "app"))
    for p in ["M400 318 H450", "M855 164 V290", "M930 318 H980", "M1065 346 V390", "M980 418 H950",
              "M700 418 H650", "M450 418 H400", "M300 390 V346"]:
        g.append(f'<path class="ar" d="{p}"/>')
    g.append('<path class="ar dash" d="M640 290 V164"/>')
    g.append('<path class="ar dash" d="M1150 150 H1168 V418 H1150"/>')
    g.append('<text class="an" x="648" y="244">sets up</text>')
    g.append('<text class="an" x="862" y="244">ping / pong DONE</text>')
    g.append('<text class="an" x="1072" y="372">ready_off</text>')
    g.append('<text class="an" x="308" y="372">command</text>')
    g.append(legend(20, 490))
    return svg(1190, 528, "Data path while streaming", "\n".join(g))


def dark_only(svg_text):
    """The same SVG with the dark palette unconditional: the
    prefers-color-scheme block turned into a plain rule after the light one.
    For GitHub's <picture> in ARCHITECTURE.md - an SVG shown through <img>
    does not reliably see the page's dark mode (iOS Safari on github.com
    drew the adaptive one light, 30.09.2026), GitHub's <picture> source
    selection follows the user's GitHub theme."""
    return re.sub(r"@media \(prefers-color-scheme:dark\)\{(svg\{[^}]*\})\}", r"\1", svg_text)


def write_svgs():
    for name, text in (("architecture_layers.svg", layers()), ("architecture_datapath.svg", datapath())):
        for fname, body in ((name, text), (name.replace(".svg", "_dark.svg"), dark_only(text))):
            path = os.path.join(HERE, fname)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(body)
            print("wrote", path)


if __name__ == "__main__":
    # box titles carry "·" and "→"; a Windows console in cp1252 would stop on them
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply-run", metavar="ZIP",
                    help="a board-run session zip (tools/board_run.py): turn every box green whose "
                         "blocks all passed in B, write docs/test_status.json, then regenerate")
    ap.add_argument("--mark-tested", nargs="+", metavar="ARG",
                    help="REV EVIDENCE BOX [BOX ...]: record a manual board test (a dated "
                         "HARDWARE-LOG entry) of these boxes at git revision REV")
    a = ap.parse_args()
    if a.mark_tested:
        if len(a.mark_tested) < 3:
            ap.error("--mark-tested needs REV EVIDENCE and at least one BOX")
        rev, evidence, titles = a.mark_tested[0], a.mark_tested[1], a.mark_tested[2:]
        data = load_status()
        unknown = [t for t in titles if t not in data["boxes"]]
        if unknown:
            ap.error("no such box: " + ", ".join(unknown) + " - titles: " + ", ".join(data["boxes"]))
        for t in titles:
            data["boxes"][t].update(status="tested", rev=rev, evidence=evidence)
        with open(STATUS_FILE, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write("\n")
        STATUS_DATA.clear()
        STATUS_DATA.update(data)
        STATUS.clear()
        STATUS.update(data["boxes"])
        print("tested at", rev + ":", ", ".join(titles))
    if a.apply_run:
        data = load_status()
        promoted, held = apply_run(a.apply_run, data)
        with open(STATUS_FILE, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write("\n")
        print("green now:", ", ".join(promoted) if promoted else "(none)")
        for title, why in held:
            print(f"  stays: {title} - {why}")
        STATUS_DATA.clear()
        STATUS_DATA.update(data)
        STATUS.clear()
        STATUS.update(data["boxes"])
    for t, b in STATUS.items():
        if b["status"] == "tested" and effective(b)[0] != "tested":
            print(f"  grey again: {t} - {effective(b)[1]}")
    write_svgs()
