#!/usr/bin/env python3
"""tools/eval_board.py - turn one board_run.py session into a list of
deviations.

    python tools/eval_board.py run-20260927-153045-EV74H48A.zip
    python tools/eval_board.py A.log --b-log B.log
    python tools/eval_board.py --selftest

Input: the zip tools/board_run.py (BR.1) writes - A.log and B.log inside
it - or the two log files directly. This module parses ONLY that log
format (documented in board_run.py's own module docstring:
`<ms> <DIR> <BLOCK> <text>`, DIR in TX/RX/EV); it never imports board_run.py
at module level (only lazily, inside selftest(), to build synthetic logs
with the real ReplayTarget/run_session - see selftest()'s own comment for
why that is safe despite board_run.py importing this module in turn).
"chain all" (R2) is judged by IMPORTING tools/eval_chain.py's own
parse()/tri_eval()/rejudge() rather than a second implementation of the
"@S<stage>.<n> ... -> VERDICT" line format or the triangle evaluator.

What this does (board-run-task.md section 6 / the BR.2 card):
  - Refuses a log whose first line names a RUNNER_VERSION this module does
    not know (KNOWN_RUNNER_VERSIONS below) rather than misreading it.
  - Completeness: every block in BLOCK_ORDER present (not "missing", not
    "timeout") in both A and B; for R2 specifically, "chain all" reaching
    its own "@END" line (a run that stopped early - a hang, a reset that
    was never asked for - still has an R2 verdict of "ok" if the ACK came
    back, but no @END; both are reported).
  - A/B diff, one line per difference: every "@S<stage>.<n>" result's
    verdict and every one of its fields (R2, via eval_chain.parse()), every
    "regs" register (R1) and "status" field (R0/R7) by key, the aggregate
    ov/late/missed/error counters per rate (R4's three sub-blocks, R5), and
    a coarse text diff of "test all"'s reply (R3, which is free text, not
    key=value lines).
  - Expectations: tests/board/expected.json, each entry tagged "source":
    "run19" (checked against BOTH A and B - A is close to the run-19
    revision itself, so a run-19 fact not holding in A points at the rig,
    not at N+1) or "source": "prediction" (checked against B only - a
    prediction in board-run-task.md's HARDWARE-LOG entry was explicitly
    "for the next run", i.e. about the NEW firmware). A missed prediction
    is reported with its source and HARDWARE-LOG date, not folded into the
    hard PASS/FAIL overview line the way an A/B diff or a broken run19 fact
    is - see build_summary_text()'s "missed predictions" line.

Output format, one line per deviation (the same shape whether printed to
the console or written into summary.txt):

    <BLOCK> <KIND>: <what> A=<value> B=<value> [expect_source=<run19|
    prediction>@<date>] files=<comma-separated source files the block
    exercises>

  KIND is "ab_diff" (A and B differ), "incomplete" (a block never reached
  a verdict, or R2 never reached @END), or "expectation" (checked against
  tests/board/expected.json rather than between A and B).

One evaluator, not two: tools/board_run.py's own summary.txt used to be
built by a small function inside board_run.py itself (BR.1); since BR.2,
board_run.py imports this module and calls build_summary_text() instead,
so a live run's summary.txt and this module's own after-the-fact report
are produced by the exact same code, not two implementations that could
quietly disagree.
"""
import argparse
import json
import os
import re
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eval_chain  # noqa: E402

KNOWN_RUNNER_VERSIONS = {"1"}

BLOCK_ORDER = ["R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7"]

# A small, static map from block to the source files a deviation there most
# likely comes from - not exhaustive (every driver's own regs_visit() feeds
# R1, for instance), just the files a correction card for that block would
# start reading.
BLOCK_FILES = {
    "R0": ["src/cli/cli.c", "src/diag/diag.c"],
    "R1": ["src/diag/diag.c", "src/port/regs.h", "src/drivers/adc.c",
           "src/drivers/dma.c", "src/drivers/clock.c"],
    "R2": ["src/tests/chaintest.c", "src/tests/chaintest_priv.h", "tools/eval_chain.py"],
    "R3": ["src/tests/bench.c"],
    "R4": ["src/link/gui_link.c", "src/app/acquisition.c", "src/app/capture.c",
           "src/app/pingpong.c", "tools/protocol.py"],
    "R5": ["src/link/gui_link.c", "src/app/acquisition.c", "src/app/capture.c",
           "src/app/pingpong.c", "tools/protocol.py"],
    "R6": ["src/app/routing.c", "src/app/routing.h", "src/cli/cli.c"],
    "R7": ["src/cli/cli.c", "src/diag/diag.c"],
}

DEFAULT_EXPECTED_PATH = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "board", "expected.json"))


class UnknownRunnerVersion(Exception):
    pass


# ---------------------------------------------------------------------------
# Parsing board_run.py's own log format
# ---------------------------------------------------------------------------
_LOG_LINE_RE = re.compile(r"^(\d+) (TX|RX|EV) (\S+) ?(.*)$")
_RUNNER_VERSION_RE = re.compile(r"RUNNER_VERSION=(\S+)")
_BLOCK_END_RE = re.compile(r"^block end (\S+)$")


