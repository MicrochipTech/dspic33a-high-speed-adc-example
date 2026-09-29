#!/usr/bin/env python3
"""trace_build.py - P0.5b: the incremental build/run/compare engine behind
tools\\trace.bat.

Why this exists (docs/IMPLEMENTATION-PLAN.md P0.5b, "problem 2"): the old
trace.bat compiled every one of the 13 scenarios from scratch, in one
`gcc a.c b.c c.c ... -o NAME.exe` invocation each - 13 full compiles of
recorder.c/stubs.c/hwmodel.c plus every firmware .c a scenario names, even
though most of those files are IDENTICAL across several scenarios
(clock.c alone was recompiled by seven of them) and the fake header only
ever changes when the device pack or the generator does. Measured
(2026-09-27, this host): a full run took ~247 s; the dominant single cost
was NOT per-scenario source count (a 1-file scenario and a 9-file one
compiled+linked in about the same time, ~65-75 s cold), it was `gcc`
itself paying that cost fresh on every invocation - once the OS/toolchain
caches were warm (a second, identical invocation right after), the same
command dropped to ~15 s. There is no way to keep gcc's own caches warm
across 13 separate all-in-one invocations that each touch fresh output
paths for the first time; splitting compilation from linking and reusing
already-compiled objects (below) sidesteps the problem instead of chasing
it.

What this script does that the old trace.bat did not:
  - skips regenerating a fake header (tools/gen_fake_sfr.py) when the
    device pack header/gld and the generator script are all older than
    the header already on disk;
  - compiles each firmware/harness .c file to one .o under
    build\\trace\\obj\\<flavor>\\, and reuses that .o for every scenario
    that needs the same file compiled the same way, only recompiling when
    the source (or any header under src/*/ or tests/trace/harness, or
    the flavor's own generated xc.h) is newer than the .o already there;
  - compiles and links scenarios in parallel (ThreadPoolExecutor - each
    gcc invocation is its own OS process, so the GIL is not a bottleneck).

"Flavor" = (which generated header a scenario uses - the default device or
NAME.mcu's, tools/trace/README.md) x (that scenario's NAME.cflags, exactly
as written). Firmware/harness files that never look at HAVE_* (every one
except stubs.c - see stubs.c's own header) are cached under a REDUCED key
that drops any `-DHAVE_*` token, so e.g. clock.c compiles once per header
flavor and is reused by every scenario that shares it, regardless of which
HAVE_* combination that particular scenario also passes (harmless, unused
macros for a file that never tests them). stubs.c and each scenario's own
main .c are cached under the FULL flavor key, because stubs.c's behaviour
genuinely depends on HAVE_DIAG/HAVE_CAPTURE/HAVE_CHAINTEST (its own
#ifndef guards, tests/trace/harness/stubs.c).

Usage (same as before, tools\\trace.bat forwards its argument here):
  python tools/trace_build.py            check mode (the default)
  python tools/trace_build.py record     (re)write every golden trace
  python tools/trace_build.py clean      wipe build\\trace\\obj and every
                                          generated header dir first, then
                                          run check mode fresh
"""
import concurrent.futures
import glob
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# The firmware sources live under src/<folder>/ (P1.1); the .sources files name
# them relative to ROOT, and every folder is on the include path so the sources
# keep including each other as "name.h".
SRC_DIRS = [ROOT / "src" / d for d in ("drivers", "app", "cli", "tests", "lib", "diag", "sim", "port", "boards", "meter", "siggen")]
TRACEDIR = ROOT / "tests" / "trace"
SCEN = TRACEDIR / "scenarios"
HARNESS = TRACEDIR / "harness"
GOLDEN = TRACEDIR / "golden"
OUTDIR = ROOT / "build" / "trace"
OBJDIR = OUTDIR / "obj"
GEN = OUTDIR / "gen"
DEFAULT_MCU = "33AK512MPS512"
GEN_SCRIPT = ROOT / "tools" / "gen_fake_sfr.py"
CHECK_SCRIPT = ROOT / "tools" / "check_fake_sfr.py"   # its ATDF resolver feeds the generator (P0.9)
DFP_DEFAULT = (r"C:\Program Files\Microchip\MPLABX\v6.35\packs\Microchip"
               r"\dsPIC33AK-MP_DFP\1.4.260\xc16\support\dsPIC33A")

