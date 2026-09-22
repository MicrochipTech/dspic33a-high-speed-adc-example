#!/usr/bin/env python3
"""
sim_trap.py - run the simulator build in the MPLAB X simulator (MDB, the
command-line debugger, driven over stdin/stdout) and report what the
firmware did.

  1. Program build\\adc_dma_40msps_sim.elf, route UART2 output to a file,
     run for --run-seconds. The simulator has no PLL/ADC/DMA; the clock
     waits are no-ops there and sim_dma.c stands in for the DMA (1 MHz
     sine, flat 3840 on the self-test input). Expected: boot_stage 9,
     [selftest] passed, [simtest] PASS from the ping-pong check, then
     [stat] lines. Anything with [TRAP] or a fail code is a software
     fault reproducible off silicon.
  2. Halt, print boot_stage / fail_code / trap_* / the check counters
     (needs -g, which build.bat sim sets).

Options:
  --fault VALUE   once the firmware reports the measurement running, plus
                  --fault-seconds, write VALUE into sim_fault_once
                  (address from xc-dsc-nm). 65536 drops one sine sample:
                  the ping-pong check must then report a mismatch at
                  index 0 and end in [simtest] FAIL - the negative test.
                  DMA0STAT masks (e.g. 8 = OVERRUN) exercise the counters.
  --inject        phase 3 (off by default): set IEC6.9/IFS6.9 (AD3CH0,
                  vector 201) by writing the SFRs, to reach the trap
                  handler. Known result with MPLAB X v6.35: the simulator
                  does not dispatch interrupts in this project and aborts
                  with E0110 "Failed to execute instruction" - the
                  handler never runs. Kept for re-checking newer versions.

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
           "blocks_done", "selftest_mean", "proc_missed", "dma_overrun",
           "sim_check_halves", "sim_check_bad", "sim_check_done"]   # ping-pong check, see sim_dma.c
NOISE = re.compile(r"W0107|INFO:|^\w{3} \d+, \d{4}|org\.|WARNING: (NetBeans|Unable)|"
                   r"^>?\s*$|Resetting|file:|address:|source line")


def find_mdb():
    hits = glob.glob(r"C:\Program Files\Microchip\MPLABX\v*\mplab_platform\bin\mdb.bat")
    return sorted(hits)[-1] if hits else None


def find_nm():
    hits = glob.glob(r"C:\Program Files\Microchip\xc-dsc\v*\bin\xc-dsc-nm.exe")
    return sorted(hits)[-1] if hits else None


def symbol_address(elf, name):
    """Data address of a global from the ELF symbol table."""
    out = subprocess.run([find_nm(), elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "_" + name:
            return int(parts[0], 16)
    sys.exit(f"symbol {name} not found in {elf} (built with -g?)")


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
        """MDB answers "Print x" with the name on one line and the value on
        the next, so keep a matching line together with the line after it."""
        start = len(self.lines)
        for n in names:
            self.cmd(f"Print {n}", 0.4)
        time.sleep(1.0)
        out = []
        tail = [l.lstrip("> ") for l in self.lines[start:]]
        for i, l in enumerate(tail):
            if any(l.startswith(n) for n in names):
                if l.endswith("=") and i + 1 < len(tail):
                    l = l + tail[i + 1].strip()
                out.append(l)
        return out

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
    ap.add_argument("--run-seconds", type=float, default=240.0,
                    help="100 halves take about 150 s after the boot, more with mismatch reports on the UART")
    ap.add_argument("--fault", type=lambda x: int(x, 0), default=0,
                    help="value to write into sim_fault_once during the run (65536 = drop a sample)")
    ap.add_argument("--fault-seconds", type=float, default=10.0,
                    help="seconds after 'measurement running' before the fault is written")
    ap.add_argument("--inject", action="store_true", help="phase 3: raise AD3CH0 (see docstring)")
    ap.add_argument("--inject-seconds", type=float, default=20.0)
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    elf = os.path.abspath(a.elf)
    out = os.path.splitext(elf)[0] + ".uart2.txt"
    if os.path.exists(out):
        os.remove(out)
    bat = find_mdb()
    if not bat or not os.path.exists(elf):
        sys.exit(f"missing: mdb={bat} elf={elf}")
    fault_addr = symbol_address(elf, "sim_fault_once") if a.fault else 0

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

    print(f"--- phase 1: run {a.run_seconds:.0f} s" + (f", fault 0x{a.fault:X} after {a.fault_seconds:.0f} s" if a.fault else ""))
    m.cmd("Run", 1)
    t0 = time.time()
    if a.fault:
        # The fault must land on the sine, not on the self-test's flat
        # halves (a dropped sample there shifts nothing the check sees).
        # The boot banner alone costs the simulator about a minute of UART
        # time, so wait for the firmware to say the measurement runs, then
        # a few halves more, before writing. Written while running:
        # sim_fault_once is plain RAM, not SFR space (an SFR write during
        # Run is what makes the simulator abort).
        while "measurement running" not in uart() and time.time() - t0 < a.run_seconds:
            time.sleep(2.0)
        time.sleep(a.fault_seconds)
        m.cmd(f"write {fault_addr} {a.fault}", 0.5)
        print(f"    wrote {a.fault} to sim_fault_once @0x{fault_addr:X} at {time.time() - t0:.0f} s")
    time.sleep(max(0.0, a.run_seconds - (time.time() - t0)))
    m.cmd("Halt", 2)
    print("\n".join(m.print_symbols(SYMBOLS)))
    txt = uart()
    seen = len(txt)
    print("--- UART2 so far ---")
    print(txt or "(empty)")
    if "[simtest] PASS" in txt:
        verdict = "PASS"
    elif "[simtest] FAIL" in txt:
        verdict = "FAIL"
    elif "[simtest] mismatch" in txt:
        verdict = "FAIL (mismatch reported, final verdict still pending - run longer)"
    else:
        verdict = "no verdict yet - run longer"
    print(f"--- ping-pong check: {verdict}")

    if a.inject:
        print(f"--- phase 3: inject IEC6/IFS6 mask 0x{AD3CH0:X}, run {a.inject_seconds:.0f} s")
        m.cmd(f"write {IEC6} {AD3CH0}", 0.5)
        m.cmd(f"write {IFS6} {AD3CH0}", 0.5)
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
