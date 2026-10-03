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

"""export_core.py - the customer's handover as one ZIP, from a commit.

    python tools/export_core.py                 HEAD -> build/core-<rev>.zip
    python tools/export_core.py --rev 72a8b9f   any commit
    python tools/export_core.py --no-verify     skip the build check

What goes in (docs/CORE.md, "What to copy"): the core - src/drivers, src/port,
src/lib, src/core - the glue as a template - src/app (without main.c, the lab's
main()) and src/boards - the MPLAB X project core_example.X, docs/CORE.md with
the architecture pages, and the GUI (tools/adc_gui.py and the modules it
imports, its setup and start scripts). Nothing of src/lab/ or src/sim/.

Taken from the commit with `git archive`, never from the working tree, so the
ZIP is exactly a tested revision; a dirty tree only earns a warning. The ZIP
also gets src/app/version.h with that revision (the banner then names it at the
customer's, where there is no git) and a short README.md at its root.

The check (default): unpack into a temporary folder and compile it with the
EV74H48A configuration's own file list and include path, read from the
exported core_example.X - -O1 -Wall -Wextra, any warning fails. This proves the
ZIP is self-contained: nothing missing, nothing reaching back into the lab.
"""
import argparse
import io
import os
import subprocess
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
XC_DSC = os.environ.get("XC_DSC", r"C:\Program Files\Microchip\xc-dsc\v3.31")
DFP = os.environ.get("DFP", r"C:\Program Files\Microchip\MPLABX\v6.35\packs\Microchip\dsPIC33AK-MP_DFP\1.4.260\xc16")
CONF = "EV74H48A_Curiosity_Platform_MPS512"
MCU = "33AK512MPS512"

PATHS = ["src/drivers", "src/port", "src/lib", "src/core", "src/app", "src/boards",
         "core_example.X/Makefile", "core_example.X/nbproject/project.xml",
         "core_example.X/nbproject/configurations.xml",
         "docs/CORE.md", "docs/ARCHITECTURE.md",
         "docs/architecture_layers.svg", "docs/architecture_layers_dark.svg",
         "docs/architecture_datapath.svg", "docs/architecture_datapath_dark.svg",
         "tools/adc_gui.py", "tools/boards.py", "tools/eval_chain.py", "tools/pins128.py",
         "tools/pins64.py", "tools/protocol.py", "tools/remote.py", "tools/trigger.py",
         "tools/wavegen_model.py", "tools/sigproc_design.py", "tools/adc_gui.bat", "tools/gui_setup.bat",
         "tools/requirements-gui.txt", "tools/adc_gui_defaults.json"]
LEAVE_OUT = {"src/app/main.c", "src/app/version.h"}


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], capture_output=True, check=True).stdout


def readme(rev):
    return f"""# ADC/DMA streaming core for the dsPIC33AK512MPS512 - revision {rev}

Start with **docs/CORE.md**: what this is, what to adapt (src/app/, src/boards/),
where your processing goes (sigproc_block() in src/core/sigproc.c), how to build.

- MPLAB X: open core_example.X (configurations EV74H48A and EV17P63A, -O1).
- GUI: tools\\gui_setup.bat once, then tools\\adc_gui.bat; console on the board's
  USB-UART at 115200 baud.

src/app/version.h names revision {rev}; the boot banner shows it.
"""


def build_zip(rev, out):
    raw = git("archive", "--format=tar", rev, "--", *PATHS)
    names = []
    with tarfile.open(fileobj=io.BytesIO(raw)) as tar, \
            zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for m in tar.getmembers():
            if not m.isfile() or m.name in LEAVE_OUT:
                continue
            assert not m.name.startswith(("src/lab/", "src/sim/")), m.name
            z.writestr(m.name, tar.extractfile(m).read())
            names.append(m.name)
        z.writestr("src/app/version.h",
                   "/* written by tools/export_core.py - the exported revision */\n"
                   f'#define GIT_REV    "{rev}"\n#define GIT_BRANCH "export"\n#define GIT_DIRTY  0\n')
        z.writestr("README.md", readme(rev))
    return names


def verify(zip_path):
    """Compile the unpacked ZIP with core_example.X's own file list."""
    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(zip_path) as z:
            z.extractall(tmp)
        prj = os.path.join(tmp, "core_example.X")
        root = ET.parse(os.path.join(prj, "nbproject", "configurations.xml")).getroot()
        src = next(e for e in root.iter("logicalFolder") if e.get("name") == "SourceFiles")
        conf = next(c for c in root.find("confs") if c.get("name") == CONF)
        excluded = {i.get("path") for i in conf.findall("item") if i.get("ex") == "true"}
        files = [p.text for p in src.iter("itemPath") if p.text.endswith(".c") and p.text not in excluded]
        incs = next(p.get("value") for p in conf.iter("property")
                    if p.get("key") == "extra-include-directories").split(";")
        cmd = [os.path.join(XC_DSC, "bin", "xc-dsc-gcc.exe"), f"-mcpu={MCU}", f"-mdfp={DFP}",
               "-O1", "-Wall", "-Wextra"] + ["-I" + os.path.normpath(os.path.join(prj, i)) for i in incs] \
            + [f"-T{os.path.join(DFP, 'support', 'dsPIC33A', 'gld', 'p' + MCU + '.gld')}"] \
            + [os.path.normpath(os.path.join(prj, f)) for f in files] \
            + ["-o", os.path.join(tmp, "core.elf")]
        r = subprocess.run(cmd, capture_output=True, text=True)
        text = (r.stdout + r.stderr)
        warnings = [l for l in text.splitlines() if "warning:" in l and "--fill-upper" not in l]
        ok = r.returncode == 0 and not warnings and os.path.exists(os.path.join(tmp, "core.elf"))
        return ok, len(files), text if not ok else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rev", default="HEAD")
    ap.add_argument("--out")
    ap.add_argument("--no-verify", action="store_true")
    a = ap.parse_args()
    rev = git("rev-parse", "--short", a.rev).decode().strip()
    if a.rev == "HEAD" and git("status", "--porcelain", "-uno").strip():
        print(f"note: the working tree has uncommitted changes - the ZIP holds {rev} as committed")
    out = a.out or os.path.join(ROOT, "build", f"core-{rev}.zip")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    names = build_zip(rev, out)
    print(f"{out}: {len(names) + 2} files from {rev}")
    if a.no_verify:
        return 0
    ok, n, log = verify(out)
    print(f"build check ({CONF}, {n} sources, -O1 -Wall -Wextra): {'PASS' if ok else 'FAIL'}")
    if not ok:
        print(log[-4000:])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
