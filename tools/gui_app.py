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

"""gui_app.py - the application's part of adc_gui.py: the impact counter.

adc_gui.py is the example's generic front end; it imports this module if
it exists (`import gui_app`, optional) and calls the hooks below at fixed
points - the signal processing card, the settings, the setups list, the
stand-in (FakeTarget) and the self-test. Without this file the GUI is the
plain example. Moved out of adc_gui.py on 02.10.2026 together with the
firmware's counter (src/core/sigproc.c -> src/app/impact.c), unchanged in
what it shows and sends.

The impact counter (CNT, 02.10.2026, docs/IMPLEMENTATION-PLAN.md): a damped
resonator at the plate's ring frequency on every sample of the input, and a
detector that counts each ring once. Console: 'sigproc cnt on|off|reset',
'sigproc cnt f|tau|thr <n>'; GRAB header: cnt= (count since the reset),
cnr= (rate per second), cpk= (largest magnitude since the previous grab) -
protocol.py hands them over as meta['ext'].
"""
import math

# merged into adc_gui.py's "sigproc" settings: on, ring frequency Hz, tau us,
# threshold LSB
SIGPROC_DEFAULTS = {"cnt": False, "cnt_f": 50000, "cnt_tau": 100, "cnt_thr": 150}
# what a setup that does not name the counter gets: off
SIGPROC_OFF = {"cnt": False}


def counter_meta(meta):
    """The counter's fields of one GRAB frame as a dict count/rate/peak, or
    None when the frame carries none (counter off, or an older firmware)."""
    ext = meta.get("ext") or {}
    if "cnt" not in ext:
        return None
    return dict(count=ext["cnt"], rate=ext.get("cnr", 0), peak=ext.get("cpk", 0))


# ---- setups (adc_gui.py's SETUPS) -------------------------------------------

def setups(sg_setup):
    """The counter's check setups. sg_setup is adc_gui.py's _sg_setup()."""
    def cnt_setup(f_ring, rate, f_cnt=None, thr=150):
        """An impact-counter check (CNT, 02.10.2026): the generator plays ONE
        damped ring per table - f_ring, decay 10000/s (100 us), the table
        1e6 / rate entries at 1 MHz, so `rate` rings a second, exactly - on
        DAC2, read on RA8 at 1 MSPS, the counter listening at f_cnt (default
        f_ring) with tau 100 us. The count must go up by `rate` a second
        (board: exact at 500, 1000, 2000/s; HARDWARE-LOG 02.10.2026)."""
        cfg = sg_setup({}, f0=float(f_ring), n=int(round(1e6 / rate)), play=1000000,
                       decay=10000.0, ksps=1000, trig=True)
        cfg["sigproc"] = {"filter": "off", "gz": False, "thr": 100, "cnt": True,
                          "cnt_f": int(f_cnt or f_ring), "cnt_tau": 100, "cnt_thr": thr}
        return cfg

    return {
        "cnt_1000": ("impact counter - rings at 50 kHz, 1000/s: the count rises by 1000 a second",
                     cnt_setup(50000, 1000)),
        "cnt_500": ("impact counter - rings at 50 kHz, 500/s", cnt_setup(50000, 500)),
        "cnt_wrong_f": ("impact counter - rings at 80 kHz, counter at 50 kHz, threshold 300: expect 0 "
                        "(at 150 it counts them - a short ring is broadband)",
                        cnt_setup(80000, 1000, f_cnt=50000, thr=300)),
        "cnt_dense": ("impact counter - 5000/s, rings 200 us apart, each on the last one's tail: "
                      "still counted", cnt_setup(50000, 5000)),
        "cnt_limit": ("impact counter - 10000/s, 100 us apart at a 100-us decay: the limit - set "
                      "tau to 25 us and it counts again", cnt_setup(50000, 10000)),
    }


# ---- the stand-in (adc_gui.py's FakeTarget) ---------------------------------

