#!/usr/bin/env python3
"""tools/board_run.py - one A/B board run over the console, host side.

    tools\\gui_setup.bat            once, installs pyserial/numpy into tools\\.venv
    tools\\board_run.bat COM5       the colleague's only command
    python tools/board_run.py --list       serial ports, no board needed
    python tools/board_run.py --selftest   the whole sequence against a stand-in

Drives the firmware's existing console through tools/protocol.py's Target
(sync/cmd/grab) and runs the same fixed sequence of blocks against the OLD
firmware (A) and the NEW firmware (B) on the same board, in one sitting -
board-run-task.md section 4, docs carried into the BR cards of the
implementation plan (BR.1). It never programs the board: the colleague is
told which hex file to program by hand, then presses ENTER.

Sequence per run (board-run-task.md section 4.2 / the BR card's table):

    R0  sync, version, help, status   - build/revision/board; command set
    R1  regs                         - register state after boot
    R2  chain all                    - the one-command chain test
    R3  test all                     - the back-to-back suite
    R4  stream on <1000|4000|8000>, >= 50 grabs, stream off   - the GUI path
    R5  stream on <ksps core pinsel>, 10 grabs                - non-DAC path
    R6  route list (B only)          - P11 on silicon; NOT_AVAILABLE in A
    R7  status again                 - end state

A command the firmware's own "help" reply does not list is never sent -
logged as NOT_AVAILABLE instead of a failure (R0 reads "version" and "help"
first and derives the command set from them, section 4.2a). A NAK or a
timeout on a command the firmware does claim to have is a finding, not an
abort: it is logged, and - for a timeout - the operator is asked to press
RESET so the run can continue with the next block. This is why the
sequencing core (run_session() and the run_r*() functions below) only ever
needs the small Target interface (sync/cmd/grab/close, an optional port
attribute) - a real serial Target and a stand-in are interchangeable, which
is what lets --selftest exercise the entire thing without a board.

Log line format (one run's A.log/B.log inside the output zip; this is
BR.2's tools/eval_board.py input, so it is fixed here and documented once):

    <ms> <DIR> <BLOCK> <text>

  ms     milliseconds since this run (A or B) started (a monotonic clock,
         not wall-clock time) - an integer, sort order is time order.
  DIR    TX  one line sent to the board: a command, or "stream grab".
         RX  one line received from the board: a reply line, the literal
             "[ACK]"/"[NAK]" that ends a command's reply, or a one-line
             summary of a 'stream grab' cycle ("GRAB n=.. ov=.. ...").
         EV  a runner-generated event, not board traffic - "block start",
             "block end <ok|fail|timeout|not_available>", "timeout",
             "not_available <cmd>", "capabilities: <names>", "reset
             requested", "reset: ...", "grab <i>: triangle PASS|FAIL ...".
  BLOCK  R0..R7; "R4.<ksps>" for one of R4's three rates; "SETUP" for the
         one-time initial capture before R0 (the stand-in's own "boot
         banner", when the transport can supply one); "-" for the first
         line of the log (below), which carries no single block.
  text   free text, the rest of the line - it never itself starts with
         another "<ms> <DIR> <BLOCK>" triple, so `line.split(None, 3)`
         recovers all four fields from any line.

The first line of every log is
    "0 EV - RUNNER_VERSION=<n> label=<A|B> port=<port>"
eval_board.py refuses a log whose RUNNER_VERSION it does not know rather
than misreading it (board-run-task.md section 4.2a).

Per-block timeouts (named constants below, TIMEOUT_R0..TIMEOUT_R7) are
ceilings for one call to Target.cmd()/target.sync(), not targets: a clean
run is much faster (chain all finishes in about a minute, most commands in
well under a second). Summed worst case for one run (A or B):
    R0 30 + R1 10 + R2 90 + R3 180 + R4 300 + R5 60 + R6 10 + R7 10 = 690 s
that is 11.5 minutes, under the <= 15 min/run, <= 30 min/(A+B) budget of
board-run-task.md sections 4.2/9.7 even if every block used its full
allowance; R3's 180 s is a first estimate (test all's real duration is not
yet measured on this firmware) to be revisited after the first board run.

sim_trap.py as a transport for R0/R1 (timeboxed per the BR.1 card): no. Its
UART path is one-way and after the fact - "Set uart2io.output file" writes
whatever the simulated UART sent to a file, read back only once MDB has
halted; nothing feeds bytes INTO the simulated UART receiver while it
runs (CLAUDE.md, the smoke build's own note: "the simulator's UART receiver
takes no injected bytes"). The one existing way to drive commands into a
simulator build is SIM_SMOKE's cmd_parser_feed_char() call from inside
main.c itself - a fixed script baked in at compile time, not something a
host-side Target can open a session with and send arbitrary lines to. Using
it here would mean a new build variant (a live command channel into the
simulator), which is exactly what the whole host-side-runner design
(board-run-task.md section 2) chose not to build. So: not usable as a
Target for this runner without new firmware; recorded here, not
implemented.

Requirements: pyserial, numpy (tools/requirements-board.txt) - imported
lazily where actually needed (list_ports here, "import serial" inside
protocol.Target.__init__), so importing this module, and running
--selftest, needs neither installed.
"""
import argparse
import hashlib
import json
import os
import platform
import re
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402
import protocol  # noqa: E402
import eval_board  # noqa: E402  (BR.2: the one place summary.txt's content is built)
from eval_chain import tri_eval as chain_tri_eval  # noqa: E402
from eval_chain import grid_ok as chain_grid_ok  # noqa: E402
from eval_chain import synth as chain_synth  # noqa: E402

