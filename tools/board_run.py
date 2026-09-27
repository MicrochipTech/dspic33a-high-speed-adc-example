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

BR.4: the hex files are found by this script itself, one `A-*.hex` and one
`B-*.hex` in board_run/ next to this repository's root (find_hex()), each
checked against board_run/SHA256SUMS.txt (prepare_hex()) before the run - a
stale, locally modified, or unlisted file stops the run and asks the
colleague to confirm before continuing, a missing SHA256SUMS.txt or A file
stops it outright. Until BR.8 adds a B-*.hex there is no B to find at all:
run B is skipped, not attempted against a file that does not exist, and the
console and summary.txt both say so plainly (`perform_full_run()`). Before
run A, the hardware set-up checklist is parsed out of board_run/README.md's
own marked section (load_checklist()) and printed for the colleague to
confirm with ENTER - one source of that text, not two. `git rev-parse HEAD`
and `git status --porcelain` (get_git_info()) go into session.json; a dirty
tree or git itself being unavailable is a warning, never a reason to stop
(board-run-task.md section 4.1a).

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
import subprocess
import sys
import tempfile
import time
import zipfile
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402
import protocol  # noqa: E402
import remote  # noqa: E402  (--remote: RemoteBench over bench_client's flash/tunnel requests)
import eval_board  # noqa: E402  (BR.2: the one place summary.txt's content is built)
from eval_chain import tri_eval as chain_tri_eval  # noqa: E402
from eval_chain import grid_ok as chain_grid_ok  # noqa: E402
from eval_chain import synth as chain_synth  # noqa: E402

RUNNER_VERSION = "1"

# ---------------------------------------------------------------------------
# BR.4 paths: this script lives in <repo>/tools/, board_run/ is its sibling.
# ---------------------------------------------------------------------------
REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BOARD_RUN_DIR = os.path.join(REPO_ROOT, "board_run")
README_PATH = os.path.join(BOARD_RUN_DIR, "README.md")
CHECKLIST_BEGIN = "BOARD_RUN_CHECKLIST:BEGIN"
CHECKLIST_END = "BOARD_RUN_CHECKLIST:END"

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
# core 3 / pinsel 5 = AD3AN5 = RA0 = DIM pin P77 = mikroBUS A socket, pin
# "AN" - tools/boards.py's own "default_core"/"default_pinsel" for
# EV74H48A, the same input README.md calls out as the default analog input
# (BR.4, board_run/README.md). NOT core 1 / pinsel 0: on THIS board that is
# AD1AN0 = RA2, which the board wires to a capacitive touch pad instead of a
# header pin (README.md: "that is why the example uses ADC3 here and not
# ADC1") - a real regression found while writing board_run/README.md's R5
# section, fixed here rather than documented as-is.
R5_DEFAULT_CORE = 3
R5_DEFAULT_PINSEL = 5
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


def handle_timeout(target, log, block, ui, remote=False):
    """board-run-task.md section 2 decision 6 / section 4.2: log it, ask
    for a reset, read the banner, then let the caller carry on with the
    next block - this function never raises.

    remote=True (--remote, BR's remote-bench path): there is nobody at the
    board to press RESET, and the recovery is automatic - close the tunnel,
    re-flash the SAME image (which resets the board the way RESET would),
    reopen the tunnel, and read the banner. This is exactly what
    RemoteTarget.boot_banner() does; read_reset_banner() already calls
    target.boot_banner() when the target has one (ReplayTarget for the
    local --selftest, RemoteTarget here), so the only difference at this
    level is what is printed and that no ENTER is waited for."""
    log.ev(block, "timeout")
    if remote:
        ui.say(f"{block}: no reply within the timeout - closing the tunnel and "
               "re-flashing to reset the board")
        log.ev(block, "reset: remote re-flash")
    else:
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


def run_r0(target, log, ui, remote=False):
    block = "R0"
    log.ev(block, "block start")
    try:
        if hasattr(target, "sync"):
            target.sync(timeout=TIMEOUT_SYNC)
            log.ev(block, "sync: ready")
    except TimeoutError:
        handle_timeout(target, log, block, ui, remote=remote)
    ok_v, version_lines = send(target, log, block, "version", timeout=TIMEOUT_CMD)
    ok_h, help_lines = send(target, log, block, "help", timeout=TIMEOUT_CMD)
    ok_s, status_lines = send(target, log, block, "status", timeout=TIMEOUT_CMD)
    caps = parse_capabilities(help_lines)
    log.ev(block, "capabilities: " + " ".join(sorted(caps)))
    verdict = "ok" if (ok_v and ok_h and ok_s) else "fail"
    log.ev(block, f"block end {verdict}")
    return dict(verdict=verdict, caps=caps, version=version_lines, status=status_lines)


def run_simple_block(target, log, caps, ui, block, cap_name, command, timeout, remote=False):
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
        handle_timeout(target, log, block, ui, remote=remote)
        log.ev(block, "block end timeout")
        return dict(verdict="timeout")
    verdict = "ok" if ok else "fail"
    log.ev(block, f"block end {verdict}")
    return dict(verdict=verdict, lines=lines)


def run_r2(target, log, caps, ui, remote=False):
    block = "R2"
    log.ev(block, "block start")
    if "chain" not in caps:
        log.ev(block, "not_available chain")
        log.ev(block, "block end not_available")
        return dict(verdict="not_available")
    try:
        ok, lines = send(target, log, block, "chain all", timeout=TIMEOUT_R2)
    except TimeoutError:
        handle_timeout(target, log, block, ui, remote=remote)
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


def run_r4(target, log, caps, ui, label, fail_frames, remote=False):
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
        handle_timeout(target, log, block, ui, remote=remote)
        log.ev(block, "block end timeout")
        return dict(verdict="timeout", rates=per_rate)
    verdict = "ok" if ok_all else "fail"
    log.ev(block, f"block end {verdict}")
    return dict(verdict=verdict, rates=per_rate)