class FakeCounter:
    """impact.c's counter for the stand-in: the same first difference,
    damped resonator, magnitude every 4th sample against the squared
    thresholds, the same relative re-arm, and the same exact scale. The
    stand-in only has the grabbed windows, not a continuous stream, so it
    counts what they show and restarts the resonator at each one (a gap, as
    on the board after missed halves)."""

    def __init__(self):
        self.on, self.f, self.tau, self.thr = False, 50000, 100, 100
        self.reset()

    def reset(self):
        self.count, self.seen, self.peak = 0, 0, 0.0
        self.armed, self.pk2, self.tr2 = True, 0.0, 0.0

    def block(self, v, fs):
        w = 2 * math.pi * self.f / fs
        d = math.exp(-1e6 / (self.tau * fs))
        c, dd, cw, sw = 2 * d * math.cos(w), d * d, math.cos(w), math.sin(w)
        dre = 1 - c * cw + dd * math.cos(2 * w)
        dim = c * sw - dd * math.sin(2 * w)
        scale = 2 * math.sin(w / 2) * sw / math.hypot(dre, dim)
        thr2, rearm2 = (self.thr * scale) ** 2, (self.thr * scale / 2) ** 2
        q1 = q2 = 0.0
        x = [float(u) for u in v]
        xp = x[0]
        for i, u in enumerate(x):
            q0 = (u - xp) + c * q1 - dd * q2
            xp, q2, q1 = u, q1, q0
            if i % 4 == 3:
                m2 = (q1 - q2 * cw) ** 2 + (q2 * sw) ** 2
                self.peak = max(self.peak, math.sqrt(m2) / scale)
                # impact.c's rule: count above thr after a rise by 1/0.7
                # from the trough, re-arm at 70 % of the peak (or thr / 2)
                if self.armed:
                    self.tr2 = min(self.tr2, m2)
                    if m2 > thr2 and m2 * 0.49 > self.tr2:
                        self.count += 1
                        self.armed, self.pk2 = False, m2
                else:
                    self.pk2 = max(self.pk2, m2)
                    if m2 < 0.49 * self.pk2 or m2 < rearm2:
                        self.armed, self.tr2 = True, m2
        self.seen += len(x)

    def fields(self, fs):
        secs = self.seen / fs if fs else 0.0
        rate = int(round(self.count / secs)) if secs else 0
        peak = int(round(self.peak))
        self.peak = 0.0
        return self.count, rate, peak, int(round(secs * 1000))


class FakeApp:
    """The counter's part of FakeTarget: what impact.c's sigproc_app_cmd(),
    sigproc_app_status() and gui_link_app_fields() answer on the board."""

    def __init__(self):
        self.k = FakeCounter()

    @property
    def active(self):
        return self.k.on

    def load_cycles(self):
        """CPU cycles per sample it costs on the board (about 25)."""
        return 25.0 if self.k.on else 0.0

    def command(self, args, fs):
        """'sigproc <args>' sub-commands cli.c does not know: None = not the
        counter's, else (ok, lines) - lines only on a refusal."""
        if not args or args[0] != "cnt":
            return None
        if len(args) == 2 and args[1] in ("on", "off", "reset"):
            if args[1] == "reset" or (args[1] == "on" and not self.k.on):
                self.k.reset()
            if args[1] != "reset":
                self.k.on = args[1] == "on"
            return True, []
        if len(args) == 3 and args[1] in ("f", "tau", "thr") and args[2].isdigit():
            v = int(args[2])
            ok_v = {"f": 1000 <= v and v * 2.5 <= fs, "tau": 2 <= v <= 5000,
                    "thr": 1 <= v <= 4095}[args[1]]
            if not ok_v:
                return False, ["usage: cnt f 1000..fs/2.5 Hz, tau 2..5000 us, thr 1..4095 LSB"]
            setattr(self.k, args[1], v)
            return True, []
        return False, ["usage: sigproc cnt on|off|reset | cnt f <hz> | cnt tau <us> | cnt thr <lsb>"]

    def status_lines(self, fs):
        k = self.k
        lines = [f"cnt: {'on' if k.on else 'off'}", f"cnt_f_hz: {k.f}", f"cnt_tau_us: {k.tau}",
                 f"cnt_thr: {k.thr}"]
        if k.on:
            secs = k.seen / fs if fs else 0.0
            lines += [f"cnt_count: {k.count}",
                      f"cnt_rate: {int(round(k.count / secs)) if secs else 0}",
                      f"cnt_ms: {int(round(secs * 1000))}", "cnt_missed: 0",
                      f"cnt_peak: {int(round(k.peak))}", f"cnt_fs_hz: {int(fs)}"]
        return lines

    def header_fields(self, v, fs):
        """The GRAB header's fields for this window (on the input, before
        the filter, as on the board)."""
        if not (self.k.on and fs):
            return ""
        self.k.block(v, fs)
        cc, cr, cp, _ = self.k.fields(fs)
        return f" cnt={cc} cnr={cr} cpk={cp}"