RUNNER_VERSION = "1"

# ---------------------------------------------------------------------------
# Per-block timeouts - see the module docstring for the budget arithmetic.
# ---------------------------------------------------------------------------
TIMEOUT_SYNC = 20.0     # Target.sync()'s own default: boot + self-test + sweep
TIMEOUT_CMD = 5.0       # one line command (Target.cmd()'s own default)
TIMEOUT_GRAB = 10.0     # one 'stream grab' cycle (Target.grab()'s own default)
TIMEOUT_R0 = 30.0       # sync, version, help, status
TIMEOUT_R1 = 10.0       # regs
TIMEOUT_R2 = 90.0       # chain all - "within a minute" (CLAUDE.md) + margin
TIMEOUT_R3 = 180.0      # test all - the back-to-back suite (first estimate)
TIMEOUT_R4 = 300.0      # 3 rates x (on + >=50 grabs + off), 10s/grab ceiling
TIMEOUT_R5 = 60.0       # one rate x (on + 10 grabs + off)
TIMEOUT_R6 = 10.0       # route list (B only)
TIMEOUT_R7 = 10.0       # status again

MIN_GRABS_PER_RATE = 50            # R4
GRABS_R5 = 10                      # R5
R4_RATES_KSPS = (1000, 4000, 8000)  # 1, 4, 8 MSPS - "stream on" takes ksps
R5_DEFAULT_KSPS = 1000
R5_DEFAULT_CORE = 1
R5_DEFAULT_PINSEL = 0
R5_DEFAULT_SAMC = 0

BLOCK_ORDER = ["R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7"]


# ---------------------------------------------------------------------------
# The log
# ---------------------------------------------------------------------------
class RunLog:
    """One run's A.log/B.log - see the module docstring for the line
    format. `lines` is the list eval_board.py (BR.2) will read; `write()`
    is only a convenience for a stand-alone .log file."""

    def __init__(self):
        self.t0 = time.monotonic()
        self.lines = []

    def _ms(self):
        return int((time.monotonic() - self.t0) * 1000)

    def add(self, direction, block, text):
        line = f"{self._ms()} {direction} {block} {text}"
        self.lines.append(line)
        return line

    def tx(self, block, text):
        return self.add("TX", block, text)

    def rx(self, block, text):
        return self.add("RX", block, text)

    def ev(self, block, text):
        return self.add("EV", block, text)

    def text(self):
        return "\n".join(self.lines) + "\n"


# ---------------------------------------------------------------------------
# Capability derivation (section 4.2a): read "help" once, decide from it
# which commands exist. Handles the firmware's real multi-line form
# ("  chain all|<n>|... - the chain test" -> "chain") and a stand-in's
# compact "commands: a b c" form.
# ---------------------------------------------------------------------------
_CAP_WORD_RE = re.compile(r"^([A-Za-z][A-Za-z0-9_]*)")


def parse_capabilities(help_lines):
    caps = set()
    for line in help_lines:
        s = line.strip()
        if not s or s.lower().startswith("available commands"):
            continue
        if s.lower().startswith("commands:"):
            caps.update(s.split(":", 1)[1].split())
            continue
        m = _CAP_WORD_RE.match(s)
        if m:
            caps.add(m.group(1))
    return caps


_BOARD_RE = re.compile(r"board:\s*([A-Za-z0-9]+)")


def board_label(version_lines):
    for line in version_lines:
        m = _BOARD_RE.search(line)
        if m:
            return m.group(1)
    return "unknown"


# ---------------------------------------------------------------------------
# The sequencing core - works on any object with the Target interface
# (protocol.Target for a real board, ReplayTarget below for --selftest).
# ---------------------------------------------------------------------------
def send(target, log, block, line, timeout=TIMEOUT_CMD):
    """One command: logged TX, the reply logged RX line by line, then the
    ACK/NAK. Raises TimeoutError straight through - every caller here
    decides for itself what a timeout means for its block."""
    log.tx(block, line)
    ok, reply = target.cmd(line, timeout=timeout)
    for l in reply:
        log.rx(block, l)
    log.rx(block, "[ACK]" if ok else "[NAK]")
    return ok, reply


def read_reset_banner(target, log, block, timeout=5.0):
    """After a timeout, the operator is asked to press RESET; this reads
    whatever the reboot printed. A stand-in supplies it through
    boot_banner() (see ReplayTarget); a real Target has no such method, so
    this falls back to a raw read on its serial object (best effort - if
    the transport exposes neither, nothing is captured and the run
    continues regardless, per board-run-task.md section 2 decision 6)."""
    if hasattr(target, "boot_banner"):
        lines = target.boot_banner()
    else:
        ser = getattr(target, "ser", None)
        lines = []
        if ser is not None:
            buf = b""
            t0 = time.time()
            while time.time() - t0 < timeout:
                chunk = ser.read(4096)
                if chunk:
                    buf += chunk
                elif buf:
                    break
            lines = buf.decode("ascii", "replace").split("\n")
    for l in lines:
        s = l.rstrip("\r")
        if s.strip():
            log.rx(block, s)
    return lines