def run_r5(target, log, caps, ui, label, ksps, core, pinsel, samc, fail_frames, remote=False):
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
        handle_timeout(target, log, block, ui, remote=remote)
        log.ev(block, "block end timeout")
        return dict(verdict="timeout", grabs=grabs)
    verdict = "ok" if ok_all else "fail"
    log.ev(block, f"block end {verdict}")
    return dict(verdict=verdict, grabs=grabs)


def run_session(target, ui, label, r5_ksps=R5_DEFAULT_KSPS, r5_core=R5_DEFAULT_CORE,
                 r5_pinsel=R5_DEFAULT_PINSEL, r5_samc=R5_DEFAULT_SAMC, remote=False):
    """R0..R7 against one already-open target, in order. Returns
    (log, results, fail_frames) - results[block]['verdict'] is one of
    ok/fail/timeout/not_available.

    remote=True (--remote) only changes what a mid-run timeout does
    (handle_timeout()'s own docstring): the sequencing itself, and the
    small Target interface this needs (sync/cmd/grab/close, an optional
    boot_banner), is exactly the one protocol.Target, ReplayTarget and
    RemoteTarget (below) all implement."""
    log = RunLog()
    log.ev("-", f"RUNNER_VERSION={RUNNER_VERSION} label={label} port={getattr(target, 'port', '?')}")
    if hasattr(target, "boot_banner"):
        for l in target.boot_banner():
            if l.strip():
                log.rx("SETUP", l.rstrip("\r"))
    results = {}
    fail_frames = []  # shared by R4 and R5 - see run_r4()'s docstring
    results["R0"] = run_r0(target, log, ui, remote=remote)
    caps = results["R0"].get("caps", set())
    results["R1"] = run_simple_block(target, log, caps, ui, "R1", "regs", "regs", TIMEOUT_R1, remote=remote)
    results["R2"] = run_r2(target, log, caps, ui, remote=remote)
    results["R3"] = run_simple_block(target, log, caps, ui, "R3", "test", "test all", TIMEOUT_R3, remote=remote)
    results["R4"] = run_r4(target, log, caps, ui, label, fail_frames, remote=remote)
    results["R5"] = run_r5(target, log, caps, ui, label, r5_ksps, r5_core, r5_pinsel, r5_samc, fail_frames,
                            remote=remote)
    results["R6"] = run_simple_block(target, log, caps, ui, "R6", "route", "route list", TIMEOUT_R6, remote=remote)
    results["R7"] = run_simple_block(target, log, caps, ui, "R7", "status", "status", TIMEOUT_R7, remote=remote)
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
        return dict(which=which, path=None, sha256=None, sha256_status="not_present")
    if not os.path.exists(path):
        raise RuntimeError(f"{which} firmware file not found: {path}")
    digest, status = check_hex_sha256(path)
    if status == "no_sums_file":
        raise RuntimeError(f"no SHA256SUMS.txt next to {path} - board_run/ looks incomplete; "
                            "run 'git pull' again before continuing")
    if status in ("mismatch", "no_entry"):
        reason = ("does not match its entry in SHA256SUMS.txt" if status == "mismatch"
                  else "has no entry in SHA256SUMS.txt")
        ui.say(f"WARNING: {path} {reason} - it may be stale or locally modified.")
        if ui.prompt("continue anyway? [y/N] ").strip().lower() != "y":
            raise RuntimeError(f"aborted: {path} {reason}")
    return dict(which=which, path=os.path.abspath(path), sha256=digest, sha256_status=status)


def find_hex(board_run_dir, prefix):
    """One `<prefix>-*.hex` file in board_run_dir (board-run-task.md's naming:
    A-EV74H48A-<rev>.hex, B-EV74H48A-<rev>.hex). None if there is no such
    file yet (B, until BR.8 lands); a RuntimeError, not a silent pick, if
    there is more than one - an ambiguous board_run/ should stop the run,
    not guess which revision the colleague meant to send back."""
    if not os.path.isdir(board_run_dir):
        return None
    matches = sorted(f for f in os.listdir(board_run_dir)
                      if f.startswith(prefix + "-") and f.endswith(".hex"))
    if not matches:
        return None
    if len(matches) > 1:
        raise RuntimeError(f"more than one {prefix}-*.hex in {board_run_dir}: "
                            f"{', '.join(matches)} - remove the stale one(s)")
    return os.path.join(board_run_dir, matches[0])


def load_checklist(readme_path):
    """The hardware set-up checklist, parsed from board_run/README.md's
    marked section (between CHECKLIST_BEGIN/CHECKLIST_END) rather than
    duplicated here - see that file's own comment. A markdown "- " bullet
    starts a new item; an unmarked, non-blank line continues the previous
    one (README.md wraps long bullets across lines). Returns [] if the file
    or the markers are missing - the caller warns rather than aborting the
    run over a documentation glitch."""
    if not os.path.exists(readme_path):
        return []
    with open(readme_path, encoding="utf-8") as f:
        text = f.read()
    if CHECKLIST_BEGIN not in text or CHECKLIST_END not in text:
        return []
    body = text.split(CHECKLIST_BEGIN, 1)[1].split(CHECKLIST_END, 1)[0]
    if "-->" in body:
        body = body.split("-->", 1)[1]  # drop the opening tag's own comment text
    body = body.rsplit("<!--", 1)[0]    # drop the closing tag's leading "<!--"
    items = []
    current = None
    for raw in body.splitlines():
        s = raw.strip()
        if not s:
            continue
        if s.startswith("- "):
            if current is not None:
                items.append(current)
            current = s[2:].strip()
        elif current is not None:
            current += " " + s
    if current is not None:
        items.append(current)
    return items


