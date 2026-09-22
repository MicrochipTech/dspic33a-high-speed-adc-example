#!/usr/bin/env python3
"""
sim_trap.py - run the simulator build in the MPLAB X simulator and find
out whether the start-up path traps, then fire the suspect interrupt.

What it does (MDB, the command-line debugger of MPLAB X, driven over
stdin/stdout):

  1. Program build\\adc_dma_40msps_sim.elf, route UART2 output to a file,
     run for --run-seconds. The simulator has no PLL/ADC/DMA; the clock
     waits are no-ops there and sim_dma_tick() in adc_dma_40msps.c stands
     in for the ADC/DMA (1 MHz sine, flat 3840 on the self-test input),
     so the expected state is boot_stage 9 with the self-test passed and
     [stat] lines following. Anything with [TRAP] or a fail code is a
     software fault reproducible off silicon.
  2. Halt, print boot_stage / fail_code / trap_* (needs -g).
  3. Resume, then set IEC6.9 and IFS6.9 (AD3CH0, vector 201) by writing
     the SFRs. That is exactly the event commit a60066a suspects on the
     board; here it must land in _DefaultInterrupt() and produce a [TRAP]
     report with VECNUM = 201 - which validates the reporting path.

Usage:  python sim_trap.py [--elf ..\\build\\adc_dma_40msps_sim.elf] [-v]
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import threading
import time

DEVICE = "dsPIC33AK512MPS512"
IFS6 = 0xA8            # p33AK512MPS512.gld
IEC6 = 0xD8
AD3CH0 = 1 << 9        # _IFS6_AD3CH0IF_MASK
SYMBOLS = ["W15", "SPLIM", "INTCON1", "INTCON2", "boot_stage", "fail_code", "trap_seen", "trap_vec", "trap_stage",
           "blocks_done", "selftest_mean", "proc_missed",
           "sim_check_halves", "sim_check_bad", "sim_check_done"]   # ping-pong check, see adc_dma_40msps.c
NOISE = re.compile(r"W0107|INFO:|^\w{3} \d+, \d{4}|org\.|WARNING: (NetBeans|Unable)|"
                   r"^>?\s*$|Resetting|file:|address:|source line")


def find_mdb():
    hits = glob.glob(r"C:\Program Files\Microchip\MPLABX\v*\mplab_platform\bin\mdb.bat")
    return sorted(hits)[-1] if hits else None


class Mdb:
    def __init__(self, bat, verbose):
        self.verbose = verbose
        self.proc = subprocess.Popen(["cmd", "/c", bat], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
        self.lines = []
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in self.proc.stdout:
            if NOISE.search(line):
                continue
            self.lines.append(line.rstrip())
            if self.verbose:
                print("MDB>", line.rstrip(), flush=True)

    def cmd(self, text, delay=0.5):
        if self.verbose:
            print(">>>", text, flush=True)
        self.proc.stdin.write(text + "\n")
        self.proc.stdin.flush()
        time.sleep(delay)

    def wait_for(self, pattern, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if any(re.search(pattern, l) for l in self.lines):
                return True
            time.sleep(0.2)
        return False

    def print_symbols(self, names):
        start = len(self.lines)
        for n in names:
            self.cmd(f"Print {n}", 0.4)
        time.sleep(1.0)
        return [l for l in self.lines[start:] if any(l.startswith(n) for n in names)]

    def quit(self):
        try:
            self.cmd("Quit", 1.0)
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=os.path.join(os.path.dirname(__file__), "..", "build",
                                                   "adc_dma_40msps_sim.elf"))
    ap.add_argument("--run-seconds", type=float, default=60.0)
    ap.add_argument("--inject-seconds", type=float, default=20.0)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--iec", type=lambda x: int(x, 0), default=IEC6)
    ap.add_argument("--ifs", type=lambda x: int(x, 0), default=IFS6)
    ap.add_argument("--mask", type=lambda x: int(x, 0), default=AD3CH0)
    a = ap.parse_args()
    elf = os.path.abspath(a.elf)
    out = os.path.splitext(elf)[0] + ".uart2.txt"
    if os.path.exists(out):
        os.remove(out)
    bat = find_mdb()
    if not bat or not os.path.exists(elf):
        sys.exit(f"missing: mdb={bat} elf={elf}")

    def uart():
        try:
            return open(out, "rb").read().decode("ascii", "replace")
        except FileNotFoundError:
            return ""

    m = Mdb(bat, a.verbose)
    m.cmd(f"Device {DEVICE}", 3)
    m.cmd("Set uart2io.uartioenabled true")
    m.cmd("Set uart2io.output file")
    m.cmd(f"Set uart2io.outputfile {out}")
    m.cmd("Set oscillator.frequency 8")
    m.cmd("Set oscillator.frequencyunit Mega")
    m.cmd("Hwtool SIM", 2)
    if not m.wait_for(r">|Resetting", 60):
        sys.exit("simulator did not come up")
    m.cmd(f'Program "{elf}"', 2)
    if not m.wait_for(r"Program succeeded", 90):
        sys.exit("programming failed:\n" + "\n".join(m.lines[-10:]))

    print(f"--- phase 1: run {a.run_seconds:.0f} s")
    m.cmd("Run", 1)
    time.sleep(a.run_seconds)
    m.cmd("Halt", 2)
    print("\n".join(m.print_symbols(SYMBOLS)))
    txt = uart()
    seen = len(txt)
    print("--- UART2 so far ---")
    print(txt or "(empty)")

    print(f"--- phase 2: inject IEC 0x{a.iec:X} / IFS 0x{a.ifs:X} mask 0x{a.mask:X}, run {a.inject_seconds:.0f} s")
    # Written while halted: a write into SFR space during Run made the
    # simulator abort with E0110 ("Failed to execute instruction") inside
    # console_puts(), before any interrupt was dispatched.
    m.cmd(f"write {a.iec} {a.mask}", 0.5)
    m.cmd(f"write {a.ifs} {a.mask}", 0.5)
    m.cmd("Run", 1)
    time.sleep(a.inject_seconds)
    m.cmd("Halt", 2)
    print("\n".join(m.print_symbols(SYMBOLS)))
    print("--- UART2 new ---")
    print(uart()[seen:] or "(nothing new)")
    m.quit()
    print(f"\nfull log: {out}")


if __name__ == "__main__":
    main()
