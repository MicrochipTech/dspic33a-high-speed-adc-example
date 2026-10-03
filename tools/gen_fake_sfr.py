#!/usr/bin/env python3
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

"""gen_fake_sfr.py - host-side stand-in for <xc.h>, generated from the DFP.

Started as a P0.3 spike prototype; P0.4 (docs/IMPLEMENTATION-PLAN.md) made
it the real generator behind tools/trace.bat. Decision of 26.09.2026
(tests/trace/README.md): approach (a), snapshot diff in plain C - so only
the DEFAULT output (--style symbols, no --cxx) is used by the harness. The
--cxx output (C++ proxies, approach b) and --style macros are kept only
because they show, concretely, why those two were rejected; nothing in
tests/trace/ builds them.

Inputs (both from the device pack, nothing typed by hand):
  - the device header  .../xc16/support/dsPIC33A/h/p<MCU>.h
      every SFR as `extern volatile uint32_t X __attribute__((__sfr__))`
      (a few with `far`), a bit-field typedef `XBITS` and
      `extern volatile XBITS Xbits`, `#define X X`, the `_X_F_MASK/_POSITION/
      _LENGTH` macros, `_F` short names, __DATA_BASE etc.
  - the linker script  .../xc16/support/dsPIC33A/gld/p<MCU>.gld
      the addresses (`X = 0x1CE0;`) - the header has none.

Output (into --out DIR):
  xc.h         C: the header body verbatim (typedefs, masks, short names,
               memory macros, `#define X X`), each SFR declaration kept as
               `extern volatile T X;` with the XC-DSC attribute dropped, and
               __DATA_BASE/__DATA_LENGTH widened for 64-bit host addresses.
               #includes "sfr_host.h" from the harness (Nop(), interrupt
               attributes, the `reset` instruction).
  sfr_syms.ld  the host "linker script" placing every X and Xbits at
               sfr_mem + 4*idx, idx = rank of X's device address. All SFRs
               are ONE contiguous array: a snapshot is a memcpy, a diff a
               loop over SFR_COUNT words, and page-protecting that array
               traps every access (tests/trace/README.md). `&X` stays a
               link-time constant, so static tables of SFR addresses
               (adc.c) compile. Needs -Wl,--disable-dynamicbase: the
               symbols are absolute, ASLR would move sfr_mem away from them.
  --style macros instead: `#define X SFR_REG(idx)` - kept only to show why
               not: 36 register names are also bit-field names (PC, SPLIM,
               FSCL...), and a function-call macro breaks `&X` in static
               initialisers.
  sfr_table.c  sfr_info[idx] = {address, name}, plus sfr_layout_check(): for
               every named bit field, the mask the HOST compiler gives it,
               compared with the pack's _X_F_MASK. Catches MinGW's
               ms_struct bit-field layout (build with -mno-ms-bitfields).
               And (P0.9, 27.09.2026) sfr_reset[idx]: the device's reset
               value of every SFR, from the pack's THIRD description of
               the silicon, the ATDF (atdf/dsPIC<MCU>.atdf, attribute
               `initval` - the one P0.8 found equal to the simulator's
               reset value for all 23 registers it could read). The
               harness's trace_begin() presets sfr_mem[] from it, so a
               field no driver touches (AD3CON.RPTCNT = 18) shows its
               real value in the trace instead of 0. An SFR the ATDF has
               no register for (the CPU's, APG*, PMD*, ...: 445 on the
               MPS512, 441 on the MPS506, listed by check_fake_sfr.py -v
               as "none") keeps 0 and is marked so in the table. The
               header-to-ATDF matching is check_fake_sfr.match_sfr(),
               the same rule the P0.8 check verifies addresses and
               fields with - so every initval comes from an ATDF entry
               that check has confirmed is the same register.
  xc_cxx.h     (--cxx) C++ proxies for approach (b): Reg<idx> per register,
               a struct of Field<idx,pos,len> per bit-field typedef, positions
               from the pack's _POSITION/_LENGTH macros.

Usage (tools/trace.bat runs exactly this, every time it runs):
  python tools/gen_fake_sfr.py --mcu 33AK512MPS512 --out build/trace/gen

tools/check_fake_sfr.py (P0.8) re-reads the same two inputs through
parse_header()/parse_gld() below and checks every address and bit-field
mask against the pack's ATDF; tools/trace.bat runs it after the scenarios.
"""
import argparse
import os
import re
import sys