def handle_timeout(target, log, block, ui):
    """board-run-task.md section 2 decision 6 / section 4.2: log it, ask
    for a reset, read the banner, then let the caller carry on with the
    next block - this function never raises."""
    log.ev(block, "timeout")
    ui.say(f"{block}: no reply within the timeout - press RESET on the board, then ENTER")
    ui.prompt("")
    log.ev(block, "reset requested")
    banner = read_reset_banner(target, log, block)
    log.ev(block, f"reset: boot banner captured ({len(banner)} lines)" if banner
            else "reset: no banner captured")
    try:
        if hasattr(target, "sync"):
            target.sync(timeout=TIMEOUT_SYNC)
        log.ev(block, "reset: board ready again")
    except TimeoutError:
        log.ev(block, "reset: board still not ready - continuing anyway")


def grab_summary_line(n, meta, ok):
    if ok:
        return (f"GRAB n={n} ov={meta.get('overrun')} late={meta.get('late')} "
                f"missed={meta.get('missed')} slp={meta.get('slpdat')} ksps={meta.get('ksps')}")
    return f"GRAB n={n} error={meta.get('error')}"


def build_grab_blob(n, meta, samples):
    """The zip's <label>-grab-<n>.bin for a failed grab. Target.grab() /
    FakeTarget.grab() decode straight into (ok, samples, meta) and hand the
    caller no raw wire bytes (parse_grab_frame, tools/protocol.py), so this
    is the DECODED content - a reconstructed header line from meta, then
    the samples packed the way the wire payload carries them (12-bit
    values, little-endian u16) - not necessarily byte-identical to what
    actually arrived on a CRC mismatch, but what the triangle evaluator
    saw, and every header field the real frame carried."""
    header = (f"# reconstructed from decoded fields, not the raw wire bytes (see build_grab_blob())\n"
              f"GRAB n={n} ov={meta.get('overrun')} late={meta.get('late')} missed={meta.get('missed')} "
              f"slp={meta.get('slpdat')} ksps={meta.get('ksps')} dachz={meta.get('dac_hz')} "
              f"error={meta.get('error')}\n")
    payload = np.asarray(list(samples), dtype="<u2").tobytes() if len(samples) else b""
    return header.encode("ascii", "replace") + payload


def run_r0(target, log, ui):
    block = "R0"
    log.ev(block, "block start")
    try:
        if hasattr(target, "sync"):
            target.sync(timeout=TIMEOUT_SYNC)
            log.ev(block, "sync: ready")
    except TimeoutError:
        handle_timeout(target, log, block, ui)
    ok_v, version_lines = send(target, log, block, "version", timeout=TIMEOUT_CMD)
    ok_h, help_lines = send(target, log, block, "help", timeout=TIMEOUT_CMD)
    ok_s, status_lines = send(target, log, block, "status", timeout=TIMEOUT_CMD)
    caps = parse_capabilities(help_lines)
    log.ev(block, "capabilities: " + " ".join(sorted(caps)))
    verdict = "ok" if (ok_v and ok_h and ok_s) else "fail"
    log.ev(block, f"block end {verdict}")
    return dict(verdict=verdict, caps=caps, version=version_lines, status=status_lines)


def run_simple_block(target, log, caps, ui, block, cap_name, command, timeout):
    """R1 (regs), R3 (test all), R6 (route list), R7 (status): one command,
    gated on the capability the firmware's own help advertised."""
    log.ev(block, "block start")
    if cap_name not in caps:
        log.ev(block, f"not_available {cap_name}")
        log.ev(block, "block end not_available")
        return dict(verdict="not_available")
    try:
        ok, lines = send(target, log, block, command, timeout=timeout)
    except TimeoutError:
        handle_timeout(target, log, block, ui)
        log.ev(block, "block end timeout")
        return dict(verdict="timeout")
    verdict = "ok" if ok else "fail"
    log.ev(block, f"block end {verdict}")
    return dict(verdict=verdict, lines=lines)


def run_r2(target, log, caps, ui):
    block = "R2"
    log.ev(block, "block start")
    if "chain" not in caps:
        log.ev(block, "not_available chain")
        log.ev(block, "block end not_available")
        return dict(verdict="not_available")
    try:
        ok, lines = send(target, log, block, "chain all", timeout=TIMEOUT_R2)
    except TimeoutError:
        handle_timeout(target, log, block, ui)
        log.ev(block, "block end timeout")
        return dict(verdict="timeout")
    ended = any(l.strip().startswith("@END") for l in lines)
    if ok and not ended:
        log.ev(block, "chain all: no @END in the reply")
    verdict = "ok" if (ok and ended) else "fail"
    log.ev(block, f"block end {verdict}")
    return dict(verdict=verdict, lines=lines, ended=ended)