def parse_log_lines(lines):
    """One dict per line: {ms, dir, block, text}. A line that does not
    match the format (blank, or something outside this tool's control) is
    dropped rather than raising - a log is a diagnostic artifact, not a
    contract the parser should be brittle against."""
    entries = []
    for line in lines:
        m = _LOG_LINE_RE.match(line)
        if not m:
            continue
        ms, direction, block, text = m.groups()
        entries.append(dict(ms=int(ms), dir=direction, block=block, text=text))
    return entries


def extract_runner_version(entries):
    for e in entries:
        if e["block"] == "-" and e["dir"] == "EV":
            m = _RUNNER_VERSION_RE.search(e["text"])
            if m:
                return m.group(1)
    return None


def block_verdict(entries, block):
    for e in entries:
        if e["block"] == block and e["dir"] == "EV":
            m = _BLOCK_END_RE.match(e["text"])
            if m:
                return m.group(1)
    return "missing"


def block_rx_lines(entries, block):
    return [e["text"] for e in entries
            if e["block"] == block and e["dir"] == "RX" and e["text"] not in ("[ACK]", "[NAK]")]


# ---------------------------------------------------------------------------
# "key: value" lines - regs (R1) and status (R0/R7): console_kv()/
# console_kv_hex() (src/cli/cli.c) both print exactly "key: value".
# ---------------------------------------------------------------------------
_KV_RE = re.compile(r"^([^:]+):\s*(\S+)$")


def parse_kv_lines(lines):
    d = {}
    for l in lines:
        m = _KV_RE.match(l.strip())
        if m:
            d[m.group(1).strip()] = m.group(2).strip()
    return d


def diff_kv_block(entries_a, entries_b, block, name):
    kv_a = parse_kv_lines(block_rx_lines(entries_a, block))
    kv_b = parse_kv_lines(block_rx_lines(entries_b, block))
    out = []
    for key in sorted(set(kv_a) | set(kv_b)):
        va, vb = kv_a.get(key), kv_b.get(key)
        if va != vb:
            out.append(dict(block=block, kind="ab_diff", detail=f"{name} '{key}' differs", a=va, b=vb))
    return out


# ---------------------------------------------------------------------------
# "chain all" (R2): eval_chain.parse() reads a FILE, so the block's RX
# lines (board_run.py's log already has them, one per line, ACK/NAK
# stripped) are written to a temp file and handed to it - the same
# parser/tri_eval()/rejudge() the firmware's own chain test is judged
# with, not a second implementation.
# ---------------------------------------------------------------------------
def chain_all_results(rx_lines):
    if not rx_lines:
        return [], False
    fd, path = tempfile.mkstemp(suffix=".log")
    try:
        with os.fdopen(fd, "w", newline="\n") as f:
            for l in rx_lines:
                f.write(l + "\n")
        results, _dumps, _sums, _recipe, _other, ended = eval_chain.parse(path)
    finally:
        os.unlink(path)
    return results, ended


def diff_chain(results_a, results_b):
    by_a = {(r.stage, r.n): r for r in results_a}
    by_b = {(r.stage, r.n): r for r in results_b}
    out = []
    for key in sorted(set(by_a) | set(by_b)):
        tag = f"S{key[0]}.{key[1]}"
        ra, rb = by_a.get(key), by_b.get(key)
        if ra is None:
            out.append(dict(block="R2", kind="ab_diff", detail=f"{tag} only in B", a="-", b=rb.verdict))
            continue
        if rb is None:
            out.append(dict(block="R2", kind="ab_diff", detail=f"{tag} only in A", a=ra.verdict, b="-"))
            continue
        if ra.verdict != rb.verdict:
            out.append(dict(block="R2", kind="ab_diff", detail=f"{tag} verdict differs",
                             a=ra.verdict, b=rb.verdict))
        for field in sorted(set(ra.f) | set(rb.f)):
            va, vb = ra.f.get(field), rb.f.get(field)
            if va != vb:
                out.append(dict(block="R2", kind="ab_diff", detail=f"{tag} field '{field}' differs",
                                 a=va, b=vb))
    return out


# ---------------------------------------------------------------------------
# R4/R5's "stream grab" summary lines - board_run.py's own
# grab_summary_line(): "GRAB n=.. ov=.. late=.. missed=.. slp=.. ksps=.."
# on success, "GRAB n=.. error=.." otherwise. Aggregated per rate (R4's
# "R4.<ksps>" sub-blocks) or per block (R5, one rate only).
# ---------------------------------------------------------------------------
_GRAB_OK_RE = re.compile(r"^GRAB n=(\d+) ov=(\d+) late=(\d+) missed=(\d+) slp=(\d+) ksps=(\d+)$")
_GRAB_ERR_RE = re.compile(r"^GRAB n=(\d+) error=")