DFP_DEFAULT = (r"C:\Program Files\Microchip\MPLABX\v6.35\packs\Microchip"
               r"\dsPIC33AK-MP_DFP\1.4.260\xc16\support\dsPIC33A")


def atdf_dir_default(dfp):
    """The pack's atdf directory for a given xc16/support/dsPIC33A one -
    shared with check_fake_sfr.py and trace_build.py."""
    return os.path.normpath(os.path.join(dfp, "..", "..", "..", "atdf"))


RE_EXTERN = re.compile(
    r"^extern\s+(?:volatile\s+)?(\w+)\s+(\w+)\s+__attribute__\(\((.*)\)\);\s*$")
RE_SELFDEF = re.compile(r"^#define\s+(\w+)\s+(\w+)\s*$")
RE_GLD = re.compile(r"^\s*(_?\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;")
RE_TYPEDEF_START = re.compile(r"^(?:__extension__\s+)?typedef\s+struct\s+tag(\w+)\s*\{")
RE_TYPEDEF_END = re.compile(r"^\}\s*(\w+)\s*;")
RE_FIELD = re.compile(r"^\s*uint(8|16|32)_t\s+(\w+)\s*:\s*(\d+)")
RE_MASKDEF = re.compile(r"^#define\s+_(\w+?)_(\w+)_(POSITION|LENGTH|MASK)\s+(0x[0-9A-Fa-f]+)")


def parse_gld(path):
    addr = {}
    with open(path, encoding="latin-1") as f:
        for line in f:
            m = RE_GLD.match(line)
            if m:
                addr[m.group(1)] = int(m.group(2), 16)
    return addr