def _stream_grabs(target, log, block, label, n_grabs, check_triangle, fail_frames):
    """The >= 50 (R4) / 10 (R5) 'stream grab' loop shared by R4 and R5: one
    grab, its CRC already checked by parse_grab_frame(), a triangle verdict
    via eval_chain's own tri_eval()/grid_ok() when the frame carries the
    DAC2 test triangle (slp > 0) and the caller asked for it; a failed grab
    (CRC/NAK) or a FAIL triangle verdict is stored for the zip."""
    grabs = []
    ok_all = True
    for i in range(n_grabs):
        log.tx(block, "stream grab")
        ok, samples, meta = target.grab(timeout=TIMEOUT_GRAB)
        log.rx(block, grab_summary_line(len(samples), meta, ok))
        verdict = None
        if ok and check_triangle and meta.get("slpdat", 0) > 0:
            r = chain_tri_eval([int(v) for v in samples])
            verdict = "PASS" if chain_grid_ok(r) else "FAIL"
            log.ev(block, f"grab {i}: triangle {verdict} tps={r['tps']} slip={r['slip']:.2f}")
        elif ok and check_triangle:
            log.ev(block, f"grab {i}: no triangle check (slp=0)")
        elif not ok:
            log.ev(block, f"grab {i}: {meta.get('error')}")
        grabs.append(dict(ok=ok, verdict=verdict, meta=meta))
        if (not ok) or verdict == "FAIL":
            ok_all = False
            name = f"{label}-grab-{len(fail_frames) + 1}.bin"
            fail_frames.append((name, build_grab_blob(len(samples), meta, samples)))
    return grabs, ok_all


def run_r4(target, log, caps, ui, label, fail_frames):
    """`fail_frames` is the ONE list shared across R4 and R5 (run_session
    passes the same object to both) and mutated in place: the file names
    it hands out ("<label>-grab-<n>.bin") must be unique over the whole
    run, not just within this block, or the zip ends up with two entries
    of the same name (an earlier version of this function used a
    block-local list and hit exactly that - R4's and R5's first failure
    both came out "A-grab-1.bin")."""
    block = "R4"
    log.ev(block, "block start")
    if "stream" not in caps:
        log.ev(block, "not_available stream")
        log.ev(block, "block end not_available")
        return dict(verdict="not_available")
    ok_all = True
    per_rate = []
    try:
        for ksps in R4_RATES_KSPS:
            sub = f"{block}.{ksps}"
            ok_on, _ = send(target, log, sub, f"stream on {ksps}", timeout=TIMEOUT_CMD)
            grabs = []
            if ok_on:
                grabs, ok_grabs = _stream_grabs(target, log, sub, label, MIN_GRABS_PER_RATE,
                                                 True, fail_frames)
                ok_all = ok_all and ok_grabs
            else:
                ok_all = False
            ok_off, _ = send(target, log, sub, "stream off", timeout=TIMEOUT_CMD)
            ok_all = ok_all and ok_off
            per_rate.append(dict(ksps=ksps, on_ok=ok_on, grabs=grabs, off_ok=ok_off))
    except TimeoutError:
        handle_timeout(target, log, block, ui)
        log.ev(block, "block end timeout")
        return dict(verdict="timeout", rates=per_rate)
    verdict = "ok" if ok_all else "fail"
    log.ev(block, f"block end {verdict}")
    return dict(verdict=verdict, rates=per_rate)


def run_r5(target, log, caps, ui, label, ksps, core, pinsel, samc, fail_frames):
    """See run_r4()'s docstring for why `fail_frames` is shared, not
    block-local."""
    block = "R5"
    log.ev(block, "block start")
    if "stream" not in caps:
        log.ev(block, "not_available stream")
        log.ev(block, "block end not_available")
        return dict(verdict="not_available")
    grabs = []
    ok_all = True
    try:
        ok_on, _ = send(target, log, block, f"stream on {ksps} {core} {pinsel} {samc}", timeout=TIMEOUT_CMD)
        if ok_on:
            grabs, ok_grabs = _stream_grabs(target, log, block, label, GRABS_R5, False, fail_frames)
            ok_all = ok_grabs
        else:
            ok_all = False
        ok_off, _ = send(target, log, block, "stream off", timeout=TIMEOUT_CMD)
        ok_all = ok_all and ok_off
    except TimeoutError:
        handle_timeout(target, log, block, ui)
        log.ev(block, "block end timeout")
        return dict(verdict="timeout", grabs=grabs)
    verdict = "ok" if ok_all else "fail"
    log.ev(block, f"block end {verdict}")
    return dict(verdict=verdict, grabs=grabs)


def run_session(target, ui, label, r5_ksps=R5_DEFAULT_KSPS, r5_core=R5_DEFAULT_CORE,
                 r5_pinsel=R5_DEFAULT_PINSEL, r5_samc=R5_DEFAULT_SAMC):
    """R0..R7 against one already-open target, in order. Returns
    (log, results, fail_frames) - results[block]['verdict'] is one of
    ok/fail/timeout/not_available."""
    log = RunLog()
    log.ev("-", f"RUNNER_VERSION={RUNNER_VERSION} label={label} port={getattr(target, 'port', '?')}")
    if hasattr(target, "boot_banner"):
        for l in target.boot_banner():
            if l.strip():
                log.rx("SETUP", l.rstrip("\r"))
    results = {}
    fail_frames = []  # shared by R4 and R5 - see run_r4()'s docstring
    results["R0"] = run_r0(target, log, ui)
    caps = results["R0"].get("caps", set())
    results["R1"] = run_simple_block(target, log, caps, ui, "R1", "regs", "regs", TIMEOUT_R1)
    results["R2"] = run_r2(target, log, caps, ui)
    results["R3"] = run_simple_block(target, log, caps, ui, "R3", "test", "test all", TIMEOUT_R3)
    results["R4"] = run_r4(target, log, caps, ui, label, fail_frames)
    results["R5"] = run_r5(target, log, caps, ui, label, r5_ksps, r5_core, r5_pinsel, r5_samc, fail_frames)
    results["R6"] = run_simple_block(target, log, caps, ui, "R6", "route", "route list", TIMEOUT_R6)
    results["R7"] = run_simple_block(target, log, caps, ui, "R7", "status", "status", TIMEOUT_R7)
    return log, results, fail_frames