BASE_CFLAGS = ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
               "-mno-ms-bitfields", "-fno-strict-aliasing",
               "-Wno-pointer-to-int-cast"]
LDFLAGS = ["-Wl,--disable-dynamicbase"]


def sanitize(s):
    return re.sub(r"[^A-Za-z0-9_]+", "_", s).strip("_") or "base"


def read_lines(path):
    if not path.exists():
        return []
    return [ln.strip() for ln in path.read_text().splitlines() if ln.strip()]


def newest_mtime(paths):
    best = 0.0
    for p in paths:
        try:
            m = p.stat().st_mtime
        except FileNotFoundError:
            continue
        if m > best:
            best = m
    return best


def ensure_header(mcu, out):
    """Regenerates the fake header for `mcu` into `out` unless it is
    already newer than the device pack's header/gld and the generator
    script. Returns True on success."""
    hdr = Path(DFP_DEFAULT) / "h" / f"p{mcu}.h"
    gld = Path(DFP_DEFAULT) / "gld" / f"p{mcu}.gld"
    # P0.9: the reset values come from the pack's ATDF, resolved by
    # check_fake_sfr.py's parser - both are inputs of the generator now
    atdf = Path(DFP_DEFAULT).parent.parent.parent / "atdf" / f"dsPIC{mcu}.atdf"
    marker = out / "xc.h"
    deps_epoch = newest_mtime([hdr, gld, atdf, GEN_SCRIPT, CHECK_SCRIPT])
    if marker.exists() and marker.stat().st_mtime >= deps_epoch:
        return True
    print(f"trace: generating the fake SFR header for {mcu} -> {out}")
    out.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([sys.executable, str(GEN_SCRIPT), "--mcu", mcu, "--out", str(out)],
                        capture_output=True, text=True)
    if r.returncode != 0:
        print(f"trace: gen_fake_sfr.py FAILED for --mcu {mcu}")
        print(r.stdout)
        print(r.stderr)
        return False
    return True


def gen_dir_epoch(gen_dir, cache={}):
    key = str(gen_dir)
    if key not in cache:
        cache[key] = newest_mtime(gen_dir.glob("*"))
    return cache[key]


def global_header_epoch(cache=[]):
    if not cache:
        hdrs = [h for d in SRC_DIRS for h in d.glob("*.h")] + list(HARNESS.glob("*.h"))
        cache.append(newest_mtime(hdrs))
    return cache[0]


def is_stale(obj, src, gen_dir):
    if not obj.exists():
        return True
    obj_m = obj.stat().st_mtime
    if not src.exists():
        return True
    if src.stat().st_mtime > obj_m:
        return True
    if global_header_epoch() > obj_m:
        return True
    if gen_dir_epoch(gen_dir) > obj_m:
        return True
    return False


class Job:
    __slots__ = ("src", "obj", "flags", "incdirs", "gen_dir", "ok", "log")

    def __init__(self, src, obj, flags, incdirs, gen_dir):
        self.src = src
        self.obj = obj
        self.flags = flags
        self.incdirs = incdirs
        self.gen_dir = gen_dir
        self.ok = None
        self.log = ""


def compile_job(job):
    job.obj.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["gcc"] + BASE_CFLAGS + job.flags + job.incdirs + ["-c", str(job.src), "-o", str(job.obj)]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))
    job.ok = (r.returncode == 0)
    job.log = r.stdout + r.stderr
    return job


