#!/usr/bin/env python3
"""
sim_cli.py - drive the example's command console inside the MPLAB X
simulator, from Python, without a board.

How it works (measured on MPLAB X v6.35, see docs/SIMULATION.md):

  * MDB, the command-line debugger that ships with MPLAB X, is started as a
    subprocess and driven over its stdin/stdout.
  * The simulator writes everything the firmware sends on UART1 into a
    file ("Set uart1io.output file"). That is the console's TX path, and it
    is the real UART peripheral.
  * The simulator does NOT model UART reception for this device. The
    firmware's simulator build therefore reads its console input from a
    RAM mailbox (sim_rx_words[] / sim_rx_len in cli.c), which this script
    fills with MDB "write" commands while the simulation runs. Every value
    of "write" is one 32-bit word, so a line is packed four characters per
    word and the byte count is written last.
  * The reply is complete when the output file ends with the parser's
    readiness byte: ACK (0x06) for success, NAK (0x15) for failure. That is
    the same rule a script uses against the real board.

Usage:
    python sim_cli.py --elf ..\\build\\adc_dma_40msps_sim.elf              interactive
    python sim_cli.py --elf ..\\build\\adc_dma_40msps_sim.elf --test       built-in test sequence
    python sim_cli.py --elf ... --send "status" --send "samc 3"           one-off commands

Build the simulator ELF first:  build.bat sim     (adds -D__MPLAB_DEBUGGER_SIMULATOR=1 -g)

The simulator runs at roughly 1/80 of real time, so expect one to three
seconds per command.
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import threading
import time

ACK = b"\x06"
NAK = b"\x15"
DEVICE = "dsPIC33AK512MPS512"
MBOX_WORDS = 16          # must match cli.c


# ---------------------------------------------------------------------------
# Tool discovery
# ---------------------------------------------------------------------------
def newest(paths):
    def key(p):
        return tuple(int(n) for n in re.findall(r"\d+", os.path.basename(p)))
    return sorted(paths, key=key)[-1] if paths else None


def find_mdb():
    for pf in (os.environ.get("ProgramFiles", r"C:\Program Files"),
               os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        hits = glob.glob(os.path.join(pf, "Microchip", "MPLABX", "v*",
                                      "mplab_platform", "bin", "mdb.bat"))
        if hits:
            return newest(hits)
    return None


def find_nm():
    for pf in (os.environ.get("ProgramFiles", r"C:\Program Files"),
               os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        hits = glob.glob(os.path.join(pf, "Microchip", "xc-dsc", "v*", "bin",
                                      "xc-dsc-nm.exe"))
        if hits:
            return newest(hits)
    return None


def symbol_addresses(elf, names):
    """Addresses of C globals via xc-dsc-nm (C names carry a leading '_')."""
    nm = find_nm()
    if nm is None:
        sys.exit("xc-dsc-nm.exe not found - is XC-DSC installed?")
    out = subprocess.run([nm, elf], capture_output=True, text=True).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2].startswith("_"):
            sym = parts[2][1:]
            if sym in names:
                found[sym] = int(parts[0], 16)
    missing = [n for n in names if n not in found]
    if missing:
        sys.exit(f"symbols not found in {elf}: {missing} - build with -g and "
                 "-D__MPLAB_DEBUGGER_SIMULATOR=1 (build.bat sim)")
    return found


# ---------------------------------------------------------------------------
# MDB session
# ---------------------------------------------------------------------------
NOISE = re.compile(r"W0107|INFO:|^\w{3} \d+, \d{4}|org\.|WARNING: (NetBeans|Unable)|"
                   r"^>?\s*$|Resetting|file:|address:|source line|Stop at")


class Mdb:
    def __init__(self, mdb_bat, verbose=False):
        self.verbose = verbose
        self.proc = subprocess.Popen(["cmd", "/c", mdb_bat], stdin=subprocess.PIPE,
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

    def quit(self):
        try:
            self.cmd("Quit", 1.0)
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


class SimConsole:
    """The console of the simulated firmware, seen through MDB."""

    def __init__(self, elf, mdb_bat, verbose=False):
        self.elf = os.path.abspath(elf)
        self.out_file = os.path.abspath(os.path.splitext(self.elf)[0] + ".uart.txt")
        self.addr = symbol_addresses(self.elf, ["sim_rx_words", "sim_rx_len"])
        if os.path.exists(self.out_file):
            os.remove(self.out_file)
        self.pos = 0
        self.mdb = Mdb(mdb_bat, verbose)
        m = self.mdb
        m.cmd(f"Device {DEVICE}", 3)
        m.cmd("Set uart1io.uartioenabled true")
        m.cmd("Set uart1io.output file")
        m.cmd(f"Set uart1io.outputfile {self.out_file}")
        m.cmd("Set oscillator.frequency 8")
        m.cmd("Set oscillator.frequencyunit Mega")
        m.cmd("Hwtool SIM", 2)
        if not m.wait_for(r"Resetting peripherals|>", 60):
            raise RuntimeError("simulator did not come up")
        m.cmd(f'Program "{self.elf}"', 2)
        if not m.wait_for(r"Program succeeded", 90):
            raise RuntimeError("programming failed:\n" + "\n".join(m.lines[-10:]))
        m.cmd("Run", 1)
        # The firmware emits prompt + ACK once at start: synchronise on it.
        self._read_reply(timeout=60)

    def _output(self):
        try:
            with open(self.out_file, "rb") as f:
                return f.read()
        except FileNotFoundError:
            return b""

    def _read_reply(self, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            data = self._output()
            new = data[self.pos:]
            if new.endswith(ACK) or new.endswith(NAK):
                self.pos = len(data)
                return new
            time.sleep(0.2)
        raise TimeoutError("no readiness byte within %.0f s; got %r"
                           % (timeout, self._output()[self.pos:]))

    def send(self, line, timeout=90):
        """Send one command line, return (ok, text) once the parser is ready."""
        payload = line.encode("ascii") + b"\r"
        if len(payload) > 4 * MBOX_WORDS:
            raise ValueError("line too long for the mailbox")
        words = []
        for i in range(0, len(payload), 4):
            chunk = payload[i:i + 4].ljust(4, b"\0")
            words.append(int.from_bytes(chunk, "little"))
        # Data first, the byte count last: the firmware polls the count.
        self.mdb.cmd("write 0x%X %s" % (self.addr["sim_rx_words"],
                                        " ".join(str(w) for w in words)), 0.3)
        self.mdb.cmd("write 0x%X %d" % (self.addr["sim_rx_len"], len(payload)), 0.1)
        raw = self._read_reply(timeout)
        ok = raw.endswith(ACK)
        body = raw
        echo = line.encode("ascii") + b"\r\n"
        if body.startswith(echo):
            body = body[len(echo):]
        body = body[:-1]                       # ACK/NAK
        if body.endswith(b"> "):
            body = body[:-2]
        return ok, body.decode("ascii", "replace")

    def close(self):
        self.mdb.quit()


# ---------------------------------------------------------------------------
# Built-in test sequence
# ---------------------------------------------------------------------------
TESTS = [
    # command,          expect ok,  substring that must appear in the reply
    ("version",         True,  "adc_dma_40msps"),
    ("help",            True,  "status"),
    ("status",          True,  "blocks"),
    ("led on",          True,  "led: on"),
    ("led off",         True,  "led: off"),
    ("led auto",        True,  "led: auto"),
    ("led sideways",    False, "usage"),
    ("samc 3",          True,  "samc: 3"),
    ("samc 99",         False, "0..31"),
    ("input 6",         True,  "input: 6"),
    ("input",           False, "usage"),
    ("start",           True,  "running: 1"),
    ("stop",            True,  "running: 0"),
    ("stats",           True,  "mean"),
    ("dump 8",          True,  "0000:"),
    ("dump 4 1020",     True,  "1020:"),
    ("dump 4 2044",     False, "offset"),      # a half has 1024 samples
    ("clear",           True,  "cleared"),
    ("selftest",        False, "selftest:"),    # no ADC/DMA in the simulator: NAK
    ("nosuchcommand",   False, ""),
]


def run_tests(console):
    passed = 0
    for cmd, expect_ok, needle in TESTS:
        try:
            ok, text = console.send(cmd)
        except TimeoutError as exc:
            print(f"FAIL  {cmd!r}: {exc}")
            continue
        good = (ok == expect_ok) and (needle in text)
        passed += good
        flag = "ok  " if good else "FAIL"
        first = text.strip().splitlines()[0] if text.strip() else ""
        print(f"{flag}  {cmd:<16} -> {'ACK' if ok else 'NAK'}  {first[:60]}")
        if not good:
            print("      expected", "ACK" if expect_ok else "NAK",
                  f"with {needle!r}; full reply:\n" +
                  "\n".join("      | " + l for l in text.splitlines()))
    print(f"\n{passed} of {len(TESTS)} passed")
    return passed == len(TESTS)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", required=True, help="simulator build of the firmware")
    ap.add_argument("--mdb", help="path to mdb.bat (default: newest MPLAB X)")
    ap.add_argument("--test", action="store_true", help="run the built-in sequence")
    ap.add_argument("--send", action="append", default=[], help="send this line and exit")
    ap.add_argument("-v", "--verbose", action="store_true", help="show MDB traffic")
    args = ap.parse_args()

    mdb = args.mdb or find_mdb()
    if not mdb:
        sys.exit("mdb.bat not found - is MPLAB X installed?")

    print(f"MDB: {mdb}\nELF: {args.elf}\nstarting the simulator (about 30 s) ...")
    console = SimConsole(args.elf, mdb, args.verbose)
    print("console is up\n")
    try:
        if args.test:
            ok = run_tests(console)
            sys.exit(0 if ok else 1)
        for line in args.send:
            ok, text = console.send(line)
            print(f"{line} -> {'ACK' if ok else 'NAK'}\n{text}")
        if not args.send:
            print("interactive - type commands, empty line to quit")
            while True:
                try:
                    line = input("sim> ")
                except EOFError:
                    break
                if not line:
                    break
                ok, text = console.send(line)
                print(text, end="" if text.endswith("\n") else "\n")
                print("[ACK]" if ok else "[NAK]")
    finally:
        console.close()


if __name__ == "__main__":
    main()