def get_git_info(repo_root, run=subprocess.run):
    """git rev-parse HEAD and git status --porcelain from repo_root, for
    session.json (board-run-task.md section 4.1a / BR.4: "the runner also
    checks that the working tree is at a committed revision ... and warns -
    does not stop - if the tree is dirty"). Never raises: git missing, a
    non-zero exit, or any other failure is recorded in info["error"] and
    left for the caller to warn about, since a board run must not depend on
    git being installed at the colleague's site. `run` is injectable so
    --selftest can exercise "git missing" and "dirty tree" without a real
    git call."""
    info = dict(head=None, dirty=None, porcelain=None, error=None)
    try:
        r = run(["git", "-C", repo_root, "rev-parse", "HEAD"],
                capture_output=True, text=True, timeout=10)
        if r.returncode == 0:
            info["head"] = r.stdout.strip()
        else:
            info["error"] = f"git rev-parse HEAD: {(r.stderr or '').strip() or r.returncode}"
    except Exception as e:
        info["error"] = f"git rev-parse HEAD: {e}"
    try:
        r = run(["git", "-C", repo_root, "status", "--porcelain"],
                capture_output=True, text=True, timeout=10)
        if r.returncode == 0:
            info["porcelain"] = r.stdout
            info["dirty"] = bool(r.stdout.strip())
        elif info["error"] is None:
            info["error"] = f"git status --porcelain: {(r.stderr or '').strip() or r.returncode}"
    except Exception as e:
        if info["error"] is None:
            info["error"] = f"git status --porcelain: {e}"
    return info


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
# --remote: the colleague's board through tools/remote.py's RemoteBench
# (bench_client's `flash`/`tunnel` requests) instead of a local COM port.
# See the module docstring's "--remote" paragraph for the sequencing this
# section drives; RemoteTarget is the one piece the R0..R7 core (above)
# needs to know nothing new about a remote session - it only ever sees the
# same sync/cmd/grab/close/boot_banner interface ReplayTarget already gave
# it for --selftest.
# ---------------------------------------------------------------------------
_HEX_REV_RE = re.compile(r"-([0-9A-Fa-f]{6,40})\.hex$")
_BANNER_REV_RE = re.compile(r"\bgit ([0-9A-Fa-f]{6,40})")


def hex_expected_rev(path):
    """The revision board_run/'s own naming convention
    ("<A|B>-<board>-<rev>.hex") carries in a hex file's name - None if the
    name does not end that way (a --hex-a/--hex-b override with a
    different naming scheme skips the check rather than false-alarming on
    it)."""
    m = _HEX_REV_RE.search(os.path.basename(path))
    return m.group(1) if m else None


def extract_banner_rev(text):
    """The revision a boot banner's "... git <rev>[+local changes] (...)"
    carries (board.h's BUILD_ID) - None if the text has no such line at
    all (a flash that produced no banner in the `--after` window)."""
    m = _BANNER_REV_RE.search(text or "")
    return m.group(1) if m else None


class RemoteTarget:
    """protocol.Target over a bench_client tunnel (tools/remote.py's
    RemoteBench), with the one addition run_session()'s R0..R7 core
    already knows how to use: boot_banner(). The first call (run_session()'s
    own "SETUP" banner, read right after connecting) just returns the
    banner flash_and_open() already captured while programming the board -
    no second flash. Every later call is a genuine mid-run recovery
    (handle_timeout(), remote=True): close the tunnel, re-flash the SAME
    hex file (which resets the board exactly as pressing RESET would),
    reopen the tunnel, open a fresh protocol.Target on it, and hand back
    its boot banner - mirroring ReplayTarget.boot_banner()'s own
    first-call-is-free/later-calls-are-a-reset pattern for the local
    --selftest, one level up (a real re-flash instead of a canned string)."""

    def __init__(self, bench, hex_path, url, initial_banner_text=""):
        self.bench = bench
        self.hex_path = hex_path
        self.port = url
        self.target = protocol.Target(url)
        self._initial_banner = initial_banner_text.splitlines()
        self._used_initial = False

    def cmd(self, line, timeout=5.0):
        return self.target.cmd(line, timeout=timeout)

    def grab(self, timeout=10.0):
        return self.target.grab(timeout=timeout)

    def sync(self, timeout=20.0):
        return self.target.sync(timeout=timeout)

    def close(self):
        self.target.close()
        self.bench.close_tunnel()

    def boot_banner(self):
        if not self._used_initial:
            self._used_initial = True
            return self._initial_banner
        self.bench.close_tunnel()
        code, banner_text = self.bench.flash(self.hex_path, after=5)
        if code != 0:
            # Nothing else in this module raises out of boot_banner() /
            # read_reset_banner() - handle_timeout() would have no block
            # left to run against. A re-flash failing mid-run is not a
            # timeout any more, it is a new, worse problem, so it is
            # allowed to stop the whole thing rather than be swallowed.
            raise RuntimeError(f"remote re-flash of {self.hex_path} failed while recovering "
                                f"from a timeout: {banner_text}")
        url = self.bench.open_tunnel()
        self.port = url
        self.target = protocol.Target(url)
        return banner_text.splitlines()


def flash_and_open(bench, hex_path, which, ui, after=5):
    """One "program, check the banner's revision, open the tunnel" step -
    used for both A and B in perform_full_run_remote(). Raises RuntimeError
    with a message meant to be read as-is (perform_full_run_remote() prints
    it and stops) on a flash failure or a revision that does not match the
    hex file's own name; returns (RemoteTarget, flash_info) on success,
    flash_info going straight into session["hex"]'s entry for this file."""
    expected = hex_expected_rev(hex_path)
    ui.say(f"flashing {which} firmware ({hex_path}) via the remote bench ...")
    code, text = bench.flash(hex_path, after=after)
    if code != 0:
        raise RuntimeError(f"remote flash of {which} firmware ({hex_path}) failed: {text}")
    got = extract_banner_rev(text)
    if expected and (got or "").lower() != expected.lower():
        raise RuntimeError(
            f"remote flash of {which} firmware: banner revision mismatch - expected "
            f"{expected}, got {got or 'none'} in the boot banner. Wrong hex flashed, the "
            "board did not reboot, or the console was not reachable in the --after window.")
    ui.say(f"{which}: banner revision {got or '(none found)'} " +
           ("confirmed" if expected else "(no expected revision to check - unusual hex name)"))
    url = bench.open_tunnel()
    target = RemoteTarget(bench, hex_path, url, initial_banner_text=text)
    return target, dict(flash_exit_code=code, banner=text)