def parse_header(lines):
    """Pass 1 over the device header: SFR declarations, bit-field typedefs,
    mask macros. Shared with tools/check_fake_sfr.py (P0.8), which checks
    exactly what this generator consumes against the pack's ATDF.
    Returns (regs, typedefs, maskdefs):
      regs      name -> (kind, type); kind 'reg' (uint32_t) or 'bits'
      typedefs  XBITS -> list of named (field, width) in declaration order
      maskdefs  (reg, field) -> {POSITION, LENGTH, MASK}"""
    regs = {}
    typedefs = {}
    maskdefs = {}
    cur = None
    for ln in lines:
        m = RE_EXTERN.match(ln)
        if m:
            typ, name, _ = m.groups()
            regs[name] = ("reg" if typ == "uint32_t" else "bits", typ)
            continue
        m = RE_TYPEDEF_START.match(ln)
        if m:
            cur = []
            continue
        if cur is not None:
            m = RE_TYPEDEF_END.match(ln)
            if m:
                typedefs[m.group(1)] = cur
                cur = None
                continue
            m = RE_FIELD.match(ln)
            if m:
                cur.append((m.group(2), int(m.group(3))))
            continue
        m = RE_MASKDEF.match(ln)
        if m:
            maskdefs.setdefault((m.group(1), m.group(2)), {})[m.group(3)] = int(m.group(4), 16)
    return regs, typedefs, maskdefs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcu", default="33AK512MPS512")
    ap.add_argument("--dfp", default=DFP_DEFAULT,
                    help="the pack's xc16/support/dsPIC33A directory")
    ap.add_argument("--out", required=True)
    ap.add_argument("--atdf-dir", default=None,
                    help="the pack's atdf directory, for the reset values (default: derived from --dfp)")
    ap.add_argument("--cxx", action="store_true", help="also write xc_cxx.h")
    ap.add_argument("--style", choices=("symbols", "macros"), default="symbols",
                    help="symbols: keep `extern volatile T X;` and place X by sfr_syms.ld "
                         "(default); macros: `#define X SFR_REG(idx)` (breaks on the 36 "
                         "register names that are also bit-field names, e.g. PC, SPLIM)")
    a = ap.parse_args()

    hdr = os.path.join(a.dfp, "h", "p%s.h" % a.mcu)
    gld = os.path.join(a.dfp, "gld", "p%s.gld" % a.mcu)
    gaddr = parse_gld(gld)
    with open(hdr, encoding="latin-1") as f:
        lines = f.read().splitlines()

    regs, typedefs, maskdefs = parse_header(lines)

    # Addresses. The gld names every register X and X's bits as _Xbits.
    def address_of(name):
        for key in (name, "_" + name):
            if key in gaddr:
                return gaddr[key]
        return None

    missing = [n for n in regs if address_of(n) is None]
    addrs = sorted({address_of(n) for n in regs if address_of(n) is not None})
    idx_of_addr = {ad: i for i, ad in enumerate(addrs)}
    # For the name table: the scalar name wins; a bits-only register keeps Xbits.
    name_of_idx = {}
    for n, (kind, _) in sorted(regs.items(), key=lambda kv: kv[1][0] != "reg"):
        ad = address_of(n)
        if ad is None:
            continue
        i = idx_of_addr[ad]
        if i not in name_of_idx:
            name_of_idx[i] = n[:-4] if (kind == "bits" and n.endswith("bits")) else n

    # ---- reset values (P0.9) --------------------------------------------
    # The ATDF resolver is check_fake_sfr.py's (P0.8): the same instance/
    # group/register chain and the same name-then-address matching that
    # the static check verifies every address and field mask with.
    # Imported here, lazily, because check_fake_sfr imports this module
    # for parse_header()/parse_gld() at load time.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import check_fake_sfr
    atdf = os.path.join(a.atdf_dir or atdf_dir_default(a.dfp), "dsPIC%s.atdf" % a.mcu)
    _r, by_name, by_addr, _notes = check_fake_sfr.resolve_atdf(atdf)
    reset = [0] * len(addrs)
    reset_src = [None] * len(addrs)      # "name" | "addr" | None (not in the ATDF)
    for i, ad in enumerate(addrs):
        reg, how = check_fake_sfr.match_sfr(name_of_idx[i], ad, by_name, by_addr)
        if reg is not None:
            reset_src[i] = how
            reset[i] = (reg.initval or 0) & 0xFFFFFFFF
    n_none = sum(1 for s in reset_src if s is None)
    n_nonzero = sum(1 for v in reset if v)

    os.makedirs(a.out, exist_ok=True)

    # ---- xc.h (C) -------------------------------------------------------
    out = []
    out.append("/* GENERATED by tools/gen_fake_sfr.py from p%s.h + p%s.gld - do not edit. */" % (a.mcu, a.mcu))
    out.append("#ifndef FAKE_XC_H")
    out.append("#define FAKE_XC_H")
    out.append("#define __dsPIC33%s__ 1" % a.mcu[2:] if a.mcu.startswith("33") else "")
    out.append("#include \"sfr_host.h\"   /* Nop() etc., and SFR_REG/SFR_BITS for --style macros */")
    out.append("#define SFR_COUNT %du" % len(addrs))
    ld = ["/* GENERATED by tools/gen_fake_sfr.py - do not edit. Host placement of",
          " * every SFR inside sfr_mem[] (index = rank of its device address), the",
          " * same job the device's .gld does: X and Xbits get the same address,",
          " * `&X` is a link-time constant, no SFR name is a macro. */"]
    for ln in lines:
        m = RE_SELFDEF.match(ln)
        if m and m.group(1) == m.group(2) and a.style == "macros":
            continue                                    # "#define X X"
        m = RE_EXTERN.match(ln)
        if m:
            typ, name, _ = m.groups()
            ad = address_of(name)
            if ad is None:
                out.append("/* %s: no address in the gld - not provided */" % name)
            elif a.style == "symbols":
                out.append("extern volatile %s %s; /* 0x%06X */" % (typ, name, ad))
                ld.append("%s = sfr_mem + %d; /* 0x%06X */" % (name, 4 * idx_of_addr[ad], ad))
            elif typ == "uint32_t":
                out.append("#define %s SFR_REG(%du) /* 0x%06X */" % (name, idx_of_addr[ad], ad))
            else:
                out.append("#define %s SFR_BITS(%du, %s) /* 0x%06X */" % (name, idx_of_addr[ad], typ, ad))
            continue
        m = re.match(r"^#define\s+(__DATA_BASE|__DATA_LENGTH)\s+(\S+)", ln)
        if m:
            # Host RAM is not at 0x4000..0x13FFF. dma.c checks that a
            # buffer's (uint32_t) address lies in this window; on a 64-bit
            # host that is the truncated host address, so the window has to
            # be the whole 32-bit range or every buffer is "outside RAM".
            host = "0x4u" if m.group(1) == "__DATA_BASE" else "0xFFFFFFF8u"
            out.append("#define %s %s /* host; device: %s */" % (m.group(1), host, m.group(2)))
            continue
        out.append(ln)
    out.append("#endif /* FAKE_XC_H */")
    with open(os.path.join(a.out, "xc.h"), "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    if a.style == "symbols":
        with open(os.path.join(a.out, "sfr_syms.ld"), "w", newline="\n") as f:
            f.write("\n".join(ld) + "\n")

    # ---- sfr_table.c ----------------------------------------------------
    t = []
    t.append("/* GENERATED by tools/gen_fake_sfr.py - do not edit. */")
    t.append("#include <stdio.h>")
    t.append("#include <xc.h>")
    t.append("#include \"sfr_table.h\"")
    t.append("const sfr_info_t sfr_info[SFR_COUNT] = {")
    for i, ad in enumerate(addrs):
        t.append("    { 0x%06Xu, \"%s\" }," % (ad, name_of_idx[i]))
    t.append("};")
    t.append("const unsigned sfr_count = SFR_COUNT;")
    t.append("")
    t.append("/* Reset value of every SFR (P0.9): the pack's ATDF `initval`, matched to")
    t.append(" * the header's register by check_fake_sfr.match_sfr(). %d of %d from the"
             % (len(addrs) - n_none, len(addrs)))
    t.append(" * ATDF (%d of them non-zero); %d have no ATDF register and keep 0 - each"
             % (n_nonzero, n_none))
    t.append(" * marked \"not in the ATDF\" below. trace_begin() copies this into sfr_mem[]. */")
    t.append("const uint32_t sfr_reset[SFR_COUNT] = {")
    for i, ad in enumerate(addrs):
        t.append("    0x%08Xu, /* %s%s */" % (reset[i], name_of_idx[i],
                                              "" if reset_src[i] else ": not in the ATDF"))
    t.append("};")
    t.append("")
    t.append("/* Host bit-field layout against the pack's masks. Returns the number")
    t.append(" * of fields whose host mask differs; prints each. */")
    t.append("int sfr_layout_check(void)")
    t.append("{")
    t.append("    int bad = 0;")
    nchecked = 0
    for n, (kind, typ) in sorted(regs.items()):
        if kind != "bits" or address_of(n) is None or typ not in typedefs:
            continue
        reg = n[:-4]
        t.append("    _Static_assert(sizeof(%s) == 4u, \"%s is not 4 bytes on the host\");" % (typ, typ))
        for fld, _w in typedefs[typ]:
            md = maskdefs.get((reg, fld))
            if not md or "MASK" not in md:
                continue
            nchecked += 1
            t.append("    { union { uint32_t w; %s b; } u; u.w = 0xFFFFFFFFu; u.b.%s = 0u;"
                     " if ((~u.w) != 0x%08Xu) { bad++; printf(\"layout %s.%s: host 0x%%08lX pack 0x%08X\\n\", (unsigned long)~u.w); } }"
                     % (typ, fld, md["MASK"], reg, fld, md["MASK"]))
    t.append("    return bad;")
    t.append("}")
    with open(os.path.join(a.out, "sfr_table.c"), "w", newline="\n") as f:
        f.write("\n".join(t) + "\n")
    with open(os.path.join(a.out, "sfr_table.h"), "w", newline="\n") as f:
        f.write("/* GENERATED by tools/gen_fake_sfr.py - do not edit. */\n"
                "#ifndef SFR_TABLE_H\n#define SFR_TABLE_H\n#include <stdint.h>\n"
                "typedef struct { uint32_t addr; const char *name; } sfr_info_t;\n"
                "extern const sfr_info_t sfr_info[];\nextern const unsigned sfr_count;\n"
                "extern const uint32_t sfr_reset[];   /* ATDF initval per SFR (P0.9) */\n"
                "int sfr_layout_check(void);\n#endif\n")

    # ---- xc_cxx.h (C++ proxies, approach b) -----------------------------
    if a.cxx:
        c = []
        c.append("/* GENERATED by tools/gen_fake_sfr.py --cxx - do not edit. */")
        c.append("#ifndef FAKE_XC_CXX_H")
        c.append("#define FAKE_XC_CXX_H")
        c.append("#define __dsPIC33%s__ 1" % a.mcu[2:])
        c.append("#include \"sfr_proxy.hpp\"   /* Reg<>, Field<> - from the harness */")
        c.append("#define SFR_COUNT %du" % len(addrs))
        # every #define except self-defines and the typedef blocks
        cur = False
        skip = 0
        for ln in lines:
            if skip:
                skip -= 1
                continue
            if ln.startswith("#ifdef __cplusplus"):
                skip = 2                     # extern "C" { / } and its #endif
                continue
            if RE_TYPEDEF_START.match(ln):
                cur = True
                continue
            if cur:
                if RE_TYPEDEF_END.match(ln):
                    cur = False
                continue
            if RE_EXTERN.match(ln):
                continue
            m = RE_SELFDEF.match(ln)
            if m and m.group(1) == m.group(2):
                continue
            c.append(ln)
        nproxy = 0
        for n, (kind, typ) in sorted(regs.items()):
            ad = address_of(n)
            if ad is None:
                continue
            i = idx_of_addr[ad]
            if kind == "reg":
                c.append("inline Reg<%du> %s;" % (i, n))
            else:
                reg = n[:-4]
                fields = []
                for fld, _w in typedefs.get(typ, []):
                    md = maskdefs.get((reg, fld))
                    if md and "POSITION" in md and "LENGTH" in md and fld not in [f[0] for f in fields]:
                        fields.append((fld, md["POSITION"], md["LENGTH"]))
                body = " ".join("Field<%du,%du,%du> %s;" % (i, p, l, f) for f, p, l in fields)
                c.append("struct %s { %s };" % (typ, body))
                c.append("inline %s %s;" % (typ, n))
                nproxy += 1
        c.append("#endif /* FAKE_XC_CXX_H */")
        with open(os.path.join(a.out, "xc_cxx.h"), "w", newline="\n") as f:
            f.write("\n".join(c) + "\n")

    print("gen_fake_sfr: %d SFR declarations, %d addresses, %d bit-field typedefs, "
          "%d fields checked by sfr_layout_check(), %d without a gld address%s; "
          "reset values: %d from the ATDF initval (%d non-zero, %d matched by address only), "
          "%d not in the ATDF (kept 0)"
          % (len(regs), len(addrs), len(typedefs), nchecked, len(missing),
             (": " + ", ".join(missing[:8])) if missing else "",
             len(addrs) - n_none, n_nonzero, sum(1 for s in reset_src if s == "addr"), n_none))
    return 0


if __name__ == "__main__":
    sys.exit(main())
