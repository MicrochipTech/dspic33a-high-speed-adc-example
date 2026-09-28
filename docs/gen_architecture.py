"""Generate the two architecture diagrams embedded in docs/ARCHITECTURE.md.

    python docs/gen_architecture.py

writes architecture_layers.svg and architecture_datapath.svg next to this file.
Standalone SVGs: colours and fonts are inside each file (a light palette plus a
prefers-color-scheme dark one), no web font is loaded, so they render the same
through an <img> on GitHub/Bitbucket and in the VS Code Markdown preview.

When a module is added, moved or renamed (CLAUDE.md's module table), change the
box here and regenerate; do not edit the SVGs by hand.
"""
import html
import os

HERE = os.path.dirname(os.path.abspath(__file__))
E = html.escape

STYLE = """<style>
svg{--bg:#f4f6f8;--ink:#17202b;--muted:#566273;--line:#b9c2cd;--surface:#fff;--accent:#0d7a6b;
--b-host:#eceef1;--s-host:#8b95a3;--b-cli:#e5edf9;--s-cli:#4f74b3;--b-test:#f0e9f8;--s-test:#8062ad;
--b-app:#e2f2ee;--s-app:#2f8a78;--b-lib:#f8efe0;--s-lib:#b07a2a;--s-diag:#b0574d;
--b-port:#ebeae6;--s-port:#7c7768;--b-drv:#e7f0e1;--s-drv:#5c8a3f;--b-hw:#e3e7ec;--s-hw:#4a5868;
--st-proven:#2e9d57;--st-restructured:#d08a00;--st-never:#d64545}
@media (prefers-color-scheme:dark){svg{--bg:#11161c;--ink:#e3e8ee;--muted:#9aa6b4;--line:#3a4552;
--surface:#1a2129;--accent:#3cc2ab;--b-host:#1a1f25;--s-host:#6f7b89;--b-cli:#172236;--s-cli:#7fa3e0;
--b-test:#221b2e;--s-test:#a88bd6;--b-app:#12251f;--s-app:#52b8a2;--b-lib:#2a2215;--s-lib:#d6a352;
--s-diag:#d98277;--b-port:#22211d;--s-port:#a8a292;--b-drv:#18231a;--s-drv:#86b865;--b-hw:#1b2027;--s-hw:#8c9aab;
--st-proven:#4cc47a;--st-restructured:#f0b030;--st-never:#f06a6a}}
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
.st-proven{fill:var(--st-proven)}.st-restructured{fill:var(--st-restructured)}.st-never{fill:var(--st-never)}
.nooff{fill:none;stroke:var(--st-never);stroke-width:1.6}
</style>
<defs>
<marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path class="mk" d="M0 0 L10 5 L0 10 z"/></marker>
<marker id="arrs" viewBox="0 0 10 10" refX="1" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path class="mk" d="M10 0 L0 5 L10 10 z"/></marker>
</defs>"""


# Test status per box, as of the last board run - the source is
# docs/TEST-COVERAGE.md ("Before N+1"/"N+1 change" and "Off-board" columns);
# update both together after every board run (BR.9). A box that holds several
# modules takes the WORST status among them. Key = the box title exactly.
#   status:   "proven"       ran on silicon, code unchanged since (moves only)
#             "restructured" ran on silicon, N+1 changed the code - board run pending
#             "never"        never ran on silicon in any form
#   offboard: False = no test without a board covers it (TEST-COVERAGE.md "none")
# A title missing here (the silicon boxes, "terminal", unused libraries) gets no mark.
STATUS_AS_OF = "28.09.2026, before the first board run after N+1"
STATUS = {
    # host tools - none has run against a board yet
    "adc_gui.py": ("never", True),
    "adc_gui.py (host)": ("never", True),
    "board_run.py · eval_board.py": ("never", True),
    "remote.py → bench_client": ("never", True),
    # console
    "cli.c · console.h": ("restructured", True),
    "cli.c": ("restructured", True),
    "cmd_parser.c": ("proven", True),
    "gui_link.c": ("never", False),
    "gui_link_stream_grab()": ("never", False),
    # tests and meters
    "chaintest.c": ("restructured", False),
    "bench.c": ("restructured", False),
    "dactest.c": ("proven", False),
    "meter.c": ("restructured", False),
    # application
    "main.c": ("restructured", True),
    "acquisition.c": ("restructured", True),
    "routing.c": ("never", True),
    "routing_apply()": ("never", True),
    "board.h · board_cfg": ("restructured", True),
    "capture.c": ("restructured", True),
    "capture_service()": ("restructured", True),
    "dma0_event()": ("restructured", True),
    "pingpong.c": ("restructured", True),
    "port_impl.c": ("never", True),
    # libraries (frame.c is new, so the shared box is "never")
    "frame · crc16 · fmt · stats · tri_eval": ("never", True),
    "diag.c": ("restructured", True),
    # port layer - new in N+1
    "log.h · port_log/trace": ("never", True),
    "panic.h · port_panic()": ("never", True),
    "wait.h · PORT_WAIT_WHILE()": ("never", True),
    "regs.h · reg_visit_t": ("never", True),
    # drivers
    "clock.c": ("restructured", True),
    "adc.c": ("restructured", True),
    "dma.c": ("restructured", True),
    "_DMA0Interrupt": ("proven", True),
    "sccp.c": ("restructured", True),
    "dac.c": ("restructured", True),
    "uart.c": ("restructured", True),
    "uart.c · UART2": ("restructured", True),
    "timebase.c": ("proven", True),
    "led.c": ("proven", True),
}