def perform_full_run_remote(bench_client, ui, out_dir, hex_a=None, hex_b=None,
                             r5_ksps=R5_DEFAULT_KSPS, r5_core=R5_DEFAULT_CORE,
                             r5_pinsel=R5_DEFAULT_PINSEL, r5_samc=R5_DEFAULT_SAMC,
                             board_run_dir=BOARD_RUN_DIR, readme_path=README_PATH,
                             repo_root=REPO_ROOT, git_run=subprocess.run, yes=False,
                             remote_bench_cls=remote.RemoteBench, bench_env=None):
    """perform_full_run()'s remote twin: no COM port, no "program the
    firmware, then press ENTER" prompts - RemoteBench.flash() does the
    programming, its banner is checked against the hex file's own name
    before anything is sent to it, and each firmware gets its own tunnel
    (opened after that firmware is confirmed, closed before the next
    flash - the agent refuses `flash` while one is open). The hardware
    checklist prompt stays: the lead confirms it on the colleague's behalf,
    since there is still a real board with real wiring at the other end of
    the tunnel - `yes=True` (--yes) answers it without asking, for a
    non-interactive lead session. bench_env lets --selftest hand the fake
    bench_client its FAKE_BENCH_* control variables without touching this
    process's own environment."""
    if hex_a is None:
        hex_a = find_hex(board_run_dir, "A")
    if hex_a is None:
        raise RuntimeError(f"no A-*.hex found in {board_run_dir} - 'git pull' again, or check "
                            f"{os.path.join(board_run_dir, 'SHA256SUMS.txt')} for what should be there")
    if hex_b is None:
        hex_b = find_hex(board_run_dir, "B")

    checklist = load_checklist(readme_path)
    if checklist:
        ui.say("Hardware set-up (remote - confirm this on the colleague's behalf):")
        for item in checklist:
            ui.say(f"  - {item}")
    else:
        ui.say(f"WARNING: could not read the hardware checklist from {readme_path} - "
               "check it by hand before continuing.")
    if yes:
        ui.say("hardware set-up: --yes given, not asking for confirmation")
    else:
        ui.prompt("hardware set-up confirmed, press ENTER: ")

    git_info = get_git_info(repo_root, run=git_run)
    if git_info["error"]:
        ui.say(f"WARNING: could not read git status ({git_info['error']}) - "
               "session.json will not record a revision.")
    elif git_info["dirty"]:
        ui.say("WARNING: the working tree is not clean (git status --porcelain is non-empty) - "
               "recorded in session.json, but a local edit changes what actually runs.")

    session = dict(runner_version=RUNNER_VERSION, transport="remote", bench_client=bench_client,
                    pc_time=time.strftime("%Y-%m-%d %H:%M:%S"), python=platform.python_version(),
                    git=git_info, hex=[])

    bench = remote_bench_cls(bench_client=bench_client, env=bench_env)
    with bench:
        info_a = prepare_hex(hex_a, "OLD", ui)
        target_a, flash_info_a = flash_and_open(bench, hex_a, "OLD", ui)
        info_a.update(flash_info_a)
        session["hex"].append(info_a)
        try:
            log_a, results_a, frames_a = run_session(target_a, ui, "A", r5_ksps, r5_core, r5_pinsel,
                                                       r5_samc, remote=True)
        finally:
            target_a.close()

        log_b = None
        frames_b = []
        if hex_b is not None:
            info_b = prepare_hex(hex_b, "NEW", ui)
            target_b, flash_info_b = flash_and_open(bench, hex_b, "NEW", ui)
            info_b.update(flash_info_b)
            session["hex"].append(info_b)
            try:
                log_b, results_b, frames_b = run_session(target_b, ui, "B", r5_ksps, r5_core, r5_pinsel,
                                                           r5_samc, remote=True)
            finally:
                target_b.close()
        else:
            ui.say("B firmware not available yet (added in BR.8, board-run-task.md section BR) "
                   "- running A only.")
            session["hex"].append(dict(which="NEW", path=None, sha256=None, sha256_status="not_present"))

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
# Output: summary.txt, session.json, the zip
#
# summary.txt's content is tools/eval_board.py's build_summary_text() (BR.2)
# - it parses the very log lines this module just wrote, exactly as it
# would parse them again later from the zip, so a live run's summary.txt
# and eval_board.py's own after-the-fact report can never disagree; this
# module no longer has a summary-building function of its own.
# ---------------------------------------------------------------------------
def write_zip(out_dir, board, session, log_a, log_b, frames_a, frames_b):
    """log_b is None while there is no B firmware yet (BR.4: only A is
    committed in board_run/, B follows in BR.8) - the zip then carries no
    B.log, and summary.txt comes from eval_board.build_summary_text_single()
    (A alone) rather than the normal A/B comparison."""
    ts = time.strftime("%Y%m%d-%H%M%S")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"run-{ts}-{board}.zip")
    if log_b is not None:
        summary = eval_board.build_summary_text(log_a.lines, log_b.lines, eval_board.load_expected())
    else:
        summary = eval_board.build_summary_text_single(
            log_a.lines, eval_board.load_expected(), label="A",
            not_run_note="firmware not available yet (added in BR.8)")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("A.log", log_a.text())
        if log_b is not None:
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
                      r5_pinsel=R5_DEFAULT_PINSEL, r5_samc=R5_DEFAULT_SAMC,
                      board_run_dir=BOARD_RUN_DIR, readme_path=README_PATH,
                      repo_root=REPO_ROOT, git_run=subprocess.run, open_target_fn=None):
    """hex_a/hex_b, when not given explicitly, are found in board_run_dir
    itself (BR.4: the colleague never passes a file name - board_run.py
    finds it). hex_b is None until BR.8 adds a B-*.hex file; run_b is then
    skipped outright, not attempted against a file that does not exist -
    session["hex"] still gets a "NEW" entry so the zip always shows two
    slots, the second marked "not_present". open_target_fn lets --selftest
    inject a stand-in target instead of opening a real serial port; the
    default opens `port` exactly as before."""
    if open_target_fn is None:
        open_target_fn = lambda: open_target(port, ui)  # noqa: E731

    if hex_a is None:
        hex_a = find_hex(board_run_dir, "A")
    if hex_a is None:
        raise RuntimeError(f"no A-*.hex found in {board_run_dir} - 'git pull' again, or check "
                            f"{os.path.join(board_run_dir, 'SHA256SUMS.txt')} for what should be there")
    if hex_b is None:
        hex_b = find_hex(board_run_dir, "B")

    checklist = load_checklist(readme_path)
    if checklist:
        ui.say("Hardware set-up - confirm every item, then press ENTER to continue:")
        for item in checklist:
            ui.say(f"  - {item}")
    else:
        ui.say(f"WARNING: could not read the hardware checklist from {readme_path} - "
               "check it by hand before continuing.")
    ui.prompt("hardware set-up confirmed, press ENTER: ")

    git_info = get_git_info(repo_root, run=git_run)
    if git_info["error"]:
        ui.say(f"WARNING: could not read git status ({git_info['error']}) - "
               "session.json will not record a revision.")
    elif git_info["dirty"]:
        ui.say("WARNING: the working tree is not clean (git status --porcelain is non-empty) - "
               "recorded in session.json, but a local edit changes what actually runs.")

    try:
        import serial
        pyserial_version = serial.__version__
    except ImportError:
        pyserial_version = None

    session = dict(runner_version=RUNNER_VERSION, port=port,
                    pc_time=time.strftime("%Y-%m-%d %H:%M:%S"),
                    python=platform.python_version(), pyserial=pyserial_version,
                    git=git_info, hex=[])

    target = open_target_fn()
    try:
        info_a = prepare_hex(hex_a, "OLD", ui)
        session["hex"].append(info_a)
        ui.prompt(f"program the OLD firmware ({info_a['path']}), then press ENTER: ")
        log_a, results_a, frames_a = run_session(target, ui, "A", r5_ksps, r5_core, r5_pinsel, r5_samc)

        log_b = None
        frames_b = []
        if hex_b is not None:
            info_b = prepare_hex(hex_b, "NEW", ui)
            session["hex"].append(info_b)
            ui.prompt(f"program the NEW firmware ({info_b['path']}), then press ENTER: ")
            if hasattr(target, "sync"):
                target.sync(timeout=TIMEOUT_SYNC)  # reprogramming rebooted the board
            log_b, results_b, frames_b = run_session(target, ui, "B", r5_ksps, r5_core, r5_pinsel, r5_samc)
        else:
            ui.say("B firmware not available yet (added in BR.8, board-run-task.md section BR) "
                   "- running A only.")
            session["hex"].append(dict(which="NEW", path=None, sha256=None, sha256_status="not_present"))
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
                lines.append("  route list - the active route(s) and the resource table")
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
            # The reply format of P11.5 (5ac4dc1, tests/smoke/expected.log):
            # R6 runs after R5's "stream off", so no route is active.
            return True, ["route: none - no route active",
                          "dma_used: 0", "dma_total: 8", "sccp_used: 0", "sccp_total: 8",
                          "dac_outputs_used: 0", "dac_outputs_total: 2",
                          "uref_used: 0", "uref_total: 1",
                          "ram_used: 0", "ram_budget: 57344"]
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

    def _grab_wire(self):
        """The three raw wire pieces (header text line, payload bytes, tail
        - prompt/CRC line/ACK-or-NAK) one 'stream grab' cycle would put on
        the console, exactly as grab() builds them, split out so a fixture
        that plays this same replay over a REAL socket (a bench_client
        stand-in, tests/host/fake_bench_client.py) can send the actual
        bytes a real Target reads, instead of the (ok, samples, meta) tuple
        grab() decodes them into for the in-process --selftest here."""
        if not self.chain_on:
            header = "GRAB n=0 from=0 ksps=0 ov=0 late=0 missed=0 halves=0 xfer=0 slp=0 dachz=0\r\n"
            crc = protocol.crc16_ccitt_false(b"")
            tail = f"\r\nCRC {crc:04X}\r\n> ".encode("ascii") + protocol.NAK
            return header, b"", tail
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
        return header, payload, tail

    def grab(self, timeout=10.0):
        return protocol.parse_grab_frame(*self._grab_wire())


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
    # BR.6 (27.09.2026): B's real "status" always carries these extra
    # fields now (src/cli/cli.c's cmd_status_fn()); A, the fixed pre-BR.6
    # baseline hex, never will - eval_board.py's own selftest (BR.2) is
    # where the field-by-field judgement (the stack rule, the guard-word
    # check) is exercised, this only proves the runner logs a status reply
    # that carries them through untouched, end to end.
    target_b = ReplayTarget("B", has_route=True, nak_once={"regs"}, extra_status_fields={
        "stack_size": "52264", "stack_used": "5192", "stack_hwm_addr": "34892",
        "stack_free_pct": "90",
        "buf_addr": "16720", "buf_align_mod4": "0", "buf_len": "4096",
        "buf_guard_ok": "1", "boot_stage": "9", "trap_seen": "0", "trap_vec": "0",
        "chain_mark": "0"})

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
    check("BR.6: B's status carries the new fields, A's does not",
          any("stack_free_pct: 90" in l for l in log_b.lines)
          and not any("stack_free_pct" in l for l in log_a.lines))

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

    # -----------------------------------------------------------------
    # BR.4: hex discovery + SHA-256 checking (find_hex/check_hex_sha256/
    # prepare_hex) - matching, modified, missing, and an ambiguous board_run/.
    # -----------------------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        a_path = os.path.join(tmp, "A-EV74H48A-deadbee.hex")
        with open(a_path, "wb") as f:
            f.write(b":10000000FF\n")
        digest = compute_sha256(a_path)
        with open(os.path.join(tmp, "SHA256SUMS.txt"), "w") as f:
            f.write(f"{digest} *A-EV74H48A-deadbee.hex\n")

        check("find_hex(): finds the one A-*.hex", find_hex(tmp, "A") == a_path)
        check("find_hex(): no B-*.hex yet -> None", find_hex(tmp, "B") is None)
        _, status = check_hex_sha256(a_path)
        check("check_hex_sha256(): a matching file", status == "match")
        info = prepare_hex(a_path, "OLD", StubUI())
        check("prepare_hex(): a matching file needs no confirmation", info["sha256_status"] == "match")

        with open(a_path, "ab") as f:
            f.write(b"\n")  # modified after SHA256SUMS.txt was written
        _, status2 = check_hex_sha256(a_path)
        check("check_hex_sha256(): a modified file is a mismatch", status2 == "mismatch")
        try:
            prepare_hex(a_path, "OLD", StubUI(answers=["n"]))
            check("prepare_hex(): a modified file, declined -> aborted", False)
        except RuntimeError:
            check("prepare_hex(): a modified file, declined -> aborted", True)
        info_yes = prepare_hex(a_path, "OLD", StubUI(answers=["y"]))
        check("prepare_hex(): a modified file, accepted -> proceeds",
              info_yes["sha256_status"] == "mismatch")

        try:
            prepare_hex(os.path.join(tmp, "A-does-not-exist.hex"), "OLD", StubUI())
            check("prepare_hex(): a missing file raises", False)
        except RuntimeError:
            check("prepare_hex(): a missing file raises", True)

        second = os.path.join(tmp, "A-EV74H48A-other12.hex")
        with open(second, "wb") as f:
            f.write(b":10000000FF\n")
        try:
            find_hex(tmp, "A")
            check("find_hex(): two A-*.hex files raise", False)
        except RuntimeError:
            check("find_hex(): two A-*.hex files raise", True)

    # -----------------------------------------------------------------
    # BR.4: git status (get_git_info) - clean, dirty and git-unavailable,
    # with the git call injected so this needs no real git and no real repo.
    # -----------------------------------------------------------------
    def fake_git_clean(cmd, **kw):
        return SimpleNamespace(returncode=0, stderr="",
                                stdout="deadbeefcafefeed\n" if "rev-parse" in cmd else "")

    def fake_git_dirty(cmd, **kw):
        return SimpleNamespace(returncode=0, stderr="",
                                stdout="deadbeefcafefeed\n" if "rev-parse" in cmd
                                else " M tools/board_run.py\n")

    def fake_git_missing(cmd, **kw):
        raise FileNotFoundError("git not found")

    info_clean = get_git_info("ignored", run=fake_git_clean)
    check("get_git_info(): a clean tree -> dirty=False, head recorded",
          info_clean["dirty"] is False and info_clean["head"] == "deadbeefcafefeed"
          and info_clean["error"] is None)
    info_dirty = get_git_info("ignored", run=fake_git_dirty)
    check("get_git_info(): a dirty tree -> dirty=True, porcelain recorded",
          info_dirty["dirty"] is True and "board_run.py" in (info_dirty["porcelain"] or ""))
    info_missing = get_git_info("ignored", run=fake_git_missing)
    check("get_git_info(): git unavailable -> recorded as an error, never raised",
          info_missing["error"] is not None and info_missing["head"] is None)

    # -----------------------------------------------------------------
    # BR.4: the hardware checklist, parsed from a README.md's marked
    # section rather than duplicated in this script.
    # -----------------------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        readme = os.path.join(tmp, "README.md")
        with open(readme, "w", encoding="utf-8") as f:
            f.write("# test\n\n<!-- BOARD_RUN_CHECKLIST:BEGIN\ncomment text\n-->\n"
                     "- item one\n- item two, wrapped\n  across two lines\n"
                     "<!-- BOARD_RUN_CHECKLIST:END -->\n")
        items = load_checklist(readme)
        check("load_checklist(): parses two items, joins the wrapped continuation",
              items == ["item one", "item two, wrapped across two lines"])
        check("load_checklist(): a missing file returns []",
              load_checklist(os.path.join(tmp, "nope.md")) == [])
    check("load_checklist(): the real board_run/README.md has a non-empty checklist",
          len(load_checklist(README_PATH)) >= 3)

    # -----------------------------------------------------------------
    # BR.4: perform_full_run() end to end, through the same dependency
    # injection points (open_target_fn/git_run) that make the whole thing
    # testable without a board or a git repository - first with only an
    # A-*.hex present (B not built yet, the state board_run/ is actually in
    # today), then with both.
    # -----------------------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        board_run_dir = os.path.join(tmp, "board_run")
        os.makedirs(board_run_dir)
        a_path = os.path.join(board_run_dir, "A-EV74H48A-deadbee.hex")
        with open(a_path, "wb") as f:
            f.write(b":10000000FF\n")
        with open(os.path.join(board_run_dir, "SHA256SUMS.txt"), "w") as f:
            f.write(f"{compute_sha256(a_path)} *A-EV74H48A-deadbee.hex\n")
        readme_path = os.path.join(board_run_dir, "README.md")
        with open(readme_path, "w", encoding="utf-8") as f:
            f.write("<!-- BOARD_RUN_CHECKLIST:BEGIN\nc\n-->\n"
                     "- confirm the board is plugged in\n<!-- BOARD_RUN_CHECKLIST:END -->\n")

        ui_a_only = StubUI(answers=["", "", "n"])  # checklist ENTER, program-A ENTER, R5 signal "n"
        out_dir = os.path.join(tmp, "out")
        rc = perform_full_run("COMX", ui_a_only, out_dir, board_run_dir=board_run_dir,
                               readme_path=readme_path, repo_root=tmp, git_run=fake_git_clean,
                               open_target_fn=lambda: ReplayTarget("A", has_route=False))
        check("perform_full_run(): A-only run (no B-*.hex yet) reports PASS", rc == 0)
        check("perform_full_run(): prints the checklist item from README.md",
              any("confirm the board is plugged in" in s for s in ui_a_only.said))
        zips = [f for f in os.listdir(out_dir) if f.endswith(".zip")]
        check("perform_full_run(): wrote exactly one zip", len(zips) == 1)
        with zipfile.ZipFile(os.path.join(out_dir, zips[0])) as z:
            names = z.namelist()
            session_back = json.loads(z.read("session.json"))
        check("perform_full_run(): A-only zip has A.log but no B.log",
              "A.log" in names and "B.log" not in names)
        check("perform_full_run(): session.json's NEW (B) hex slot is not_present",
              session_back["hex"][1]["sha256_status"] == "not_present")
        check("perform_full_run(): session.json carries the (injected) git HEAD",
              session_back["git"]["head"] == "deadbeefcafefeed")

        b_path = os.path.join(board_run_dir, "B-EV74H48A-cafefee.hex")
        with open(b_path, "wb") as f:
            f.write(b":10000000EE\n")
        with open(os.path.join(board_run_dir, "SHA256SUMS.txt"), "a") as f:
            f.write(f"{compute_sha256(b_path)} *B-EV74H48A-cafefee.hex\n")
        ui_ab = StubUI(answers=["", "", "", "n"])  # checklist, program-A, program-B, R5 signal
        out_dir2 = os.path.join(tmp, "out2")
        rc2 = perform_full_run("COMX", ui_ab, out_dir2, board_run_dir=board_run_dir,
                                readme_path=readme_path, repo_root=tmp, git_run=fake_git_dirty,
                                open_target_fn=lambda: ReplayTarget("A", has_route=True))
        check("perform_full_run(): with both A and B present, reports PASS", rc2 == 0)
        zips2 = [f for f in os.listdir(out_dir2) if f.endswith(".zip")]
        with zipfile.ZipFile(os.path.join(out_dir2, zips2[0])) as z:
            names2 = z.namelist()
        check("perform_full_run(): with B present, the zip has both A.log and B.log",
              "A.log" in names2 and "B.log" in names2)
        check("perform_full_run(): a dirty tree is reported to the operator, not fatal",
              any("not clean" in s for s in ui_ab.said))

    # -----------------------------------------------------------------
    # --remote: perform_full_run_remote() against tests/host/fake_bench_client.py
    # - never the real bench_client.py or a real relay/agent (that fixture's
    # own docstring). One FAKE_BENCH_STATE_DIR per scenario below: it is how
    # the fixture's separate flash/tunnel subprocesses agree on which
    # firmware is "flashed", so reusing one across scenarios would leak a
    # hang_done marker or a wrong-revision flash from one into the next.
    # -----------------------------------------------------------------
    fake_bench_client = os.path.join(REPO_ROOT, "tests", "host", "fake_bench_client.py")

    def make_remote_board_run(tmp):
        board_run_dir = os.path.join(tmp, "board_run")
        os.makedirs(board_run_dir)
        a_path = os.path.join(board_run_dir, "A-EV74H48A-deadbee.hex")
        b_path = os.path.join(board_run_dir, "B-EV74H48A-cafefee.hex")
        with open(a_path, "wb") as f:
            f.write(b":10000000FF\n")
        with open(b_path, "wb") as f:
            f.write(b":10000000EE\n")
        with open(os.path.join(board_run_dir, "SHA256SUMS.txt"), "w") as f:
            f.write(f"{compute_sha256(a_path)} *A-EV74H48A-deadbee.hex\n")
            f.write(f"{compute_sha256(b_path)} *B-EV74H48A-cafefee.hex\n")
        readme_path = os.path.join(board_run_dir, "README.md")
        with open(readme_path, "w", encoding="utf-8") as f:
            f.write("<!-- BOARD_RUN_CHECKLIST:BEGIN\nc\n-->\n"
                     "- confirm the board is plugged in\n<!-- BOARD_RUN_CHECKLIST:END -->\n")
        return board_run_dir, readme_path, a_path, b_path

    # ---- full A+B remote session (happy path) ----
    with tempfile.TemporaryDirectory() as tmp:
        board_run_dir, readme_path, a_path, b_path = make_remote_board_run(tmp)
        ui_remote = StubUI(answers=["", "n"])  # checklist ENTER, R5 signal "n"
        out_dir = os.path.join(tmp, "out")
        rc = perform_full_run_remote(
            fake_bench_client, ui_remote, out_dir, hex_a=a_path, hex_b=b_path,
            board_run_dir=board_run_dir, readme_path=readme_path, repo_root=tmp,
            git_run=fake_git_clean,
            bench_env=dict(os.environ, FAKE_BENCH_STATE_DIR=os.path.join(tmp, "state")))
        check("perform_full_run_remote(): full A+B session reports PASS", rc == 0)
        zips = [f for f in os.listdir(out_dir) if f.endswith(".zip")]
        with zipfile.ZipFile(os.path.join(out_dir, zips[0])) as z:
            names = z.namelist()
            session_back = json.loads(z.read("session.json"))
        check("perform_full_run_remote(): zip has both A.log and B.log",
              "A.log" in names and "B.log" in names)
        check("perform_full_run_remote(): session.json records transport and bench_client path",
              session_back["transport"] == "remote" and session_back["bench_client"] == fake_bench_client)
        check("perform_full_run_remote(): each hex entry carries its flash exit code and banner",
              all(h.get("flash_exit_code") == 0 and "git" in (h.get("banner") or "")
                  for h in session_back["hex"]))

    # ---- flash failure (exit 9) stopping cleanly ----
    with tempfile.TemporaryDirectory() as tmp:
        board_run_dir, readme_path, a_path, b_path = make_remote_board_run(tmp)
        try:
            perform_full_run_remote(
                fake_bench_client, StubUI(answers=["", "n"]), os.path.join(tmp, "out"),
                hex_a=a_path, hex_b=b_path, board_run_dir=board_run_dir, readme_path=readme_path,
                repo_root=tmp, git_run=fake_git_clean,
                bench_env=dict(os.environ, FAKE_BENCH_STATE_DIR=os.path.join(tmp, "state"),
                                FAKE_BENCH_FAIL9="1"))
            check("perform_full_run_remote(): a flash failure (exit 9) stops cleanly", False)
        except RuntimeError as e:
            check("perform_full_run_remote(): a flash failure (exit 9) stops cleanly",
                  "exit 9" in str(e) or "programmer" in str(e).lower())

    # ---- banner-revision mismatch stopping cleanly ----
    with tempfile.TemporaryDirectory() as tmp:
        board_run_dir, readme_path, a_path, b_path = make_remote_board_run(tmp)
        try:
            perform_full_run_remote(
                fake_bench_client, StubUI(answers=["", "n"]), os.path.join(tmp, "out"),
                hex_a=a_path, hex_b=b_path, board_run_dir=board_run_dir, readme_path=readme_path,
                repo_root=tmp, git_run=fake_git_clean,
                bench_env=dict(os.environ, FAKE_BENCH_STATE_DIR=os.path.join(tmp, "state"),
                                FAKE_BENCH_WRONG_REV="1"))
            check("perform_full_run_remote(): a banner revision mismatch stops cleanly", False)
        except RuntimeError as e:
            check("perform_full_run_remote(): a banner revision mismatch stops cleanly",
                  "revision mismatch" in str(e))

    # ---- a mid-run timeout, recovered by closing the tunnel and re-flashing ----
    with tempfile.TemporaryDirectory() as tmp:
        board_run_dir, readme_path, a_path, b_path = make_remote_board_run(tmp)
        env = dict(os.environ, FAKE_BENCH_STATE_DIR=os.path.join(tmp, "state"),
                   FAKE_BENCH_TIMEOUT_BLOCK="regs")
        bench = remote.RemoteBench(bench_client=fake_bench_client, env=env)
        with bench:
            target, _ = flash_and_open(bench, a_path, "OLD", StubUI())
            try:
                log_r, results_r, _ = run_session(target, StubUI(), "A", remote=True)
            finally:
                target.close()
        check("--remote timeout recovery: R1 (regs) times out once",
              results_r["R1"]["verdict"] == "timeout")
        check("--remote timeout recovery: logged closing the tunnel and re-flashing",
              any("reset: remote re-flash" in l for l in log_r.lines))
        check("--remote timeout recovery: later blocks still ran, against the reopened tunnel",
              results_r["R2"]["verdict"] == "ok" and results_r["R7"]["verdict"] == "ok")

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
    ap.add_argument("--hex-a", help="the OLD firmware's hex file (default: the one A-*.hex found in "
                                     "board_run/, checked against board_run/SHA256SUMS.txt)")
    ap.add_argument("--hex-b", help="the NEW firmware's hex file (default: the one B-*.hex in "
                                     "board_run/, if any - not present until BR.8, in which case A "
                                     "runs alone)")
    ap.add_argument("--out-dir", default=".", help="where to write run-*.zip (default: .)")
    ap.add_argument("--r5-ksps", type=int, default=R5_DEFAULT_KSPS)
    ap.add_argument("--r5-core", type=int, default=R5_DEFAULT_CORE)
    ap.add_argument("--r5-pinsel", type=int, default=R5_DEFAULT_PINSEL)
    ap.add_argument("--r5-samc", type=int, default=R5_DEFAULT_SAMC)
    ap.add_argument("--remote", action="store_true",
                     help="the colleague's board through bench_client's flash/tunnel requests "
                          "(CLAUDE.md's 'Remote board access') instead of a local COM port - "
                          "no port argument, no 'program the firmware, press ENTER' prompts")
    ap.add_argument("--bench-client",
                     help="path to bench_client.py (default: $BENCH_CLIENT, else "
                          "C:\\work\\Claas\\Relay\\bench_client.py) - only with --remote")
    ap.add_argument("--yes", action="store_true",
                     help="answer the hardware set-up checklist prompt without asking "
                          "(--remote only; the R5 signal-generator question still asks)")
    a = ap.parse_args(argv)

    if a.selftest:
        return selftest()
    if a.list:
        print_port_list()
        return 0

    ui = ConsoleUI()
    if a.remote:
        bench_client = a.bench_client or os.environ.get("BENCH_CLIENT", remote.DEFAULT_BENCH_CLIENT)
        return perform_full_run_remote(bench_client, ui, a.out_dir, a.hex_a, a.hex_b,
                                        a.r5_ksps, a.r5_core, a.r5_pinsel, a.r5_samc, yes=a.yes)

    port = pick_port(a.port)
    return perform_full_run(port, ui, a.out_dir, a.hex_a, a.hex_b,
                             a.r5_ksps, a.r5_core, a.r5_pinsel, a.r5_samc)


if __name__ == "__main__":
    sys.exit(main())