def grab_family_counters(entries, prefix):
    """`prefix` "R4" groups every block "R4" or "R4.<ksps>" together (one
    entry per rate); "R5" matches only the exact block "R5" (no sub-rates
    there) since "R5" does not start with "R5." either way this still
    works out to one entry."""
    out = {}
    for e in entries:
        if e["dir"] != "RX":
            continue
        if not (e["block"] == prefix or e["block"].startswith(prefix + ".")):
            continue
        m = _GRAB_OK_RE.match(e["text"])
        if m:
            _n, ov, late, missed, _slp, _ksps = (int(x) for x in m.groups())
            d = out.setdefault(e["block"], dict(grabs=0, ov=0, late=0, missed=0, errors=0))
            d["grabs"] += 1
            d["ov"] += ov
            d["late"] += late
            d["missed"] += missed
            continue
        if _GRAB_ERR_RE.match(e["text"]):
            d = out.setdefault(e["block"], dict(grabs=0, ov=0, late=0, missed=0, errors=0))
            d["grabs"] += 1
            d["errors"] += 1
    return out


def diff_grab_family(entries_a, entries_b, prefix):
    ca = grab_family_counters(entries_a, prefix)
    cb = grab_family_counters(entries_b, prefix)
    block = "R4" if prefix == "R4" else "R5"
    out = []
    for sub in sorted(set(ca) | set(cb)):
        da, db = ca.get(sub), cb.get(sub)
        if da is None:
            out.append(dict(block=block, kind="ab_diff", detail=f"{sub} only in B", a="-", b=db))
            continue
        if db is None:
            out.append(dict(block=block, kind="ab_diff", detail=f"{sub} only in A", a=da, b="-"))
            continue
        for k in ("grabs", "ov", "late", "missed", "errors"):
            if da[k] != db[k]:
                out.append(dict(block=block, kind="ab_diff", detail=f"{sub} '{k}' differs",
                                 a=da[k], b=db[k]))
    return out


# ---------------------------------------------------------------------------
# R3 ("test all"): free text, not key=value lines - a coarse diff.
# ---------------------------------------------------------------------------
def diff_raw_block(entries_a, entries_b, block):
    la = block_rx_lines(entries_a, block)
    lb = block_rx_lines(entries_b, block)
    if la == lb:
        return []
    n = min(len(la), len(lb))
    idx = next((i for i in range(n) if la[i] != lb[i]), n)
    a_sample = la[idx] if idx < len(la) else "(nothing)"
    b_sample = lb[idx] if idx < len(lb) else "(nothing)"
    return [dict(block=block, kind="ab_diff",
                 detail=f"reply differs ({len(la)} vs {len(lb)} lines, first difference at line {idx})",
                 a=a_sample, b=b_sample)]


# ---------------------------------------------------------------------------
# BR.6 stack rule (BR.9 pass criterion): "the stack high-water mark leaves
# at least 25% of the stack unused". Judged in B only - A predates BR.6 and
# its "status" has none of these fields, reported as NOT_AVAILABLE rather
# than compared, the same vocabulary board_run.py uses for R6 on A (no
# 'route' command at all). The ordinary diff_kv_block(..., "R7", "status")
# call already reports every BR.6 field as an "only in B" difference; this
# is the extra check that turns "B has a stack_free_pct" into a pass/fail
# judgement, and separately flags the DMA buffer's guard words.
# ---------------------------------------------------------------------------
def check_stack_criterion(entries_b):
    """Only ever looks at B: A is the fixed pre-BR.6 baseline and will
    never have these fields, which is normal, not a deviation (the plain
    diff_kv_block(..., "R7", "status") call above already reports each
    BR.6 field as an ordinary "only in B" difference; A's side of THIS
    judgement is always "NOT_AVAILABLE", never fetched from A's log). A B
    that does not have the field either (a pre-BR.6 stand-in, or a B build
    that predates BR.6) is not judged - nothing to compare against 25%."""
    kv_b = parse_kv_lines(block_rx_lines(entries_b, "R7"))
    out = []
    pct = kv_b.get("stack_free_pct")
    if pct is not None:
        try:
            pct_val = int(pct)
        except ValueError:
            pct_val = -1
        if pct_val < 25:
            out.append(dict(block="R7", kind="expectation",
                             detail="stack_free_pct below the BR.9 pass criterion (>= 25% unused)",
                             a="NOT_AVAILABLE", b=pct))
    guard = kv_b.get("buf_guard_ok")
    if guard is not None and guard != "1":
        out.append(dict(block="R7", kind="expectation",
                         detail="buf_guard_ok: the guard words behind the DMA buffer are not intact",
                         a="NOT_AVAILABLE", b=guard))
    return out


# ---------------------------------------------------------------------------
# Expectations: tests/board/expected.json
# ---------------------------------------------------------------------------
def load_expected(path=DEFAULT_EXPECTED_PATH):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    return data.get("expectations", [])


def _result_matches(r, stages, match):
    if stages and r.stage not in stages:
        return False
    for k, v in (match or {}).items():
        got = r.f.get(k)
        if v == "*":                  # the field only has to be present
            if got is None:
                return False
            continue
        if isinstance(v, list):
            if got not in v:
                return False
        elif got != v:
            return False
    return True