# ---------------------------------------------------------------------------
# Interactive layer: real prompts, or an injectable stand-in for --selftest.
# ---------------------------------------------------------------------------
class ConsoleUI:
    def prompt(self, text):
        return input(text)

    def say(self, text):
        print(text)


class StubUI:
    """Answers every prompt from a canned queue (default: an empty ENTER),
    so the whole A+B sequence runs with nobody at the keyboard."""

    def __init__(self, answers=None):
        self.answers = list(answers or [])
        self.said = []

    def prompt(self, text):
        self.said.append(f"PROMPT: {text}")
        return self.answers.pop(0) if self.answers else ""

    def say(self, text):
        self.said.append(text)


# ---------------------------------------------------------------------------
# SHA-256 against SHA256SUMS.txt, if the package that shipped the hex file
# put one next to it (board-run-task.md section 4.1a / BR.4).
# ---------------------------------------------------------------------------
def compute_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_sha256sums(path):
    sums = {}
    if not os.path.exists(path):
        return sums
    with open(path, encoding="ascii", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 1)
            if len(parts) == 2:
                digest, name = parts
                sums[os.path.basename(name.lstrip("*").strip())] = digest.lower()
    return sums


def check_hex_sha256(path):
    """Returns (digest, status); status is 'match'/'mismatch'/'no_entry'/
    'no_sums_file' - only 'mismatch' is ever worth stopping the operator
    for, and even that is their call (see prepare_hex())."""
    digest = compute_sha256(path)
    sums_path = os.path.join(os.path.dirname(os.path.abspath(path)), "SHA256SUMS.txt")
    sums = load_sha256sums(sums_path)
    if not sums:
        return digest, "no_sums_file"
    name = os.path.basename(path)
    if name not in sums:
        return digest, "no_entry"
    return digest, "match" if sums[name] == digest.lower() else "mismatch"


def prepare_hex(path, which, ui):
    if not path:
        return dict(which=which, path=None, sha256=None, sha256_status="not_checked")
    digest, status = check_hex_sha256(path)
    if status == "mismatch":
        ui.say(f"WARNING: SHA-256 mismatch for {path} against its SHA256SUMS.txt")
        if ui.prompt("continue anyway? [y/N] ").strip().lower() != "y":
            raise RuntimeError(f"aborted: SHA-256 mismatch for {path}")
    return dict(which=which, path=os.path.abspath(path), sha256=digest, sha256_status=status)


# ---------------------------------------------------------------------------
# Port discovery (section 4.1b)
# ---------------------------------------------------------------------------
_LIKELY_DESC_RE = re.compile(r"PKOB|nEDBG|Curiosity", re.I)


def _list_ports():
    import serial.tools.list_ports as list_ports_mod
    return list(list_ports_mod.comports())


def print_port_list():
    try:
        ports = _list_ports()
    except ImportError:
        print("pyserial not installed - install it first (tools/requirements-board.txt)")
        return
    if not ports:
        print("no serial ports found")
        return
    for p in ports:
        mark = "  <- likely the board" if _LIKELY_DESC_RE.search(p.description or "") else ""
        vidpid = f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None and p.pid is not None else "----:----"
        print(f"{p.device}  {vidpid}  {p.description}{mark}")


def pick_port(explicit):
    if explicit:
        return explicit
    try:
        ports = _list_ports()
    except ImportError:
        sys.exit("pyserial not installed - install it first (tools/requirements-board.txt), "
                  "or pass a port explicitly")
    candidates = [p for p in ports if _LIKELY_DESC_RE.search(p.description or "")]
    pool = candidates or ports
    if len(pool) == 1:
        print(f"using {pool[0].device} ({pool[0].description}) - the only candidate")
        return pool[0].device
    if not pool:
        sys.exit("no serial ports found - pass a port explicitly")
    sys.exit("more than one serial port candidate - pass one explicitly: "
              + ", ".join(p.device for p in pool))


def open_target(port, ui):
    try:
        return protocol.Target(port)
    except Exception as e:
        text = str(e)
        if isinstance(e, PermissionError) or "denied" in text.lower() or "access" in text.lower():
            ui.say(f"cannot open {port}: access denied - another program has it open "
                   "(the GUI, a terminal program, or MPLAB X's own terminal). Close it and retry.")
        else:
            ui.say(f"cannot open {port}: {e}")
        raise


# ---------------------------------------------------------------------------
# Output: summary.txt, session.json, the zip
#
# summary.txt's content is tools/eval_board.py's build_summary_text() (BR.2)
# - it parses the very log lines this module just wrote, exactly as it
# would parse them again later from the zip, so a live run's summary.txt
# and eval_board.py's own after-the-fact report can never disagree; this
# module no longer has a summary-building function of its own.
# ---------------------------------------------------------------------------
def write_zip(out_dir, board, session, log_a, log_b, frames_a, frames_b):
    ts = time.strftime("%Y%m%d-%H%M%S")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"run-{ts}-{board}.zip")
    summary = eval_board.build_summary_text(log_a.lines, log_b.lines, eval_board.load_expected())
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("A.log", log_a.text())
        z.writestr("B.log", log_b.text())
        z.writestr("summary.txt", summary)
        z.writestr("session.json", json.dumps(session, indent=2, sort_keys=True))
        for name, blob in frames_a:
            z.writestr(name, blob)
        for name, blob in frames_b:
            z.writestr(name, blob)
    return path, summary