# ---- the self-test (adc_gui.py --selftest) ----------------------------------

def selftest(t, setups_, siggen_commands):
    """The counter's commands, header fields and counting setups through the
    stand-in - a ring per table, counted in the grabs. Returns ok."""
    cnt_res = {}
    for key in ("cnt_1000", "cnt_wrong_f", "cnt_dense", "cnt_limit"):
        cfg = setups_[key][1]
        sg, acq, sp = cfg["siggen"], cfg["acquisition"], cfg["sigproc"]
        t.cmd("stream off")
        for line in siggen_commands(dict(sg, play=sg["play_hz"], h={k: sg["h"][k - 2] for k in range(2, 8)})):
            t.cmd(line)
        t.cmd(f"stream on {acq['ksps']} {acq['core']} {acq['pinsel']}")
        okc = all(t.cmd(x)[0] for x in (f"sigproc cnt f {sp['cnt_f']}", f"sigproc cnt tau {sp['cnt_tau']}",
                                         f"sigproc cnt thr {sp['cnt_thr']}", "sigproc cnt on"))
        for _ in range(3):
            okg, s_c, meta_c = t.grab()
        cm = counter_meta(meta_c) or {}
        cnt_res[key] = (okc, cm.get("count"), cm.get("peak"))
        t.cmd("sigproc cnt off")
        t.cmd("siggen off")
    t.cmd("stream on 5000 3 5 0")
    ok_bad_f, _ = t.cmd("sigproc cnt f 3000000")
    # 3 grabs of 2048 samples at 1 MSPS hold 6 rings at 1000/s and 30 at
    # 5000/s (counted since the relative re-arm); 10000/s at tau 100 is the
    # limit - the rings merge, at most one counted per grab
    # (the stand-in sees only the grabs and restarts at each one: a grab
    # that begins inside a ring counts its tail too - up to one per grab)
    ok_cnt = (cnt_res["cnt_1000"][0] and 5 <= (cnt_res["cnt_1000"][1] or 0) <= 9
              and cnt_res["cnt_wrong_f"][1] == 0 and 27 <= (cnt_res["cnt_dense"][1] or 0) <= 33
              and (cnt_res["cnt_limit"][1] or 0) <= 3 and not ok_bad_f)
    print(f"impact counter setups on the stand-in (ok, count, peak): {cnt_res}, f beyond fs/2.5 refused ->",
          "PASS" if ok_cnt else "FAIL")
    return ok_cnt


# ---- the page (adc_gui.py's signal processing card) -------------------------