def check_expectation(entry, results):
    """One expected.json entry against one run's parsed chain-all Result
    list. Returns a list of {field, got, expect} dicts, empty if the
    expectation holds. A stage the ladder never reached (an aborted run, or
    a rate outside this firmware's ladder) matches nothing and is silently
    fine here - a run that stopped early is already reported separately as
    "incomplete"."""
    matched = [r for r in results if _result_matches(r, entry.get("stage"), entry.get("match"))]
    if not matched:
        return []
    check = entry["check"]
    out = []
    if check == "verdict":
        for r in matched:
            if r.verdict != entry["value"]:
                out.append(dict(line=f"S{r.stage}.{r.n}", got=r.verdict, expect=entry["value"]))
    elif check == "field_eq":
        for r in matched:
            for k, v in entry["value"].items():
                got = r.f.get(k)
                if got != v:
                    out.append(dict(line=f"S{r.stage}.{r.n} {k}", got=got, expect=v))
    elif check == "field_max":
        for r in matched:
            got = r.f.get(entry["field"])
            if got is None or got > entry["value"]:
                out.append(dict(line=f"S{r.stage}.{r.n} {entry['field']}", got=got,
                                 expect=f"<= {entry['value']}"))
    elif check == "field_min":
        for r in matched:
            got = r.f.get(entry["field"])
            if got is None or got < entry["value"]:
                out.append(dict(line=f"S{r.stage}.{r.n} {entry['field']}", got=got,
                                 expect=f">= {entry['value']}"))
    elif check == "field_in":
        for r in matched:
            got = r.f.get(entry["field"])
            if got not in entry["value"]:
                out.append(dict(line=f"S{r.stage}.{r.n} {entry['field']}", got=got,
                                 expect=f"in {entry['value']}"))
    return out


def grab_lengths(log_entries, block):
    """The sample counts of every successful grab logged under `block`
    (e.g. "R4.gui"), from board_run.py's grab_summary_line()."""
    out = []
    for e in log_entries:
        if e["dir"] == "RX" and e["block"] == block:
            m = _GRAB_OK_RE.match(e["text"])
            if m:
                out.append(int(m.group(1)))
    return out


def check_grab_n(entry, log_entries):
    """check "grab_n": every successful grab in entry["block"] carries
    entry["value"] samples (29.09.2026, R4.gui after "buf 512")."""
    ns = grab_lengths(log_entries, entry["block"])
    bad = sorted({n for n in ns if n != entry["value"]})
    return [dict(line=f"{entry['block']} grab n", got=bad, expect=entry["value"])] if bad else []


def evaluate_expectations(entries, results_a, results_b, log_a=None, log_b=None):
    out = []
    for e in entries:
        targets = [("B", results_b, log_b)] if e["source"] == "prediction" else             [("A", results_a, log_a), ("B", results_b, log_b)]
        for label, results, log_entries in targets:
            found = (check_grab_n(e, log_entries or []) if e["check"] == "grab_n"
                     else check_expectation(e, results))
            for d in found:
                out.append(dict(block=e.get("block", "R2"), kind="expectation",
                                 detail=f"{e['id']} ({e.get('note', '')}) - {label} {d['line']}",
                                 **({"a": d["got"]} if label == "A" else {"b": d["got"]}),
                                 expect=d["expect"], source=e["source"], date=e["date"]))
    return out


# ---------------------------------------------------------------------------
# Putting it together
# ---------------------------------------------------------------------------
def evaluate(log_a_lines, log_b_lines, expected_entries=None):
    entries_a = parse_log_lines(log_a_lines)
    entries_b = parse_log_lines(log_b_lines)

    rv_a = extract_runner_version(entries_a)
    rv_b = extract_runner_version(entries_b)
    for label, rv in (("A", rv_a), ("B", rv_b)):
        if rv not in KNOWN_RUNNER_VERSIONS:
            raise UnknownRunnerVersion(
                f"{label}: RUNNER_VERSION {rv!r} is not one this module knows ({sorted(KNOWN_RUNNER_VERSIONS)}) "
                "- board_run.py and eval_board.py must be updated together (see both module docstrings)")

    verdicts_a = {b: block_verdict(entries_a, b) for b in BLOCK_ORDER}
    verdicts_b = {b: block_verdict(entries_b, b) for b in BLOCK_ORDER}

    chain_results_a, chain_ended_a = chain_all_results(block_rx_lines(entries_a, "R2"))
    chain_results_b, chain_ended_b = chain_all_results(block_rx_lines(entries_b, "R2"))

    deviations = []
    for label, verdicts in (("A", verdicts_a), ("B", verdicts_b)):
        for b in BLOCK_ORDER:
            v = verdicts[b]
            if v in ("missing", "timeout"):
                deviations.append(dict(block=b, kind="incomplete", detail=f"{label} block {v}"))
    if verdicts_a["R2"] == "ok" and not chain_ended_a:
        deviations.append(dict(block="R2", kind="incomplete", detail="A: chain all ran but no @END"))
    if verdicts_b["R2"] == "ok" and not chain_ended_b:
        deviations.append(dict(block="R2", kind="incomplete", detail="B: chain all ran but no @END"))

    deviations += diff_kv_block(entries_a, entries_b, "R0", "status")
    deviations += diff_kv_block(entries_a, entries_b, "R1", "regs")
    deviations += diff_chain(chain_results_a, chain_results_b)
    # diff_raw_block("R3") only makes sense when BOTH sides actually got a
    # "test all" reply. A timeout already produces its own "incomplete"
    # deviation above (verdicts[b] in ("missing", "timeout")) - without this
    # guard, diff_raw_block() would ALSO run, and read_reset_banner()
    # (board_run.py) logs the reboot's boot banner as RX lines under the
    # SAME block it timed out in, so the "R3" RX lines it compares against
    # the other side's real reply are that banner text, not a second "test
    # all" reply - producing a confusing, redundant "ab_diff" alongside the
    # correct "incomplete" one (e.g. "A=[test] self: PASS
    # B=[boot] uart up on FRC, ..."), for the one block (R3) that uses a
    # raw, unfiltered line diff rather than diff_kv_block()'s key/value
    # parse (which already ignores non-"key: value" banner lines on its
    # own). Found running a real remote board-run rehearsal against a
    # deliberate mid-run hang recovered by re-flash (2026-09-28).
    if verdicts_a["R3"] not in ("missing", "timeout") and verdicts_b["R3"] not in ("missing", "timeout"):
        deviations += diff_raw_block(entries_a, entries_b, "R3")
    deviations += diff_grab_family(entries_a, entries_b, "R4")
    deviations += diff_grab_family(entries_a, entries_b, "R5")
    deviations += diff_kv_block(entries_a, entries_b, "R7", "status")
    deviations += check_stack_criterion(entries_b)

    expectation_deviations = evaluate_expectations(expected_entries, chain_results_a, chain_results_b,
                                                   entries_a, entries_b) \
        if expected_entries else []

    return dict(runner_version_a=rv_a, runner_version_b=rv_b,
                verdicts_a=verdicts_a, verdicts_b=verdicts_b,
                chain_ended_a=chain_ended_a, chain_ended_b=chain_ended_b,
                deviations=deviations, expectation_deviations=expectation_deviations)