# ---------------------------------------------------------------------------
# The real, interactive run
# ---------------------------------------------------------------------------
def perform_full_run(port, ui, out_dir, hex_a=None, hex_b=None,
                      r5_ksps=R5_DEFAULT_KSPS, r5_core=R5_DEFAULT_CORE,
                      r5_pinsel=R5_DEFAULT_PINSEL, r5_samc=R5_DEFAULT_SAMC):
    try:
        import serial
        pyserial_version = serial.__version__
    except ImportError:
        pyserial_version = None

    session = dict(runner_version=RUNNER_VERSION, port=port,
                    pc_time=time.strftime("%Y-%m-%d %H:%M:%S"),
                    python=platform.python_version(), pyserial=pyserial_version, hex=[])

    target = open_target(port, ui)
    try:
        info_a = prepare_hex(hex_a, "OLD", ui)
        session["hex"].append(info_a)
        ui.prompt(f"program the OLD firmware{' (' + hex_a + ')' if hex_a else ''}, then press ENTER: ")
        log_a, results_a, frames_a = run_session(target, ui, "A", r5_ksps, r5_core, r5_pinsel, r5_samc)

        info_b = prepare_hex(hex_b, "NEW", ui)
        session["hex"].append(info_b)
        ui.prompt(f"program the NEW firmware{' (' + hex_b + ')' if hex_b else ''}, then press ENTER: ")
        if hasattr(target, "sync"):
            target.sync(timeout=TIMEOUT_SYNC)  # reprogramming rebooted the board
        log_b, results_b, frames_b = run_session(target, ui, "B", r5_ksps, r5_core, r5_pinsel, r5_samc)
    finally:
        target.close()

    yn = ui.prompt("R5: signal generator connected to the input pin? [y/N] ").strip().lower()
    signal_present = yn.startswith("y")
    freq = ui.prompt("R5: signal frequency (Hz)? ").strip() if signal_present else None
    session["r5_signal"] = dict(present=signal_present, frequency_hz=freq)

    board = board_label(results_a["R0"].get("version", []))
    zip_path, summary = write_zip(out_dir, board, session, log_a, log_b, frames_a, frames_b)
    print(summary)
    print(f"file to send: {zip_path}")
    return 0 if summary.rstrip("\n").splitlines()[-1] == "overview: PASS" else 1


# ---------------------------------------------------------------------------
# --selftest: a replayed A and B firmware, no board.
# ---------------------------------------------------------------------------
# The clean "chain all" reply every ReplayTarget answers with by default -
# a module-level constant so BR.2's eval_board.py selftest (tools/eval_board.py)
# can build a second, deliberately-FAILing variant of it without duplicating
# the PASS lines, and so both selftests are provably looking at the same
# baseline text.
DEFAULT_CHAIN_ALL_LINES = [
    "@S0.1 hz=160000000 expect=160000000 -> PASS",
    "@S4.1 ksps=1000 xfer=1000 expect=1000 tol=5 overrun=0 brake=0 -> PASS",
    "@S4.2 ksps=4000 xfer=4000 expect=4000 tol=5 overrun=0 brake=0 -> PASS",
    "@S4.3 ksps=8000 xfer=8000 expect=8000 tol=5 overrun=0 brake=0 -> PASS",
    "@S5.0 ksps=100 slip_x100=5 zero=0 dbl=0 up=1 dn=1 slip_n=2 ratio_x1000=1000 -> PASS",
    "@S5.1 ksps=1000 slip_x100=5 zero=0 dbl=0 up=1 dn=1 slip_n=2 ratio_x1000=1000 -> PASS",
    "@S5.2 ksps=4000 slip_x100=5 zero=0 dbl=0 up=1 dn=1 slip_n=2 ratio_x1000=1000 -> PASS",
    "@S5.3 ksps=8000 slip_x100=5 zero=0 dbl=0 up=1 dn=1 slip_n=2 ratio_x1000=1000 -> PASS",
    "@S5.4 ksps=10000 slip_x100=8 zero=0 dbl=0 up=1 dn=1 slip_n=2 ratio_x1000=1000 -> PASS",
    "@S6.1 ksps=1000 xfer=1000 expect=1000 tol=5 overrun=0 late=0 missed=0 isr=2 half=1 "
    "done=1 brake=0 guard_ok=1 load_max_x10=200 free_cyc_per_sample=10 -> PASS",
    "@S6.2 ksps=4000 xfer=4000 expect=4000 tol=5 overrun=0 late=0 missed=0 isr=2 half=1 "
    "done=1 brake=0 guard_ok=1 load_max_x10=350 free_cyc_per_sample=8 -> PASS",
    "@S6.3 ksps=8000 xfer=8000 expect=8000 tol=5 overrun=0 late=0 missed=0 isr=2 half=1 "
    "done=1 brake=0 guard_ok=1 load_max_x10=460 free_cyc_per_sample=6 -> PASS",
    "@S9.1 ksps=8000 xfer=8000 expect=8000 tol=5 overrun=0 late=0 missed=0 isr=2 half=1 "
    "done=1 brake=0 guard_ok=1 load_max_x10=460 free_cyc_per_sample=6 -> PASS",
    "@SUM stages=10 pass=13 fail=0",
    "@END",
]