def marks(x, y, w, title):
    st = STATUS.get(title)
    if not st:
        return ""
    status, offboard = st
    cx, cy = x + w - 11, y + 11
    s = f'<circle class="st-{status}" cx="{cx}" cy="{cy}" r="5"><title>{status}</title></circle>'
    if not offboard:
        s += f'<circle class="nooff" cx="{cx-15}" cy="{cy}" r="4.5"><title>no off-board test</title></circle>'
    return s


def legend(x, y):
    items = [("st-proven", "ran on silicon, unchanged since"),
             ("st-restructured", "ran on silicon, changed in N+1 - board run pending"),
             ("st-never", "never ran on silicon"),
             ("nooff", "no test without a board")]
    s = [f'<text class="s" x="{x}" y="{y+4}">Test status, {E(STATUS_AS_OF)} (docs/TEST-COVERAGE.md):</text>']
    y += 20
    cx = x + 6
    for cls, label in items:
        r = "4.5" if cls == "nooff" else "5"
        s.append(f'<circle class="{cls}" cx="{cx}" cy="{y}" r="{r}"/>')
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

    d.append(band(112, 86, "cli", "Console", ["src/cli/", "src/link/"]))
    d.append(box(170, 124, 400, 62, "cli.c · console.h", ["27 commands + help in 32 parser slots", "stream on|off|grab · route list · status · clk · pll"], "cli"))
    d.append(box(582, 124, 250, 62, "cmd_parser.c", ["upstream zabooh/cmd_parser", "do not edit (MAX_COMMANDS only)"], "cli"))
    d.append(box(844, 124, 296, 62, "gui_link.c", ["snap · rate · blk", "stream grab: halt → send → resume"], "cli"))

    d.append(band(214, 86, "test", "Tests & meters", ["src/tests/", "src/meter/"]))
    for i, (t, l) in enumerate([
            ("chaintest.c", ["chain all S0..S9 · chain run", "@ log for eval_chain.py"]),
            ("bench.c", ["sweep · test self/clock/clkoff/", "bursts/rate/matrix/dac"]),
            ("dactest.c", ["judges captured halves", "against the DAC triangle"]),
            ("meter.c", ["selftest · oneshot", "measure_rate (back-to-back)"])]):
        d.append(box(170 + i * 245, 226, 233, 62, t, l, "test"))

    d.append(band(316, 120, "app", "Application", ["src/app/", "src/boards/"]))
    d.append(box(170, 328, 150, 48, "main.c", ["start-up order, loop"], "app"))
    d.append(box(332, 328, 330, 48, "acquisition.c", ["rate (PLL1), variant matrix, stream on/off, chain setup"], "app"))
    d.append(box(674, 328, 250, 48, "routing.c", ["route_t, resources, routing_apply()"], "app"))
    d.append(box(936, 328, 204, 48, "board.h · board_cfg", ["EV74H48A | EV17P63A"], "app"))
    d.append(box(170, 384, 440, 44, "capture.c", ["DMA buffer + guard words · dma0_event() · counters · chain halt/resume"], "app"))
    d.append(box(622, 384, 240, 44, "pingpong.c", ["completed half, missed, block count"], "app"))
    d.append(box(874, 384, 266, 44, "port_impl.c", ["port_* → console / fail(), clock_fail_hook()"], "app"))

    d.append(band(452, 86, "lib", "Libraries", ["src/lib/", "src/diag/"]))
    d.append(box(170, 464, 380, 62, "frame · crc16 · fmt · stats · tri_eval", ["hardware-free, tested on the host", "(tests/host, Python cross-checks)"], "lib"))
    d.append(box(562, 464, 300, 62, "iir1 · goertzel_f/i · detect · wavegen", ["linked, not called yet", "(for N+3 / N+4)"], "lib", True))
    d.append(box(874, 464, 266, 62, "diag.c", ["fail() codes, trap, boot record,", "reg_print(), stack high-water mark"], "diag"))

    d.append(band(554, 50, "port", "Port layer", ["src/port/ · headers"]))
    for i, t in enumerate(["log.h · port_log/trace", "panic.h · port_panic()",
                           "wait.h · PORT_WAIT_WHILE()", "regs.h · reg_visit_t"]):
        d.append(box(170 + i * 245, 562, 233, 34, t, (), "port"))

    d.append(band(620, 86, "drv", "Drivers", ["src/drivers/", "src/sim/"]))
    drv = [("clock.c", ["PLL1, PLL2", "CLKGEN6/7/13"]), ("adc.c", ["core 5, burst", "trigger, calib."]),
           ("dma.c", ["channel 0 + ISR", "sim: sim_dma.c"]), ("sccp.c", ["SCCP1 as", "trigger source"]),
           ("dac.c", ["DAC1/2 triangle", "UREF route"]), ("uart.c", ["UART2, PPS", "RX ISR"]),
           ("timebase.c", ["Timer1", "stopwatch"]), ("led.c", ["LED0"])]
    hw = [("PLL1 / PLL2", ["PLL1 → ADC path", "PLL2 → CPU"]), ("ADC", ["core 5, AD5AN3", "(pin RA8)"]),
          ("DMA0", ["repeated", "one-shot → RAM"]), ("SCCP1", ["on CLKGEN13", "160 MHz"]),
          ("DAC2", ["on CLKGEN7", "400 MHz → RA8"]), ("UART2", ["console", "(COM port)"]),
          ("Timer1", ["12.5 MHz", "on PLL2"]), ("LED0", ["heartbeat"])]
    d.append(band(722, 86, "hw", "Silicon", ["dsPIC33AK512", "MPS512"]))
    for i, ((t, l), (ht, hl)) in enumerate(zip(drv, hw)):
        x = 170 + i * 122
        d.append(box(x, 632, 110, 62, t, l, "drv"))
        d.append(f'<path class="ln1" d="M{x+55} 694 V734"/>')
        d.append(box(x, 734, 110, 62, ht, hl, "hw"))
    d.append(legend(20, 834))
    return svg(1160, 872, "Firmware layers and modules", "\n".join(d))


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
    g.append(box(780, 80, 150, 84, "DMA0", ["repeated one-shot", "TRMODE = 1", "1 transfer / trigger"], "hw"))
    g.append('<text class="an" x="980" y="52">ping-pong buffer in RAM</text>')
    g.append('<rect class="bx s-hw" x="980" y="60" width="170" height="52" rx="4"/><text class="t" x="990" y="91">half A</text>')
    g.append('<rect class="bx s-hw" x="980" y="112" width="170" height="52" rx="4"/><text class="t" x="990" y="143">half B</text>')
    g.append('<rect class="bx s-diag" x="980" y="164" width="170" height="20" rx="3"/><text class="s" x="990" y="178">guard words</text>')
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
    g.append(box(780, 290, 150, 56, "_DMA0Interrupt", ["dma.c · 42 instr."], "drv"))
    g.append(box(980, 290, 170, 56, "dma0_event()", ["capture.c", "→ pingpong_on_half()"], "app"))
    g.append(box(200, 390, 200, 56, "adc_gui.py (host)", ["time signal, FFT, tri_eval"], "host", True))
    g.append(box(450, 390, 200, 56, "uart.c · UART2", ["GRAB header, data, CRC"], "drv"))
    g.append(box(700, 390, 250, 56, "gui_link_stream_grab()", ["halt → completed half → frame_send()", "→ resume"], "cli"))
    g.append(box(980, 390, 170, 56, "capture_service()", ["main loop:", "process_buffer()"], "app"))
    for p in ["M400 318 H450", "M855 164 V290", "M930 318 H980", "M1065 346 V390", "M980 418 H950",
              "M700 418 H650", "M450 418 H400", "M300 390 V346"]:
        g.append(f'<path class="ar" d="{p}"/>')
    g.append('<path class="ar dash" d="M640 290 V164"/>')
    g.append('<path class="ar dash" d="M1150 150 H1168 V418 H1150"/>')
    g.append('<text class="an" x="648" y="244">sets up</text>')
    g.append('<text class="an" x="862" y="244">HALF / DONE</text>')
    g.append('<text class="an" x="1072" y="372">ready_half</text>')
    g.append('<text class="an" x="308" y="372">command</text>')
    g.append(legend(20, 490))
    return svg(1190, 528, "Data path while streaming", "\n".join(g))


if __name__ == "__main__":
    for name, text in (("architecture_layers.svg", layers()), ("architecture_datapath.svg", datapath())):
        path = os.path.join(HERE, name)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("wrote", path)