def format_deviation(d):
    files = ",".join(BLOCK_FILES.get(d["block"], []))
    text = f"{d['block']} {d['kind']}: {d['detail']}"
    if "a" in d or "b" in d:
        text += f" A={d.get('a', '-')} B={d.get('b', '-')}"
    if "expect" in d:
        text += f" expect={d['expect']}"
    if "source" in d:
        text += f" expect_source={d['source']}@{d['date']}"
    text += f" files={files}"
    return text


def build_summary_text(log_a_lines, log_b_lines, expected_entries=None):
    """The one place summary.txt's content is built - see the module
    docstring's "one evaluator, not two" paragraph. Ends with exactly one
    line "overview: PASS" or "overview: FAIL"; callers that need the
    verdict programmatically (board_run.py's own return code) read that
    last line rather than re-deriving it."""
    result = evaluate(log_a_lines, log_b_lines, expected_entries)
    lines = ["board run evaluation", ""]
    lines.append("blocks:")
    for b in BLOCK_ORDER:
        lines.append(f"  {b}: A={result['verdicts_a'][b]} B={result['verdicts_b'][b]}")
    lines.append("")

    hard = [d for d in result["deviations"]]
    predicted = [d for d in result["expectation_deviations"] if d["source"] == "prediction"]
    established = [d for d in result["expectation_deviations"] if d["source"] == "run19"]
    hard += established

    all_reported = result["deviations"] + result["expectation_deviations"]
    if all_reported:
        lines.append(f"deviations ({len(all_reported)}):")
        for d in result["deviations"] + established:
            lines.append("  " + format_deviation(d))
        for d in predicted:
            lines.append("  " + format_deviation(d) + "  (missed prediction, not a regression)")
    else:
        lines.append("deviations: none")
    lines.append("")
    lines.append(f"missed predictions: {len(predicted)}" if predicted else "missed predictions: none")
    lines.append(f"overview: {'FAIL' if hard else 'PASS'}")
    return "\n".join(lines) + "\n"


