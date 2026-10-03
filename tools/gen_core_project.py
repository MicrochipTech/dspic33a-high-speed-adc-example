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

"""gen_core_project.py - derive the core's MPLAB X project from this one.

    python tools/gen_core_project.py          writes core_example.X/ (Makefile,
                                              nbproject/project.xml, configurations.xml)
    python tools/gen_core_project.py --check  exit 1 if core_example.X/ is not what this
                                              script would write (stale after a change
                                              to adc_dma_40msps.X)

core_example.X is the project a customer opens to build the core alone (CORE,
02.10.2026; docs/CORE.md): the files of tools\\build.bat core - drivers, port,
lib, core, the app glue - with src/core/example_main.c as main(), and no file of
src/lab/ or src/sim/, neither folder on the include path. It is DERIVED, not
kept by hand: every setting (device, pack, compiler, debugger, the per-board
file exclusions) comes from adc_dma_40msps.X/nbproject/configurations.xml, so a
change there reaches the core project by running this script again. Kept: the
two hardware configurations (EV74H48A, EV17P63A); dropped: the simulator one
(it needs src/sim/sim_dma.c). The logical folders in the IDE's tree are rebuilt
from the files' real folders (src/core, src/drivers, ...).
"""
import os
import sys
import uuid
import xml.etree.ElementTree as ET

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SRC_PRJ = os.path.join(ROOT, "adc_dma_40msps.X")
DST_PRJ = os.path.join(ROOT, "core_example.X")
NAME = "core_example"
KEEP_CONFS = ("EV74H48A_Curiosity_Platform_MPS512", "EV17P63A_Curiosity_Nano_MPS506")
MAIN_LAB = "../src/app/main.c"
MAIN_CORE = "../src/core/example_main.c"
FOLDER_ORDER = ("core", "drivers", "lib", "port", "app", "boards")


def is_lab(path):
    """A file the core build leaves out (tools\\build.bat core)."""
    return path.startswith(("../src/lab/", "../src/sim/")) or path == MAIN_LAB


def folder_of(path):
    return path.split("/")[2] if path.startswith("../src/") else ""


def rebuild_folders(parent, fname):
    """Replace parent's logical folder `fname` by one sub-folder per real
    source folder, lab files dropped, example_main.c added to Source Files."""
    lf = next(e for e in parent.findall("logicalFolder") if e.get("name") == fname)
    paths = [e.text for e in lf.iter("itemPath") if not is_lab(e.text)]
    if fname == "SourceFiles":
        paths.insert(0, MAIN_CORE)
    for sub in list(lf):
        lf.remove(sub)
    groups = {}
    for p in paths:
        groups.setdefault(folder_of(p), []).append(p)
    for f in sorted(groups, key=lambda f: FOLDER_ORDER.index(f) if f in FOLDER_ORDER else 99):
        sub = ET.SubElement(lf, "logicalFolder", name=f, displayName=f, projectFiles="true")
        for p in groups[f]:
            ET.SubElement(sub, "itemPath").text = p


def configurations():
    tree = ET.parse(os.path.join(SRC_PRJ, "nbproject", "configurations.xml"))
    root = tree.getroot()
    top = root.find("logicalFolder")
    rebuild_folders(top, "HeaderFiles")
    rebuild_folders(top, "SourceFiles")
    confs = root.find("confs")
    for conf in list(confs):
        if conf.get("name") not in KEEP_CONFS:
            confs.remove(conf)
            continue
        for item in list(conf.findall("item")):
            if is_lab(item.get("path")):
                conf.remove(item)
        for prop in conf.iter("property"):
            if prop.get("key") == "extra-include-directories":
                dirs = [d for d in prop.get("value").split(";")
                        if d not in ("../src/lab", "../src/sim")]
                prop.set("value", ";".join(dirs))
        # -O1 like tools\build.bat: every figure in docs/CORE.md was measured
        # with it. MPLAB X's default -O0 doubled the low-pass's CPU load (62-69 %
        # instead of 31 % at 1 MSPS) and missed halves at 8 MSPS with no
        # processing at all (board, 02.10.2026).
        c30 = conf.find("C30")
        for prop in c30.findall("property"):
            if prop.get("key") == "optimization-level":
                c30.remove(prop)
        ET.SubElement(c30, "property", key="optimization-level", value="1")   # a bare digit in the C30 section
    assert [c.get("name") for c in confs] == list(KEEP_CONFS), "a hardware configuration is missing"
    ET.indent(tree, "  ")
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            + ET.tostring(root, encoding="unicode") + "\n")