class ReplayTarget:
    """Stands in for protocol.Target in --selftest: answers like the
    firmware's console would for the commands this runner sends, with
    several injectable faults used to exercise the runner's (and, via
    `chain_all_lines`/`extra_status_fields`, eval_board.py's) own handling
    of them - a command that times out once (`timeout_block`), a command
    that NAKs once (`nak_once`), one 'stream grab' that comes back with a
    corrupted CRC (`grab_fault_at`, a (ksps, grab index) pair), a
    "chain all" reply other than the clean default (`chain_all_lines`, used
    by eval_board.py's selftest to manufacture an A/B difference at a known
    stage), and extra "key: value" lines appended to "status" beyond the
    baseline set (`extra_status_fields`, standing in for BR.6's not-yet-named
    fields - present in B, absent in A)."""

    def __init__(self, label, has_route, timeout_block=None, nak_once=None, grab_fault_at=None,
                 chain_all_lines=None, extra_status_fields=None):
        self.label = label
        self.has_route = has_route
        self.timeout_block = timeout_block
        self._timeout_fired = False
        self.nak_once = set(nak_once or ())
        self._nak_fired = set()
        self.grab_fault_at = grab_fault_at
        self.chain_all_lines = chain_all_lines or DEFAULT_CHAIN_ALL_LINES
        self.extra_status_fields = extra_status_fields or {}
        self.port = f"replay-{label}"
        self.chain_on = False
        self.chain_ksps = 0
        self.chain_test = True
        self._grab_i = {}
        self._reset_count = 0

    def close(self):
        pass

    def sync(self, timeout=20.0):
        return True

    def boot_banner(self):
        """Called once up front (the "power-up" banner) and again after a
        simulated reset, deliberately with a different reset cause the
        second time - so a selftest can tell the two apart."""
        self._reset_count += 1
        cause = "POR BOR EXTR" if self._reset_count == 1 else "EXTR (simulated reset)"
        return [
            "",
            "[boot] uart up on FRC, 115200 8N1",
            f"[boot] adc_dma_40msps replay-selftest git replay-{self.label} (master)",
            f"[boot] reset cause: {cause}",
            f"adc_dma_40msps - ADC at 40 MSPS into RAM via DMA (replay {self.label})",
        ]

    def cmd(self, line, timeout=5.0):
        if self.timeout_block is not None and line == self.timeout_block and not self._timeout_fired:
            self._timeout_fired = True
            raise TimeoutError(f"replay: simulated hang on {line!r}")
        if line in self.nak_once and line not in self._nak_fired:
            self._nak_fired.add(line)
            return False, [f"NAK: simulated fault on {line!r}"]
        return self._reply(line)

    def _reply(self, line):
        parts = line.split()
        c = parts[0] if parts else ""
        if c == "version":
            return True, [f"[build] adc_dma_40msps replay {self.label}",
                          "[build] board: EV74H48A, dsPIC33AK512MPS512 GP DIM"]
        if c == "help":
            lines = ["Available commands:",
                     "  help - show this help",
                     "  version - build id, git revision, board, configuration",
                     "  status - run state and counters",
                     "  regs - clock, ADC, DMA and UART registers",
                     "  chain all|<n>|from <n>|run <ksps> [s] - the chain test",
                     "  test [all|self|clock|clkoff|bursts|matrix|rate|sweep|dac] [n]",
                     "  stream on <ksps> [core pinsel [samc]]|off|grab - the chain streaming"]
            if self.has_route:
                lines.append("  route list - the routing table")
            return True, lines
        if c == "status":
            lines = ["running: " + ("1" if self.chain_on else "0"),
                     "overrun: 0", "late: 0", "missed: 0", "fail_code: 0"]
            lines += [f"{k}: {v}" for k, v in self.extra_status_fields.items()]
            return True, lines
        if c == "regs":
            return True, ["CLK1CON: 0x00001234", "AD5CON1: 0x00005678"]
        if line == "chain all":
            return True, self.chain_all_lines
        if line == "test all":
            return True, ["[test] self: PASS", "[test] clock: PASS", "[test] all: done"]
        if c == "route" and len(parts) > 1 and parts[1] == "list":
            if not self.has_route:
                return False, ["unknown command"]
            return True, ["route 1: core 5 <- SCCP1 -> DMA0", "routes: 1 active"]
        if c == "stream":
            return self._stream_cmd(parts[1:])
        return False, ["unknown command"]

    def _stream_cmd(self, args):
        if args and args[0] == "on":
            rest = args[1:]
            self.chain_on = True
            self.chain_ksps = int(rest[0])
            self.chain_test = len(rest) <= 1
            self._grab_i[self.chain_ksps] = 0
            return True, [f"stream: on - {self.chain_ksps} ksps"]
        if args and args[0] == "off":
            self.chain_on = False
            return True, ["stream: off, boot configuration restored"]
        return False, ["unknown command"]

    def grab(self, timeout=10.0):
        if not self.chain_on:
            header = "GRAB n=0 from=0 ksps=0 ov=0 late=0 missed=0 halves=0 xfer=0 slp=0 dachz=0\r\n"
            crc = protocol.crc16_ccitt_false(b"")
            tail = f"\r\nCRC {crc:04X}\r\n> ".encode("ascii") + protocol.NAK
            return protocol.parse_grab_frame(header, b"", tail)
        i = self._grab_i.get(self.chain_ksps, 0)
        self._grab_i[self.chain_ksps] = i + 1
        n = 512
        if self.chain_test:
            slp = 20
            samples = chain_synth(n, 30.0, phase=float((i * 7) % 60 or 1), seed=i + 1)
        else:
            slp = 0
            samples = [2048 + (i * 37) % 100] * n
        payload = np.asarray(samples, dtype="<u2").tobytes()
        crc = protocol.crc16_ccitt_false(payload)
        if self.grab_fault_at == (self.chain_ksps, i):
            payload = bytes([payload[0] ^ 0xFF]) + payload[1:]
        header = (f"GRAB n={n} from=0 ksps={self.chain_ksps} ov=0 late=0 missed=0 "
                  f"halves=2 xfer={2 * n} slp={slp} dachz=400000000\r\n")
        tail = f"\r\nCRC {crc:04X}\r\n> ".encode("ascii") + protocol.ACK
        return protocol.parse_grab_frame(header, payload, tail)