def plan_scenario(name, mcu_of_gendir):
    scen_c = SCEN / f"{name}.c"
    sources = read_lines(SCEN / f"{name}.sources")
    xflags = []
    for line in read_lines(SCEN / f"{name}.cflags"):
        xflags.extend(line.split())
    mcu_file = SCEN / f"{name}.mcu"
    mcu = mcu_file.read_text().strip() if mcu_file.exists() else DEFAULT_MCU
    gen_dir = GEN if mcu == DEFAULT_MCU else OUTDIR / f"gen_{mcu}"
    mcu_of_gendir[str(gen_dir)] = mcu

    nonhave = [f for f in xflags if "HAVE" not in f]
    tierA = sanitize(gen_dir.name + ("_" + "_".join(nonhave) if nonhave else ""))
    tierB = sanitize(gen_dir.name + "__" + "_".join(xflags))
    incdirs = [f"-I{HARNESS}", f"-I{gen_dir}"] + [f"-I{d}" for d in SRC_DIRS]

    objs = {}  # obj path -> Job (local to this scenario; merged into the global plan)

    def add(src, obj, flags):
        j = Job(src, obj, flags, incdirs, gen_dir)
        objs[str(obj)] = j
        return j

    recorder_obj = OBJDIR / tierA / "recorder.o"
    hwmodel_obj = OBJDIR / tierA / "hwmodel.o"
    stubs_obj = OBJDIR / tierB / "stubs.o"
    main_obj = OBJDIR / tierB / f"{name}__main.o"
    sfrtab_obj = gen_dir / "sfr_table.o"
    sfr_syms = gen_dir / "sfr_syms.ld"

    add(HARNESS / "recorder.c", recorder_obj, nonhave)
    add(HARNESS / "hwmodel.c", hwmodel_obj, nonhave)
    add(HARNESS / "stubs.c", stubs_obj, xflags)
    add(scen_c, main_obj, xflags)
    add(gen_dir / "sfr_table.c", sfrtab_obj, [])

    fw_objs = []
    for s in sources:
        obj = OBJDIR / tierA / f"{Path(s).stem}.o"
        add(ROOT / s, obj, nonhave)
        fw_objs.append(obj)

    link_objs = [main_obj, recorder_obj, stubs_obj, hwmodel_obj, sfrtab_obj] + fw_objs

    return dict(name=name, gen_dir=gen_dir, mcu=mcu, jobs=objs,
                link_objs=link_objs, sfr_syms=sfr_syms)