def build_summary_text_single(log_lines, expected_entries=None, label="A", not_run_note=""):
    """The A-only summary for board-run-task.md's window between BR.4 (A
    committed) and BR.8 (B added): same log format, same BLOCK_ORDER
    verdicts and the same run19-sourced expectation checks as
    build_summary_text() above, but for one run instead of an A/B pair -
    there is no A/B diff section, and "prediction"-sourced entries are
    skipped outright (board-run-task.md ties every prediction to "the NEW
    firmware", which has not run yet). Ends with exactly one line
    "overview: PASS" or "overview: FAIL", the same convention
    build_summary_text() uses, so board_run.py's return code reads either
    the same way."""
    entries = parse_log_lines(log_lines)
    rv = extract_runner_version(entries)
    if rv not in KNOWN_RUNNER_VERSIONS:
        raise UnknownRunnerVersion(
            f"{label}: RUNNER_VERSION {rv!r} is not one this module knows ({sorted(KNOWN_RUNNER_VERSIONS)}) "
            "- board_run.py and eval_board.py must be updated together (see both module docstrings)")

    verdicts = {b: block_verdict(entries, b) for b in BLOCK_ORDER}
    chain_results, chain_ended = chain_all_results(block_rx_lines(entries, "R2"))

    deviations = []
    for b in BLOCK_ORDER:
        if verdicts[b] in ("missing", "timeout"):
            deviations.append(dict(block=b, kind="incomplete", detail=f"{label} block {verdicts[b]}"))
    if verdicts["R2"] == "ok" and not chain_ended:
        deviations.append(dict(block="R2", kind="incomplete", detail=f"{label}: chain all ran but no @END"))

    skipped_predictions = 0
    for e in (expected_entries or []):
        if e["source"] == "prediction":
            skipped_predictions += 1
            continue
        found = check_grab_n(e, entries) if e["check"] == "grab_n" else check_expectation(e, chain_results)
        for d in found:
            deviations.append(dict(block=e.get("block", "R2"), kind="expectation",
                                    detail=f"{e['id']} ({e.get('note', '')}) - {label} {d['line']}",
                                    a=d["got"], expect=d["expect"], source=e["source"], date=e["date"]))

    lines = ["board run evaluation", "", f"{label} only - B not run: {not_run_note}", "", "blocks:"]
    for b in BLOCK_ORDER:
        lines.append(f"  {b}: {label}={verdicts[b]}")
    lines.append("")
    if deviations:
        lines.append(f"deviations ({len(deviations)}):")
        for d in deviations:
            lines.append("  " + format_deviation(d))
    else:
        lines.append("deviations: none")
    lines.append("")
    lines.append(f"predictions skipped (B not run): {skipped_predictions}" if skipped_predictions
                 else "predictions skipped: none")
    lines.append(f"overview: {'FAIL' if deviations else 'PASS'}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def load_session_zip_logs(path):
    with zipfile.ZipFile(path) as z:
        a = z.read("A.log").decode("utf-8", "replace").splitlines()
        b = z.read("B.log").decode("utf-8", "replace").splitlines()
    return a, b


def selftest():
    """Synthetic logs, built with tools/board_run.py's own
    ReplayTarget/run_session wherever the scenario allows (so the log
    format this module parses cannot silently drift from what board_run.py
    actually writes) - a truncated log is the one exception, built by
    cutting a normal one, since "truncated" has no ReplayTarget knob.

    board_run.py is imported lazily, here, rather than at module level:
    board_run.py itself does `import eval_board` at ITS module level (BR.2:
    it calls build_summary_text() for its own summary.txt), so an
    unconditional `import board_run` up top would be a circular import
    evaluated while this module is still mid-definition. By the time this
    function runs, eval_board.py has finished loading, so board_run.py's
    own `import eval_board` just binds the already-complete module - no
    problem, and no need for either module to avoid the other's existence."""
    import board_run

    ok_all = True

    def check(label, cond):
        nonlocal ok_all
        ok_all = ok_all and bool(cond)
        print(f"  {'ok ' if cond else 'BAD'} {label}")
        return cond

    ui = board_run.StubUI(answers=["", "", "n"])

    # 1. Identical A/B: two clean stand-ins, nothing injected -> no ab_diff,
    # nothing incomplete.
    log_a, _, _ = board_run.run_session(board_run.ReplayTarget("A", has_route=True), ui, "A")
    log_b, _, _ = board_run.run_session(board_run.ReplayTarget("B", has_route=True), ui, "B")
    r = evaluate(log_a.lines, log_b.lines)
    check("identical A/B: no deviations",
          not r["deviations"] and r["verdicts_a"] == r["verdicts_b"])

    # 2. One FAIL only in B: B's "chain all" flips JUST S9.1's verdict token
    # to FAIL, otherwise byte-identical to the default - so the only
    # deviation diff_chain() can find is that one verdict, not a field
    # dump's worth of noise from an otherwise different reply.
    fail_lines = [l.replace(" -> PASS", " -> FAIL") if l.startswith("@S9.1 ") else l
                  for l in board_run.DEFAULT_CHAIN_ALL_LINES]
    log_a2, _, _ = board_run.run_session(board_run.ReplayTarget("A", has_route=True), ui, "A")
    log_b2, _, _ = board_run.run_session(
        board_run.ReplayTarget("B", has_route=True, chain_all_lines=fail_lines), ui, "B")
    r2 = evaluate(log_a2.lines, log_b2.lines)
    s91 = [d for d in r2["deviations"] if d["block"] == "R2" and "S9.1 verdict differs" in d["detail"]]
    check("one FAIL only in B: caught as an R2 verdict deviation",
          len(s91) == 1 and s91[0]["a"] == "PASS" and s91[0]["b"] == "FAIL")

    # 3. A timeout: A hangs on "regs" (R1); after the runner's own reset
    # handling it is still reported as an incomplete block, not silently
    # dropped.
    log_a3, _, _ = board_run.run_session(
        board_run.ReplayTarget("A", has_route=True, timeout_block="regs"), ui, "A")
    log_b3, _, _ = board_run.run_session(board_run.ReplayTarget("B", has_route=True), ui, "B")
    r3 = evaluate(log_a3.lines, log_b3.lines)
    check("a timeout is reported as incomplete",
          any(d["block"] == "R1" and "timeout" in d["detail"] for d in r3["deviations"]))

    # 3b. A timeout on R3 specifically (free-text diff_raw_block(), not the
    # key/value diff_kv_block() R1 above already exercises): the reboot's
    # boot banner (read_reset_banner(), board_run.py) is logged as RX lines
    # under the SAME block it timed out in, and must not leak into an
    # "ab_diff" alongside the correct "incomplete" deviation - a real
    # remote board-run rehearsal hit exactly this (2026-09-28: B hung on
    # "test all", recovered by re-flash, and the report showed both
    # "R3 incomplete: B block timeout" AND a spurious
    # "R3 ab_diff: ... A=[test] self: PASS B=[boot] uart up on FRC, ...").
    log_a3b, _, _ = board_run.run_session(
        board_run.ReplayTarget("A", has_route=True, timeout_block="test all"), ui, "A")
    log_b3b, _, _ = board_run.run_session(board_run.ReplayTarget("B", has_route=True), ui, "B")
    r3b = evaluate(log_a3b.lines, log_b3b.lines)
    check("R3 timeout: still reported as incomplete",
          any(d["block"] == "R3" and d["kind"] == "incomplete" for d in r3b["deviations"]))
    check("R3 timeout: no spurious ab_diff against the recovery boot banner",
          not any(d["block"] == "R3" and d["kind"] == "ab_diff" for d in r3b["deviations"]))

    # 4. Truncated without @END: cut a clean A log off partway through R2,
    # before "@END" and before R2's own "block end" line ever arrive - the
    # shape of a host crash or a lost connection mid-block, not something
    # any ReplayTarget knob produces on purpose.
    log_a4, _, _ = board_run.run_session(board_run.ReplayTarget("A", has_route=True), ui, "A")
    cut_at = next(i for i, l in enumerate(log_a4.lines) if " R2 " in l and "@S9.1" in l)
    truncated = log_a4.lines[:cut_at + 1]
    r4 = evaluate(truncated, log_b3.lines)
    check("truncated log (no @END): R2 reported incomplete, no crash",
          r4["verdicts_a"]["R2"] == "missing"
          and any(d["block"] == "R2" and d["detail"] == "A block missing" for d in r4["deviations"]))

    # 5. BR.6's real fields: A has none of them (the fixed pre-BR.6 baseline),
    # B has the full set. The plain ab_diff mechanism reports every one of
    # them as "only in B" without any field-name-specific code (proven
    # below); check_stack_criterion() is the one piece that DOES know two
    # of the names, and judges them.
    BR6_FIELDS_PASS = {
        "stack_size": "12480", "stack_used": "4928", "stack_hwm_addr": "34628",
        "stack_free_pct": "61",
        "buf_addr": "16512", "buf_align_mod4": "0", "buf_len": "4096",
        "buf_guard_ok": "1", "boot_stage": "9", "trap_seen": "0", "trap_vec": "0",
        "chain_mark": "0",
    }
    log_a5, _, _ = board_run.run_session(board_run.ReplayTarget("A", has_route=True), ui, "A")
    log_b5, _, _ = board_run.run_session(
        board_run.ReplayTarget("B", has_route=True, extra_status_fields=BR6_FIELDS_PASS), ui, "B")
    r5 = evaluate(log_a5.lines, log_b5.lines)
    extra = [d for d in r5["deviations"] if d["block"] == "R7" and d["kind"] == "ab_diff"
             and any(f in d["detail"] for f in BR6_FIELDS_PASS)]
    check("A without any BR.6 field, B with all of them: every one reported, not a crash",
          len(extra) == len(BR6_FIELDS_PASS) and all(d.get("a") is None for d in extra))
    check("BR.6 stack rule: 61% free in B passes, nothing NOT_AVAILABLE-flagged",
          not any(d["kind"] == "expectation" and "stack_free_pct" in d["detail"] for d in r5["deviations"])
          and not any(d["kind"] == "expectation" and "buf_guard_ok" in d["detail"] for d in r5["deviations"]))

    # 5b. Below the BR.9 pass criterion (< 25% free): a judged expectation
    # deviation, not just the ordinary ab_diff every field already gets.
    fields_low = dict(BR6_FIELDS_PASS, stack_free_pct="10")
    log_b5b, _, _ = board_run.run_session(
        board_run.ReplayTarget("B", has_route=True, extra_status_fields=fields_low), ui, "B")
    r5b = evaluate(log_a5.lines, log_b5b.lines)
    low = [d for d in r5b["deviations"] if d["kind"] == "expectation" and "stack_free_pct" in d["detail"]]
    check("BR.6 stack rule: 10% free in B fails, reported with A=NOT_AVAILABLE",
          len(low) == 1 and low[0]["a"] == "NOT_AVAILABLE" and low[0]["b"] == "10")

    # 5c. A guard word behind the DMA buffer no longer intact in B.
    fields_guard = dict(BR6_FIELDS_PASS, buf_guard_ok="0")
    log_b5c, _, _ = board_run.run_session(
        board_run.ReplayTarget("B", has_route=True, extra_status_fields=fields_guard), ui, "B")
    r5c = evaluate(log_a5.lines, log_b5c.lines)
    guard = [d for d in r5c["deviations"] if d["kind"] == "expectation" and "buf_guard_ok" in d["detail"]]
    check("BR.6 buffer rule: guard word gone in B, reported with A=NOT_AVAILABLE",
          len(guard) == 1 and guard[0]["a"] == "NOT_AVAILABLE" and guard[0]["b"] == "0")

    # 5d. B without stack_free_pct at all (the plain stand-in every other
    # selftest scenario uses, including test 1's "identical A/B") is not
    # judged - nothing to compare against 25%, and this is what keeps test
    # 1 free of a spurious deviation now that this check exists.
    not_judged = check_stack_criterion(parse_log_lines(log_a.lines))
    check("BR.6 stack rule: B without stack_free_pct at all is not judged",
          not_judged == [])

    # 6. An unknown RUNNER_VERSION is refused, not silently misread.
    bad = ["0 EV - RUNNER_VERSION=99 label=A port=x"]
    try:
        evaluate(bad, log_b.lines)
        check("unknown RUNNER_VERSION refused", False)
    except UnknownRunnerVersion:
        check("unknown RUNNER_VERSION refused", True)

    # 7. tests/board/expected.json actually loads and is checkable (the
    # run19-sourced 8 MSPS clean-stream fact, against the clean default
    # chain-all reply every ReplayTarget starts with).
    expected = load_expected()
    r7 = evaluate(log_a.lines, log_b.lines, expected)
    check("tests/board/expected.json loads with at least one entry", len(expected) >= 1)
    check("expected.json entries hold against the clean stand-in (no expectation deviations)",
          len(r7["expectation_deviations"]) == 0)

    # 8. build_summary_text() runs end to end and ends with the one
    # required "overview: ..." line.
    text = build_summary_text(log_a.lines, log_b.lines, expected)
    check("build_summary_text() ends with an overview line",
          text.rstrip("\n").splitlines()[-1].startswith("overview: "))

    # 9. build_summary_text_single() (BR.4: A only, B not built yet) - the
    # clean A-only log passes with no deviations and skips the
    # prediction-sourced entries (about the NEW firmware) rather than
    # silently treating "not run" as "confirmed".
    text_a_only = build_summary_text_single(log_a.lines, expected, label="A",
                                             not_run_note="firmware not available yet (BR.8)")
    n_predictions = sum(1 for e in expected if e["source"] == "prediction")
    check("build_summary_text_single() ends with an overview line",
          text_a_only.rstrip("\n").splitlines()[-1].startswith("overview: "))
    check("build_summary_text_single(): clean A alone is PASS",
          text_a_only.rstrip("\n").splitlines()[-1] == "overview: PASS")
    check("build_summary_text_single(): prediction-sourced entries are skipped, not checked",
          n_predictions == 0 or f"predictions skipped (B not run): {n_predictions}" in text_a_only)
    log_a_timeout, _, _ = board_run.run_session(
        board_run.ReplayTarget("A", has_route=True, timeout_block="regs"), ui, "A")
    text_a_timeout = build_summary_text_single(log_a_timeout.lines, label="A")
    check("build_summary_text_single(): a timeout in A alone is reported and FAILs",
          "R1 incomplete: A block timeout" in text_a_timeout
          and text_a_timeout.rstrip("\n").splitlines()[-1] == "overview: FAIL")

    # 10. "grab_n" (29.09.2026): R4.gui's grabs must carry the buf length.
    # A stand-in B whose firmware ignores buf (the pre-955c473 fault) is
    # reported against the r4gui-grab-length entry; the honest B is not.
    t_bad = board_run.ReplayTarget("B", has_route=True)
    t_bad.buf_follows, t_bad._fixed_n = False, 1024
    log_bad, _, _ = board_run.run_session(t_bad, board_run.StubUI(answers=["", "", "n"]), "B")
    r10 = evaluate(log_a.lines, log_bad.lines, expected)
    check("grab_n: a B that ignores 'buf' is reported (R4.gui grab n)",
          any("r4gui-grab-length" in d["detail"] for d in r10["expectation_deviations"]))
    check("grab_n: the honest B is not", not any("r4gui-grab-length" in d["detail"]
                                                  for d in r7["expectation_deviations"]))

    print("eval_board", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("session", nargs="?", help="the session zip, or A.log (paired with --b-log)")
    ap.add_argument("--b-log", help="B.log, when `session` is A.log rather than a zip")
    ap.add_argument("--expected", default=DEFAULT_EXPECTED_PATH,
                     help="tests/board/expected.json (default: %(default)s)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)

    if a.selftest:
        return selftest()
    if not a.session:
        ap.error("a session zip, or A.log with --b-log, is required (or --selftest)")

    if a.b_log:
        with open(a.session, encoding="utf-8", errors="replace") as f:
            log_a_lines = f.read().splitlines()
        with open(a.b_log, encoding="utf-8", errors="replace") as f:
            log_b_lines = f.read().splitlines()
    else:
        log_a_lines, log_b_lines = load_session_zip_logs(a.session)

    expected_entries = load_expected(a.expected)
    try:
        text = build_summary_text(log_a_lines, log_b_lines, expected_entries)
    except UnknownRunnerVersion as e:
        print(f"eval_board: refused - {e}")
        return 2
    print(text)
    return 0 if text.rstrip("\n").splitlines()[-1] == "overview: PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