def selftest():
    ok_all = True

    def check(label, cond):
        nonlocal ok_all
        ok_all = ok_all and bool(cond)
        print(f"  {'ok ' if cond else 'BAD'} {label}")
        return cond

    ui = StubUI(answers=["", "", "n"])  # ENTER x2 (reprogram prompts), "n" (no R5 signal)
    target_a = ReplayTarget("A", has_route=False, timeout_block="test all",
                             grab_fault_at=(R4_RATES_KSPS[0], 5))
    target_b = ReplayTarget("B", has_route=True, nak_once={"regs"})

    log_a, results_a, frames_a = run_session(target_a, ui, "A")
    log_b, results_b, frames_b = run_session(target_b, ui, "B")

    check("A: R3 (test all) times out", results_a["R3"]["verdict"] == "timeout")
    check("A: the reset's boot banner differs from the initial one",
          any("POR BOR EXTR" in l for l in log_a.lines)
          and any("EXTR (simulated reset)" in l for l in log_a.lines))
    check("A: R6 (route list) is NOT_AVAILABLE (no 'route' in help)",
          results_a["R6"]["verdict"] == "not_available")
    check("A: the injected grab fault produced a fail frame", len(frames_a) >= 1)
    check("A: run completes past the timeout (R4..R7 still ran)",
          all(results_a[b]["verdict"] != "missing" for b in ("R4", "R5", "R6", "R7")))

    check("B: R1 (regs) shows the simulated NAK",
          results_b["R1"]["verdict"] == "fail"
          and any("R1" in l and "[NAK]" in l for l in log_b.lines))
    check("B: R6 (route list) runs (has 'route' in help)", results_b["R6"]["verdict"] == "ok")

    with tempfile.TemporaryDirectory() as tmp:
        session = dict(runner_version=RUNNER_VERSION, port="replay", pc_time="selftest",
                        python=platform.python_version(), pyserial=None, hex=[],
                        r5_signal=dict(present=False, frequency_hz=None))
        zip_path, _ = write_zip(tmp, "EV74H48A-selftest", session, log_a, log_b, frames_a, frames_b)
        with zipfile.ZipFile(zip_path) as z:
            names = z.namelist()
        check("zip contains A.log/B.log/summary.txt/session.json",
              all(n in names for n in ("A.log", "B.log", "summary.txt", "session.json")))
        check("zip contains a fail-frame .bin for the A grab fault",
              any(n.startswith("A-grab-") for n in names))
        with zipfile.ZipFile(zip_path) as z:
            session_back = json.loads(z.read("session.json"))
        check("session.json round-trips", session_back["runner_version"] == RUNNER_VERSION)

    print("board_run", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


# ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", nargs="?",
                     help="serial port, e.g. COM5 (auto-picked if omitted and exactly one candidate)")
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    ap.add_argument("--selftest", action="store_true",
                     help="run the full A+B sequence against a stand-in, no board")
    ap.add_argument("--hex-a", help="the OLD firmware's hex file (checked against a SHA256SUMS.txt "
                                     "next to it, if present)")
    ap.add_argument("--hex-b", help="the NEW firmware's hex file")
    ap.add_argument("--out-dir", default=".", help="where to write run-*.zip (default: .)")
    ap.add_argument("--r5-ksps", type=int, default=R5_DEFAULT_KSPS)
    ap.add_argument("--r5-core", type=int, default=R5_DEFAULT_CORE)
    ap.add_argument("--r5-pinsel", type=int, default=R5_DEFAULT_PINSEL)
    ap.add_argument("--r5-samc", type=int, default=R5_DEFAULT_SAMC)
    a = ap.parse_args(argv)

    if a.selftest:
        return selftest()
    if a.list:
        print_port_list()
        return 0

    port = pick_port(a.port)
    ui = ConsoleUI()
    return perform_full_run(port, ui, a.out_dir, a.hex_a, a.hex_b,
                             a.r5_ksps, a.r5_core, a.r5_pinsel, a.r5_samc)


if __name__ == "__main__":
    sys.exit(main())
