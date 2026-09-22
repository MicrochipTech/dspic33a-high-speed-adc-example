#!/usr/bin/env python3
"""
setup.py - find the toolchain for this example and configure the build files.

Scans this machine for XC-DSC compilers and dsPIC33AK-MP device packs, checks
which combination can actually build the target device, lets you pick one, and
writes the paths into build.bat and Makefile.

    python setup.py              interactive
    python setup.py --auto       take the newest combination without asking
    python setup.py --list       show what was found, change nothing
    python setup.py --verify     run a test build after configuring

Standard library only, needs Python 3.8+.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

MCU = "33AK512MPS512"
PACK_NAME = "dsPIC33AK-MP_DFP"
SOURCE = "adc_dma_40msps.c"      # lives one level up, next to the .X project

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent              # repo root: holds the single .c file


# --------------------------------------------------------------------------
# Sort version numbers numerically: "v3.31" must sort above "v3.9", and
# "1.4.260" above "1.3.185". A plain string compare gets both backwards.
# --------------------------------------------------------------------------
def version_key(text):
    return tuple(int(n) for n in re.findall(r"\d+", str(text)))


# --------------------------------------------------------------------------
# Find compilers
#
# A hit only counts if xc-dsc-gcc actually exists - a leftover directory
# from an uninstall is not a compiler.
# --------------------------------------------------------------------------
def find_compilers():
    roots = [
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Microchip" / "xc-dsc",
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microchip" / "xc-dsc",
        Path(r"C:\Microchip\xc-dsc"),
        Path("/opt/microchip/xc-dsc"),
    ]
    exe = "xc-dsc-gcc.exe" if os.name == "nt" else "xc-dsc-gcc"
    found = []
    for root in roots:
        if not root.is_dir():
            continue
        for d in sorted(root.iterdir()):
            gcc = d / "bin" / exe
            if gcc.is_file():
                found.append({"dir": d, "gcc": gcc, "version": d.name})

    # A compiler on PATH counts too.
    which = shutil.which("xc-dsc-gcc")
    if which:
        gcc = Path(which)
        d = gcc.parent.parent
        if not any(f["dir"] == d for f in found):
            found.append({"dir": d, "gcc": gcc, "version": d.name + " (PATH)"})

    found.sort(key=lambda f: version_key(f["version"]), reverse=True)
    return found


# --------------------------------------------------------------------------
# Find device packs
#
# Two locations, and this is the part that is easy to miss: MPLAB X ships
# its own packs, which do NOT live under ~/.mchp_packs, and they can be
# newer or older than the ones downloaded through MPLAB X. On the machine
# this script was written on, 1.3.185 (user) and 1.4.260 (MPLAB X) sat
# side by side.
#
# A pack only counts if it carries the linker script for the target: the
# dsPIC33AK-MP packs contain the MPS parts, the -MC packs do not.
# --------------------------------------------------------------------------
def find_packs():
    roots = [Path.home() / ".mchp_packs" / "Microchip"]

    for pf in (os.environ.get("ProgramFiles", r"C:\Program Files"),
               os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        mplabx = Path(pf) / "Microchip" / "MPLABX"
        if mplabx.is_dir():
            for ver in mplabx.iterdir():
                p = ver / "packs" / "Microchip"
                if p.is_dir():
                    roots.append(p)

    found = []
    for root in roots:
        pack_root = root / PACK_NAME
        if not pack_root.is_dir():
            continue
        for ver in sorted(pack_root.iterdir()):
            # -mdfp points at the xc16 SUBDIRECTORY, not the pack root:
            # c30_device.info lives one level down. Point it at the root
            # and the compiler says "does not seem to support the selected
            # device" even though the pack does contain it.
            dfp = ver / "xc16"
            gld = dfp / "support" / "dsPIC33A" / "gld" / f"p{MCU}.gld"
            if gld.is_file():
                found.append({
                    "dir": ver,
                    "dfp": dfp,
                    "gld": gld,
                    "version": ver.name,
                    "origin": "MPLAB X" if "MPLABX" in str(root) else "user",
                    "has_info": (dfp / "bin" / "c30_device.info").is_file(),
                })

    found.sort(key=lambda f: version_key(f["version"]), reverse=True)
    return found


# --------------------------------------------------------------------------
def choose(items, label, fmt):
    print(f"\n{label}")
    for i, it in enumerate(items, 1):
        mark = "  (newest)" if i == 1 else ""
        print(f"  [{i}] {fmt(it)}{mark}")
    if len(items) == 1:
        print("  -> only one found, using it")
        return items[0]
    while True:
        answer = input(f"  select [1-{len(items)}, Enter = 1]: ").strip()
        if answer == "":
            return items[0]
        if answer.isdigit() and 1 <= int(answer) <= len(items):
            return items[int(answer) - 1]
        print("  please enter a number from the list")


# --------------------------------------------------------------------------
# Write the build files
#
# Only the path lines are replaced, everything else stays as it is, and a
# .bak copy is kept.
#
# Note on re.sub: the replacement has to go through a function. A Windows
# path contains backslashes, and re.sub reads those in the replacement as
# escape sequences - "C:\Program Files" dies with "bad escape \P". A lambda
# skips that interpretation entirely.
# --------------------------------------------------------------------------
def patch_build_bat(path, compiler, pack):
    if not path.is_file():
        print(f"  ! {path.name} not found, skipped")
        return False
    text = path.read_text(encoding="utf-8", errors="replace")
    shutil.copy2(path, path.with_suffix(path.suffix + ".bak"))

    text = re.sub(r"(?m)^set XC_DSC=.*$",
                  lambda m: "set XC_DSC=" + str(compiler["dir"]), text)
    text = re.sub(r"(?m)^set DFP=.*$",
                  lambda m: "set DFP=" + str(pack["dfp"]), text)

    path.write_text(text, encoding="utf-8")
    return True


def patch_makefile(path, compiler, pack):
    if not path.is_file():
        print(f"  ! {path.name} not found, skipped")
        return False
    text = path.read_text(encoding="utf-8", errors="replace")
    shutil.copy2(path, path.with_suffix(path.suffix + ".bak"))

    # Forward slashes in the Makefile, otherwise make eats the backslashes
    # as escape characters.
    cc = str(compiler["dir"]).replace("\\", "/")
    dfp = str(pack["dfp"]).replace("\\", "/")

    text = re.sub(r"(?m)^XC_DSC\s*\?=.*$", lambda m: f"XC_DSC  ?= {cc}", text)
    text = re.sub(r"(?m)^DFP\s*\?=.*$", lambda m: f"DFP     ?= {dfp}", text)

    path.write_text(text, encoding="utf-8")
    return True


# --------------------------------------------------------------------------
# Find make.exe - MPLAB X ships one, and many Windows machines have no
# other. Informational only.
# --------------------------------------------------------------------------
def find_make():
    which = shutil.which("make")
    if which:
        return Path(which)
    for pf in (os.environ.get("ProgramFiles", r"C:\Program Files"),
               os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        mplabx = Path(pf) / "Microchip" / "MPLABX"
        if not mplabx.is_dir():
            continue
        for ver in sorted(mplabx.iterdir(), key=lambda p: version_key(p.name),
                          reverse=True):
            cand = ver / "gnuBins" / "GnuWin32" / "bin" / "make.exe"
            if cand.is_file():
                return cand
    return None


# --------------------------------------------------------------------------
# Test build - proves the selected combination really works.
# --------------------------------------------------------------------------
def verify(compiler, pack):
    src = ROOT / SOURCE
    if not src.is_file():
        print(f"  ! {SOURCE} not found, test build skipped")
        return None

    out = HERE / "setup_verify.elf"
    cmd = [
        str(compiler["gcc"]),
        f"-mcpu={MCU}",
        f'-mdfp={pack["dfp"]}',
        "-O1", "-Wall",
        f'-T{pack["gld"]}',
        str(src), "-o", str(out),
    ]
    print("\nRunning test build ...")
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"  FAILED: {exc}")
        return False

    ok = res.returncode == 0 and out.is_file()
    if ok:
        print(f"  OK - {out.name} produced ({out.stat().st_size} bytes)")
        out.unlink()
    else:
        print(f"  FAILED (exit code {res.returncode})")
        for line in (res.stderr or res.stdout or "").splitlines()[:12]:
            print("   ", line)
    return ok


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="Find the toolchain and configure build.bat / Makefile.")
    ap.add_argument("--auto", action="store_true",
                    help="take the newest combination without asking")
    ap.add_argument("--list", action="store_true",
                    help="show what was found, change nothing")
    ap.add_argument("--verify", action="store_true",
                    help="run a test build after configuring")
    args = ap.parse_args()

    print("=" * 70)
    print(f"  Toolchain scan for {MCU}")
    print("=" * 70)

    compilers = find_compilers()
    packs = find_packs()

    if not compilers:
        print("\nNo XC-DSC compiler found.")
        print("Download: https://www.microchip.com/mplab/compilers")
        print("Looked under Program Files\\Microchip\\xc-dsc and on PATH.")
        return 1

    if not packs:
        print(f"\nNo {PACK_NAME} with a linker script for {MCU} found.")
        print("Download: https://packs.download.microchip.com/")
        print("Looked under:")
        print("  %USERPROFILE%\\.mchp_packs\\Microchip")
        print("  Program Files\\Microchip\\MPLABX\\<version>\\packs\\Microchip")
        print(f"\nNote: the {PACK_NAME.replace('-MP', '-MC')} packs do NOT")
        print("contain the MPS devices - it has to be the MP pack.")
        return 1

    print(f"\nFound {len(compilers)} compiler(s) and {len(packs)} matching pack(s).")

    if args.list:
        print("\nCompilers:")
        for c in compilers:
            print(f"  {c['version']:14s} {c['dir']}")
        print("\nDevice packs:")
        for p in packs:
            note = "" if p["has_info"] else "  [no c30_device.info!]"
            print(f"  {p['version']:12s} ({p['origin']:7s}) {p['dir']}{note}")
        mk = find_make()
        print(f"\nmake: {mk if mk else 'not found (use build.bat)'}")
        return 0

    if args.auto:
        compiler, pack = compilers[0], packs[0]
        print(f"\n--auto: compiler {compiler['version']}, pack {pack['version']}")
    else:
        compiler = choose(compilers, "XC-DSC compiler:",
                          lambda c: f"{c['version']:14s} {c['dir']}")
        pack = choose(packs, "Device pack:",
                      lambda p: f"{p['version']:12s} ({p['origin']}) {p['dir']}")

    # XC-DSC v3.21 has dsPIC33A support built in, but only the MC variants.
    # For the MPS parts it is too old.
    if version_key(compiler["version"])[:2] < (3, 30):
        print(f"\n  Warning: {compiler['version']} is older than v3.30.")
        print("  v3.31 or newer is recommended for the MPS devices;")
        print("  v3.21 only knows the MC variants of dsPIC33A.")

    if not pack["has_info"]:
        print("\n  Warning: this pack has no xc16/bin/c30_device.info.")
        print("  The compiler may refuse it.")

    print("\nConfiguring:")
    print(f"  compiler : {compiler['dir']}")
    print(f"  DFP      : {pack['dfp']}")
    print(f"  linker   : {pack['gld'].name}")
    print("  note     : the source writes FICD_NOBTSWP numerically, so it")
    print("             builds against any pack version")

    print()
    n = 0
    if patch_build_bat(HERE / "build.bat", compiler, pack):
        print("  wrote:        build.bat  (backup: build.bat.bak)")
        n += 1
    if patch_makefile(HERE / "Makefile", compiler, pack):
        print("  wrote:        Makefile   (backup: Makefile.bak)")
        n += 1

    if n == 0:
        print("  Nothing changed.")
        return 1

    ok = verify(compiler, pack) if args.verify else None

    print("\n" + "=" * 70)
    if ok is False:
        print("  Configuration written, but the test build failed.")
        print("  See the message above - most likely the compiler and pack")
        print("  versions do not go together.")
        return 1

    print("  Done. Build with:   build.bat")
    mk = find_make()
    if mk:
        print(f"  or with make:       \"{mk}\"")
    if ok is None:
        print("  (run with --verify to confirm with a real build)")
    print("=" * 70)
    return 0


if __name__ == "__main__":
    sys.exit(main())
