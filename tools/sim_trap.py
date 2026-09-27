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

  --smoke         the smoke run [SMOKE] (docs/IMPLEMENTATION-PLAN.md P0.7):
                  program build\\adc_dma_40msps_smoke.elf (build.bat smoke),
                  run until the firmware prints "[smoke] DONE", halt, and
                  judge: FAIL on a [TRAP] block or a fail code in the UART
                  text, on trap_seen/fail_code != 0 after the halt, on the
                  simulator reporting an error, or on --smoke-timeout
                  without the marker. The UART text is saved as
                  build\\smoke.log and compared line by line with
                  tests\\smoke\\expected.log; the diff is printed. Lines
                  that legitimately differ between two builds - the three
                  that carry BUILD_ID (date, time, git revision) - are
                  masked on both sides before the comparison (mask_line()).
                  --update-expected rewrites expected.log from this run,
                  for a task that changes the console on purpose (a new
                  command in "help"): commit it with the change.
                  Exit code 0 = PASS, 1 = FAIL. Prints where the time went.

  --dump-sfr      with --smoke (P0.8, tests\\trace\\README.md "P0.8
                  cross-check"): Print the named SFRs once after
                  programming (the simulator's reset values) and once
                  after the halt at "[smoke] DONE", and write both to
                  --dump-out (build\\sfr_dump.txt) for
                  tools\\check_fake_sfr.py --sim-dump, which compares them
                  with the end state of a host golden trace.
                  "@tests\\trace\\golden\\boot.trace" names exactly the
                  registers that trace writes. MDB prints SFRs in decimal.

Usage:  python sim_trap.py [--elf ..\\build\\adc_dma_40msps_sim.elf] [-v]
        python sim_trap.py --smoke [--update-expected] [-v]
        python sim_trap.py --smoke --dump-sfr @tests\\trace\\golden\\boot.trace
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


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# The lines of the console output that differ between two builds of the
# same source: BUILD_ID (board.h) carries __DATE__, __TIME__ and the git
# revision, and it appears at boot ("[boot] ..."), in the console banner
# ("build: ...") and in "version"'s reply ("[build] ..."). Everything from
# the program name to the end of the line is replaced on both sides of the
# comparison; the prefix stays, so a line that moved or vanished still shows.
BUILD_ID_RE = re.compile(r"adc_dma_40msps \w{3} +\d+ \d{4} \d\d:\d\d:\d\d git .*$")
MASK = "adc_dma_40msps <build-id>"


def mask_line(line):
    return BUILD_ID_RE.sub(MASK, line)


def start_sim(elf, out, verbose):
    """MDB up, UART2 to `out`, the simulator selected and `elf` programmed.
    Returns the Mdb and the seconds it took (start-up, programming)."""
    if os.path.exists(out):
        os.remove(out)
    bat = find_mdb()
    if not bat or not os.path.exists(elf):
        sys.exit(f"missing: mdb={bat} elf={elf}")
    t0 = time.time()
    m = Mdb(bat, verbose)
    m.cmd(f"Device {DEVICE}", 3)
    m.cmd("Set uart2io.uartioenabled true")
    m.cmd("Set uart2io.output file")
    m.cmd(f"Set uart2io.outputfile {out}")
    m.cmd("Set oscillator.frequency 8")
    m.cmd("Set oscillator.frequencyunit Mega")
    m.cmd("Hwtool SIM", 2)
    if not m.wait_for(r">|Resetting", 60):
        sys.exit("simulator did not come up")
    t_up = time.time() - t0
    m.cmd(f'Program "{elf}"', 2)
    if not m.wait_for(r"Program succeeded", 90):
        sys.exit("programming failed:\n" + "\n".join(m.lines[-10:]))
    t_prog = time.time() - t0 - t_up
    return m, t_up, t_prog


def uart_reader(out):
    def uart():
        try:
            return open(out, "rb").read().decode("ascii", "replace")
        except FileNotFoundError:
            return ""
    return uart


SMOKE_SYMBOLS = ["boot_stage", "fail_code", "trap_seen", "trap_vec", "trap_stage", "INTCON1", "W15", "SPLIM"]
# INTCON1's trap flags (p33AK512MPS512.h): BADOPERR bit 2, ADDRERR bit 3,
# STKERR bit 4. Set by the CPU when the trap is raised, cleared by the
# handler - so after the halt they say "a trap was raised and nobody
# served it", which is what a trap in a simulator that dispatches no
# interrupt looks like.
INTCON1_TRAPS = 0x1C
# What the simulator itself prints when the firmware trips: an address
# error, a stack error, an illegal opcode. Seen as MDB output, not UART.
SIM_ERROR_RE = re.compile(r"E\d{4}|[Tt]rap|Address error|Stack error|Illegal|halted at", re.I)


def dump_sfr_names(spec):
    """--dump-sfr: 'A,B,C', or @FILE with one name per line (a .trace
    file: the registers its 'W NAME old -> new' lines write, in order)."""
    if not spec:
        return []
    if not spec.startswith("@"):
        return [s.strip() for s in spec.split(",") if s.strip()]
    names = []
    with open(spec[1:], encoding="ascii", errors="replace") as f:
        for ln in f:
            mm = re.match(r"^W\s+(\w+)\s", ln) if spec.endswith(".trace") else re.match(r"^\s*(\w+)", ln)
            if mm and mm.group(1) not in names and not ln.startswith("#"):
                names.append(mm.group(1))
    return names


def write_sfr_dump(path, names, before, after):
    """MDB's Print answers ('NAME=value', see Mdb.print_symbols) for the
    two moments, one line per register: 'NAME reset=<v> after=<v>'; a
    register MDB did not answer for gets '-'."""
    def table(lines):
        d = {}
        for l in lines:
            mm = re.match(r"^(\w+)\s*=\s*(\S+)", l)
            if mm:
                d[mm.group(1)] = mm.group(2)
        return d
    b, aft = table(before), table(after)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        for n in names:
            f.write(f"{n} reset={b.get(n, '-')} after={aft.get(n, '-')}\n")
    print(f"--- SFR dump ({len(names)} registers, before Run and after the halt): {path}")
    for n in names:
        print(f"    {n:<12} reset={b.get(n, '-'):<12} after={aft.get(n, '-')}")


def smoke(a):
    elf = os.path.abspath(a.elf or os.path.join(ROOT, "build", "adc_dma_40msps_smoke.elf"))
    out = os.path.splitext(elf)[0] + ".uart2.txt"
    log = os.path.abspath(a.log)
    expected = os.path.abspath(a.expected)
    uart = uart_reader(out)
    t_all = time.time()
    m, t_up, t_prog = start_sim(elf, out, a.verbose)
    dump_names = dump_sfr_names(a.dump_sfr)
    dump_reset = m.print_symbols(dump_names) if dump_names else []
    sim_lines_before = len(m.lines)

    print(f"--- smoke: run until '[smoke] DONE' (timeout {a.smoke_timeout:.0f} s)")
    m.cmd("Run", 1)
    t0 = time.time()
    why = None
    while time.time() - t0 < a.smoke_timeout:
        txt = uart()
        if "[smoke] DONE" in txt:
            break
        if "[TRAP]" in txt:
            why = "the firmware reported a [TRAP]"
            time.sleep(3.0)             # let the rest of the trap block out
            break
        if "[FAIL]" in txt or "fail code" in txt:
            why = "the firmware reported a fail code"
            time.sleep(3.0)
            break
        if any(SIM_ERROR_RE.search(l) for l in m.lines[sim_lines_before:]):
            why = "the simulator reported an error (see the MDB lines below)"
            time.sleep(1.0)
            break
        time.sleep(1.0)
    else:
        why = f"no '[smoke] DONE' within {a.smoke_timeout:.0f} s"
    t_run = time.time() - t0
    m.cmd("Halt", 2)
    # Only what MDB said between Run and Halt counts as a simulator
    # message: the Print answers that follow ("trap_seen=0") must not
    # trip the error pattern.
    mdb_run = [l for l in m.lines[sim_lines_before:] if l.strip()]
    sim_errors = [l for l in mdb_run if SIM_ERROR_RE.search(l)]
    syms = m.print_symbols(SMOKE_SYMBOLS)
    print("\n".join(syms))
    if mdb_run:
        print("--- MDB lines during the run ---")
        print("\n".join(mdb_run[-20:]))
    if dump_names:
        dump_after = m.print_symbols(dump_names)
        write_sfr_dump(a.dump_out, dump_names, dump_reset, dump_after)
    m.quit()

    txt = uart()
    os.makedirs(os.path.dirname(log), exist_ok=True)
    with open(log, "wb") as f:
        f.write(txt.encode("ascii", "replace"))

    problems = []
    if why:
        problems.append(why)
    if "[TRAP]" in txt and not why:
        problems.append("the firmware reported a [TRAP]")
    for s in syms:
        mm = re.match(r"(trap_seen|fail_code)=(\d+)", s)
        if mm and int(mm.group(2)) != 0:
            problems.append(f"{mm.group(1)} = {mm.group(2)} after the halt")
        mm = re.match(r"INTCON1=(\d+)", s)
        if mm and (int(mm.group(1)) & INTCON1_TRAPS):
            problems.append(f"INTCON1 = 0x{int(mm.group(1)):X} after the halt: a trap flag is set")
    if not syms:
        problems.append("MDB answered no Print (symbols missing - built with -g?)")
    if sim_errors and "simulator reported" not in (why or ""):
        problems.append("the simulator reported an error: " + sim_errors[0].strip())

    got = [mask_line(l) for l in txt.splitlines()]
    if a.update_expected:
        os.makedirs(os.path.dirname(expected), exist_ok=True)
        with open(expected, "w", newline="\n") as f:
            f.write("\n".join(txt.splitlines()) + "\n")
        print(f"--- expected.log rewritten from this run: {expected}")
    elif os.path.exists(expected):
        want = [mask_line(l) for l in open(expected, encoding="ascii", errors="replace").read().splitlines()]
        if got != want:
            import difflib
            diff = list(difflib.unified_diff(want, got, "expected.log", "smoke.log", lineterm="", n=1))
            print("--- diff against expected.log (build-id lines masked) ---")
            print("\n".join(diff[:80]))
            if len(diff) > 80:
                print(f"... ({len(diff) - 80} more diff lines)")
            problems.append(f"console output differs from {os.path.relpath(expected, ROOT)}")
        else:
            print(f"--- console output matches expected.log ({len(got)} lines, build-id lines masked)")
    else:
        problems.append(f"no {expected} - run once with --update-expected and commit it")

    t_total = time.time() - t_all
    print(f"--- time: mdb start {t_up:.0f} s, program {t_prog:.0f} s, run {t_run:.0f} s, total {t_total:.0f} s"
          f" ({len(txt)} UART characters)")
    print(f"--- log: {log}")
    if problems:
        print("--- [smoke] FAIL: " + "; ".join(problems))
        return 1
    print("--- [smoke] PASS")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=None,
                    help="default: build\\adc_dma_40msps_sim.elf (build\\adc_dma_40msps_smoke.elf with --smoke)")
    ap.add_argument("--smoke", action="store_true", help="the smoke run (see the docstring)")
    ap.add_argument("--smoke-timeout", type=float, default=180.0,
                    help="seconds to wait for '[smoke] DONE' before the run counts as failed")
    ap.add_argument("--expected", default=os.path.join(ROOT, "tests", "smoke", "expected.log"))
    ap.add_argument("--log", default=os.path.join(ROOT, "build", "smoke.log"))
    ap.add_argument("--update-expected", action="store_true",
                    help="--smoke: rewrite tests/smoke/expected.log from this run")
    ap.add_argument("--dump-sfr", default=None,
                    help="--smoke (P0.8): SFRs to Print after programming and again after the halt at "
                         "'[smoke] DONE' - a comma-separated list, or @FILE (one name per line; a .trace "
                         "file gives the registers its W lines write). Written to --dump-out as "
                         "'NAME reset=<v> after=<v>' for tools/check_fake_sfr.py --sim-dump")
    ap.add_argument("--dump-out", default=os.path.join(ROOT, "build", "sfr_dump.txt"))
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
    if a.smoke:
        sys.exit(smoke(a))
    elf = os.path.abspath(a.elf or os.path.join(ROOT, "build", "adc_dma_40msps_sim.elf"))
    out = os.path.splitext(elf)[0] + ".uart2.txt"
    if not os.path.exists(elf):
        sys.exit(f"missing: elf={elf}")
    fault_addr = symbol_address(elf, "sim_fault_once") if a.fault else 0
    uart = uart_reader(out)
    m, _, _ = start_sim(elf, out, a.verbose)

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
        # The marker: main.c's "[boot] simulator: starting the stream"
        # since afc5e00 removed "measurement running" (kept for older
        # builds). Until 27.09.2026 (P9.5) only the old one was looked for,
        # so the wait ran out, the fault landed after the check had finished
        # and the run reported PASS - a fault case that proved nothing.
        markers = ("starting the stream for the ping-pong check", "measurement running")
        while not any(k in uart() for k in markers) and time.time() - t0 < a.run_seconds:
            time.sleep(2.0)
        if not any(k in uart() for k in markers):
            m.cmd("Halt", 2)
            m.quit()
            sys.exit("--- fault case INVALID: the stream-start marker never appeared, "
                     "the fault was not written")
        time.sleep(a.fault_seconds)
        m.cmd(f"write {fault_addr} {a.fault}", 0.5)
        print(f"    wrote {a.fault} to sim_fault_once @0x{fault_addr:X} at {time.time() - t0:.0f} s")
        fault_written_at = time.time() - t0
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
    if a.fault and verdict == "PASS":
        # A fault that the check never saw: it was written after the last
        # compared half. Say so rather than let a PASS stand for a fault run.
        print(f"--- fault case INVALID: PASS although a fault was written at "
              f"{fault_written_at:.0f} s - it landed after the check (lower --fault-seconds)")

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