class Card:
    """The counter's rows in the signal processing card. Built inside the
    card (ui is nicegui's ui); adc_gui.py wires the rest: on_change() for
    every value that is sent, the reset button, settings, chart marks."""

    def __init__(self, ui):
        # CNT: the impact counter - a resonator at the plate's ring
        # frequency and a detector that counts each ring once
        ui.label("impact counter").classes("text-xs text-slate-400")
        with ui.row().classes("w-full gap-2 items-center"):
            self.cb = ui.checkbox("count", value=False)
            self.f_in = ui.number("ring f, kHz", value=50, min=1, max=400, step=1,
                                  format="%g").props("dense outlined").classes("flex-grow")
        with ui.row().classes("w-full gap-2 items-center"):
            self.tau_in = ui.number("tau, us", value=100, min=2, max=5000, step=10,
                                    format="%d").props("dense outlined").classes("flex-grow")
            self.thr_in = ui.number("threshold, LSB", value=150, min=1, max=4095, step=10,
                                    format="%d").props("dense outlined").classes("flex-grow")
            self.reset_btn = ui.button("reset", icon="restart_alt").props("outline dense")
        with ui.row().classes("w-full gap-1 items-center"):
            self.cnt_chip = ui.chip("count –", color="grey-8").props("dense outline")
            self.cnr_chip = ui.chip("rate –", color="grey-8").props("dense outline")
            self.cpk_chip = ui.chip("peak –", color="grey-8").props("dense outline")
        self.prev = None

    def tips(self):
        return [
            (self.cb, "The impact counter in the firmware ('sigproc cnt on|off'): a damped resonator "
                      "at the plate's ring frequency, on every sample of the input, and a detector "
                      "that counts each ring once - above the threshold, again only after it has "
                      "fallen below half of it. Counts continuously, also between grabs."),
            (self.f_in, "The ring frequency the resonator listens at, kHz ('sigproc cnt f <hz>'); "
                        "at most fs/2.5."),
            (self.tau_in, "The resonator's time constant, us ('sigproc cnt tau <us>'): best the ring's "
                          "own decay time (a matched filter). Longer: more selective, slower to fall, "
                          "so close impacts merge sooner."),
            (self.thr_in, "Count above this magnitude, LSB of the ring's amplitude ('sigproc cnt thr'). "
                          "The 'peak' chip shows what the rings reach - set it well below that, and "
                          "above what other noises reach. A ring at another frequency still reaches "
                          "a part (a short ring is broadband: 39 % at 80 kHz for 50)."),
            (self.reset_btn, "Start count and time over ('sigproc cnt reset')."),
            (self.cnt_chip, "Impacts counted since the reset."),
            (self.cnr_chip, "Impacts per second, over the time since the reset."),
            (self.cpk_chip, "The largest resonator magnitude since the previous grab, LSB - what a ring "
                            "reaches, to set the threshold against."),
        ]

    def value_elements(self):
        """The elements whose change is sent to the board."""
        return (self.cb, self.f_in, self.tau_in, self.thr_in)

    def _vals(self):
        return (bool(self.cb.value), int(round(float(self.f_in.value or 50) * 1000)),
                int(self.tau_in.value or 100), int(self.thr_in.value or 150))

    def settings(self):
        on, f, tau, thr = self._vals()
        return {"cnt": on, "cnt_f": f, "cnt_tau": tau, "cnt_thr": thr}

    def load(self, spc):
        self.f_in.value = int(spc.get("cnt_f", 50000)) / 1000.0
        self.tau_in.value = int(spc.get("cnt_tau", 100))
        self.thr_in.value = int(spc.get("cnt_thr", 150))
        self.cb.value = bool(spc.get("cnt", False))

    def command_lines(self):
        on, f, tau, thr = self._vals()
        return (([f"sigproc cnt f {f}", f"sigproc cnt tau {tau}", f"sigproc cnt thr {thr}"]
                 if on else []) + [f"sigproc cnt {'on' if on else 'off'}"])

    def summary(self):
        on, f, tau, thr = self._vals()
        return "counter " + (f"at {f / 1e3:g} kHz, tau {tau} us, threshold {thr} LSB" if on else "off")

    RESET_LINE = "sigproc cnt reset"

    def after_reset(self):
        self.cnt_chip.text = "count 0"

    def fft_marks(self, meta):
        if counter_meta(meta) is None:
            return []
        return [{"xAxis": float(self.f_in.value or 0), "label": {"formatter": "ring f"}}]

    def update(self, meta):
        cntm = counter_meta(meta)
        if cntm is None:
            self.prev = None
            self.cnt_chip.text, self.cnr_chip.text, self.cpk_chip.text = "count –", "rate –", "peak –"
            self.cnt_chip.props("color=grey-8")
            return
        self.cnt_chip.text = f"count {cntm['count']}"
        self.cnr_chip.text = f"{cntm['rate']} /s"
        self.cpk_chip.text = f"peak {cntm['peak']} LSB"
        # no new count since the last grab although the resonator rang
        # above the threshold: the rings merge (or one long tone) - the
        # detector never re-arms (board: 7000/s at tau 100 counts 0, at
        # tau 25 exactly; HARDWARE-LOG 02.10.2026)
        stuck = (self.prev is not None and cntm["count"] == self.prev
                 and cntm["peak"] > int(self.thr_in.value or 150))
        self.prev = cntm["count"]
        if stuck:
            self.cnr_chip.text = f"{cntm['rate']} /s - rings merge or one long tone: lower tau"
            self.cnr_chip.props("color=warning")
        else:
            self.cnr_chip.props("color=grey-8")
        self.cnt_chip.props("color=positive" if cntm["count"] else "color=grey-8")