def link_scenario(plan):
    exe = OUTDIR / f"{plan['name']}.exe"
    cmd = ["gcc"] + [str(o) for o in plan["link_objs"]] + [str(plan["sfr_syms"])] + LDFLAGS + ["-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))
    return plan["name"], exe, (r.returncode == 0), (r.stdout + r.stderr)


def run_scenario(name, exe):
    trace_path = OUTDIR / f"{name}.trace"
    try:
        r = subprocess.run([str(exe)], capture_output=True, cwd=str(ROOT), timeout=30)
        trace_path.write_bytes(r.stdout)
    except subprocess.TimeoutExpired as e:
        trace_path.write_bytes(e.stdout or b"")
        print(f"{name}: exceeded the external 30s safety timeout (the harness's own watchdog "
              f"should have fired first - see tests/trace/README.md, \"runaway guard\")")
    return trace_path


def filter_errors(log):
    lines = [ln for ln in log.splitlines() if "error" in ln.lower()]
    return "\n".join(lines) if lines else log[-2000:]


def compare(trace_path, golden_path, mode):
    new_bytes = trace_path.read_bytes() if trace_path.exists() else b""
    if mode == "record":
        golden_path.parent.mkdir(parents=True, exist_ok=True)
        golden_path.write_bytes(new_bytes)
        return "RECORDED"
    if not golden_path.exists():
        return "FAIL - no golden trace yet (tools\\trace.bat record)"
    if golden_path.read_bytes() == new_bytes:
        return "PASS"
    # a short, human diff - not byte-for-byte fc, but enough to see what moved.
    import difflib
    old_lines = golden_path.read_text(errors="replace").splitlines()
    new_lines = trace_path.read_text(errors="replace").splitlines() if trace_path.exists() else []
    diff = list(difflib.unified_diff(old_lines, new_lines, "golden", "new", lineterm="", n=1))
    return "FAIL\n" + "\n".join(diff[:60])


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "check"
    if mode not in ("check", "record", "clean"):
        print(f"trace: unknown mode '{mode}' (expected nothing, 'record' or 'clean')")
        return 1

    if shutil.which("gcc") is None:
        print("trace: no gcc found on PATH - install a host MinGW gcc first.")
        return 1

    if mode == "clean":
        shutil.rmtree(OBJDIR, ignore_errors=True)
        for d in [GEN] + [Path(p) for p in glob.glob(str(OUTDIR / "gen_*"))]:
            shutil.rmtree(d, ignore_errors=True)
        print("trace: cache cleared (build\\trace\\obj and every generated header dir)")
        mode = "check"

    OUTDIR.mkdir(parents=True, exist_ok=True)
    GOLDEN.mkdir(parents=True, exist_ok=True)

    if not ensure_header(DEFAULT_MCU, GEN):
        return 1

    names = sorted(p.stem for p in SCEN.glob("*.c"))
    mcu_of_gendir = {}
    plans = {}
    for name in names:
        plans[name] = plan_scenario(name, mcu_of_gendir)

    # per-mcu header generation (default already done above; any NAME.mcu
    # override gets its own dir, generated once even if several scenarios
    # shared it - none do today, but the cache keys by gen_dir regardless).
    seen_gendirs = {str(GEN)}
    for plan in plans.values():
        gd = plan["gen_dir"]
        if str(gd) not in seen_gendirs:
            seen_gendirs.add(str(gd))
            if not ensure_header(plan["mcu"], gd):
                return 1

    # merge every scenario's compile jobs into one global, de-duplicated plan
    all_jobs = {}
    for plan in plans.values():
        for key, job in plan["jobs"].items():
            all_jobs.setdefault(key, job)

    stale_jobs = [j for j in all_jobs.values() if is_stale(j.obj, j.src, j.gen_dir)]
    workers = os.cpu_count() or 4
    if stale_jobs:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
            list(ex.map(compile_job, stale_jobs))

    failed_objs = {j.obj: j for j in all_jobs.values() if j.ok is False}

    total = len(names)
    passed = 0
    link_plans = []
    for name in names:
        plan = plans[name]
        broken = [j for j in plan["link_objs"] if j in failed_objs]
        # also check the plan's own compiled-here jobs (stubs/main/sfr_table
        # are only in plan["jobs"], link_objs already lists their paths too)
        if broken:
            print(f"{name}: BUILD FAILED")
            for j in broken:
                print(filter_errors(failed_objs[j].log))
            continue
        link_plans.append(plan)

    link_results = {}
    if link_plans:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
            for name, exe, ok, log in ex.map(link_scenario, link_plans):
                link_results[name] = (exe, ok, log)

    for name in names:
        if name not in link_results:
            continue  # already reported BUILD FAILED above
        exe, ok, log = link_results[name]
        if not ok:
            print(f"{name}: BUILD FAILED")
            print(filter_errors(log))
            continue
        trace_path = run_scenario(name, exe)
        golden_path = GOLDEN / f"{name}.trace"
        result = compare(trace_path, golden_path, mode)
        print(f"{name}: {result}")
        if result == "PASS" or result == "RECORDED":
            passed += 1

    # orphan goldens: a golden with no matching scenario source.
    for g in sorted(GOLDEN.glob("*.trace")):
        if not (SCEN / f"{g.stem}.c").exists():
            print(f"{g.stem}: WARNING - golden trace exists but tests\\trace\\scenarios\\{g.stem}.c does not")

    print()
    if passed == total:
        print(f"{passed}/{total} PASS")
        return 0
    print(f"{passed}/{total} FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