def project():
    """project.xml as text: name, creation uuid and the dropped configuration
    changed, everything else as the main project writes it (ElementTree would
    turn its two default namespaces into prefixes MPLAB X does not read)."""
    import re
    with open(os.path.join(SRC_PRJ, "nbproject", "project.xml"), encoding="utf-8") as f:
        text = f.read()     # text mode: line ends arrive as \n
    text = re.sub(r"<name>adc_dma_40msps</name>", "<name>" + NAME + "</name>", text, count=1)
    text = re.sub(r"<creation-uuid>[^<]*</creation-uuid>",
                  "<creation-uuid>" + str(uuid.uuid5(uuid.NAMESPACE_URL, "adc-dma-core-example")) + "</creation-uuid>",
                  text, count=1)
    for name in re.findall(r"<confElem>\s*<name>([^<]+)</name>", text):
        if name not in KEEP_CONFS:
            text = re.sub(r"[ \t]*<confElem>\s*<name>" + re.escape(name) + r"</name>.*?</confElem>\n",
                          "", text, count=1, flags=re.S)
    assert "<name>" + NAME + "</name>" in text and text.count("<confElem>") == len(KEEP_CONFS)
    return text


def makefile():
    with open(os.path.join(SRC_PRJ, "Makefile"), encoding="utf-8") as f:
        return f.read()


def outputs():
    return {os.path.join(DST_PRJ, "Makefile"): makefile(),
            os.path.join(DST_PRJ, "nbproject", "project.xml"): project(),
            os.path.join(DST_PRJ, "nbproject", "configurations.xml"): configurations()}


def summary(path, text):
    """What --check compares: the content that decides the build, not the
    formatting. MPLAB X rewrites configurations.xml in its own layout as soon
    as the project is opened (folders sorted, every tool option spelled out,
    CRLF) and the makefile generator rewrites languageToolchainVersion - none
    of that makes the project stale."""
    name = os.path.basename(path)
    if name == "configurations.xml":
        root = ET.fromstring(text)
        files = sorted(e.text for e in root.find("logicalFolder").iter("itemPath"))
        confs = []
        for c in root.find("confs"):
            # the configuration's own C30 section only (MPLAB X also writes
            # empty ones into every file's <item>), an empty value = absent
            props = {p.get("key"): p.get("value") for p in c.find("C30").findall("property")
                     if p.get("key") in ("extra-include-directories", "optimization-level",
                                         "preprocessor-macros") and p.get("value")}
            confs.append((c.get("name"), c.findtext("toolsSet/targetDevice"),
                          c.findtext("toolsSet/platformTool"),
                          sorted(i.get("path") for i in c.findall("item") if i.get("ex") == "true"),
                          sorted(props.items())))
        return files, confs
    if name == "project.xml":
        root = ET.fromstring(text)
        return sorted(e.text for e in root.iter() if e.tag.endswith("}name"))
    return text.replace("\r\n", "\n")


def main():
    check = "--check" in sys.argv[1:]
    stale = []
    for path, text in outputs().items():
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old == text or (check and old is not None and summary(path, old) == summary(path, text)):
            continue
        stale.append(os.path.relpath(path, ROOT))
        if not check:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
    if check:
        print("core_example.X: " + ("stale: " + ", ".join(stale) if stale else "up to date"))
        return 1 if stale else 0
    print("core_example.X: " + ("wrote " + ", ".join(stale) if stale else "unchanged"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
