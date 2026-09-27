#!/usr/bin/env python3
"""check_fake_sfr.py - P0.8: the generated fake <xc.h> against the device's
ATDF, and (optionally) against what the MPLAB X simulator holds after boot.

The register trace (tests/trace/) is only as good as tools/gen_fake_sfr.py's
output, and that generator reads the C device header (p<MCU>.h) and the
linker script (p<MCU>.gld) - both hand-maintained files of the pack. This
script checks exactly what the generator consumed against the pack's
third description of the same silicon, the ATDF (atdf/dsPIC<MCU>.atdf),
which CLAUDE.md records as the one that was right every time the pack and
the datasheet disagreed. It never reads the generated header: it re-parses
the same inputs with the same parser (gen_fake_sfr.parse_header/parse_gld),
so what it checks is what the generator saw.

Static check (always, cheap - tools\\trace.bat runs it for both devices):
  for every SFR the generator emits (every `extern volatile uint32_t X`
  with a gld address), found in the ATDF by name (see resolve_atdf() for
  the naming), else by address:
    address    the gld's address == the ATDF's, resolved through the
               peripheral instance / register-group / register chain;
    width      the ATDF's `size` - the header declares every SFR 32-bit,
               the ATDF says 2 for the GPIO/PPS registers (upper half
               unimplemented): reported, not an error;
    fields     every _X_F_MASK of the header == the ATDF bitfield of the
               same name; fields known to one side only are listed.
  Errors (exit 1): an address that differs, a same-named field whose mask
  differs. Two things the ATDF does NOT say and therefore cannot be
  errors, reported as counts (the lists with --verbose):
    - the element pitch of a counted register group. The ATDF's `size`
      attribute is the sum of the group's registers (ADC CH: 28), not the
      distance between two elements (the gld and the pack's .PIC file:
      32). Where the ATDF's pitch and the gld's differ, the check demands
      that element `start-index` matches the ATDF exactly and that every
      later element sits at one consistent pitch, and reports that pitch
      once per group ("pitch").
    - registers the ATDF has no entry for (the CPU's, the PMD, ...):
      "none", listed by name, unverifiable here.

Dynamic check (--sim-dump, needs a simulator run first):
    tools\\build.bat smoke
    python tools\\sim_trap.py --smoke --dump-sfr @tests\\trace\\golden\\boot.trace
    python tools\\check_fake_sfr.py --sim-dump build\\sfr_dump.txt \\
                                   --trace tests\\trace\\golden\\boot.trace
  compares the simulator's SFR values after "[smoke] DONE" with the end
  state of the host `boot` trace and classifies each register:
    agree        same value;
    status       differs only in bits the ATDF marks read-only, or in a
                 self-clearing switch request (*SWEN) the host model
                 clears as the hardware would;
    not stored   the simulator still shows its reset value, or reads the
                 written fields back as 0 (PLLxDIV: the P0.3 finding -
                 PLLPRE survives, POSTDIV1/2 and PLLFBDIV do not);
    reset value  differs only in bits the simulator's reset value sets
                 and the host's reset value lacks. Until P0.9 the host
                 assumed every SFR 0 at reset and this class explained
                 ADxCON.RPTCNT = 18 (the ATDF's initval); since P0.9
                 (27.09.2026) the host presets every SFR from that very
                 initval (gen_fake_sfr.py's sfr_reset[]), so this class
                 can only mean the ATDF and the simulator disagree - it
                 is a finding now, like DISAGREE;
    not written  the host trace wrote it but the simulator build does not
                 (sim_dma.c replaces dma.c; see tests/trace/README.md);
    DISAGREE     anything else - a finding.
  Independently of the class, every register's simulator reset value is
  compared with the ATDF's initval (the "initval" column: "same" or the
  ATDF's value) - that is the evidence the P0.9 preset rests on, and a
  register whose two values differ is a finding ("INITVAL") and exit 1.

Usage:
  python tools/check_fake_sfr.py                       both devices, static
  python tools/check_fake_sfr.py --mcu 33AK512MPS506   one device
  python tools/check_fake_sfr.py --sim-dump build/sfr_dump.txt --trace tests/trace/golden/boot.trace
"""
import argparse
import os
import re
import sys
import time
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_fake_sfr  # noqa: E402  (parse_gld, parse_header, DFP_DEFAULT)

ROOT = os.path.normpath(os.path.join(HERE, ".."))
MCUS_DEFAULT = ("33AK512MPS512", "33AK512MPS506")
M32 = 0xFFFFFFFF


# ---------------------------------------------------------------------------
# ATDF
# ---------------------------------------------------------------------------
class AtdfReg:
    """One register of one element of one instance.
    parts     the name prefix chain, e.g. ["AD3", "CH0"]
    grp       key of the (innermost) counted group this register sits in,
              or None; idx/start its element index and the group's
              start-index; elem_base the ATDF address of element `start`'s
              base; rel the register's offset inside its element; pitch
              the ATDF's element pitch (its `size`, or recovered - see
              resolve_atdf) or None when the ATDF does not state one."""
    __slots__ = ("name", "parts", "addr", "size", "initval", "rw", "fields", "path",
                 "grp", "idx", "start", "elem_base", "rel", "pitch")

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))

    def candidates(self):
        """The names the C header might use for this register: the chain
        plus the name (AD3CH0 + CON1), the name alone (GPIO/TRISA), the
        chain minus its last part, the chain alone (TIMER1/TMR/TMR ->
        TMR1), and the chain with the overlap merged (DACCTRL + CTRL1 ->
        DACCTRL1, INTCON + INTCON1 -> INTCON1)."""
        p = "".join(self.parts)
        c = {self.name, p + self.name}
        if self.parts:
            c.add("".join(self.parts[:-1]) + self.name)
            c.add(p)
            for k in range(min(len(p), len(self.name)), 0, -1):
                if p.endswith(self.name[:k]):
                    c.add(p + self.name[k:])
                    break
        return c


def _int(s, default=None):
    if s is None:
        return default
    return int(s, 0)


def _signed(v):
    return v - (1 << 32) if v >= (1 << 31) else v


def resolve_atdf(atdf_path):
    """Every register of the ATDF as an AtdfReg. Returns (regs, by_name,
    by_addr, notes); by_name maps every candidate name to the registers
    claiming it (normally one).

    Name chain: the instance's register-group name, then the instance's
    trailing digits if the group is not counted (ADC3/AD -> AD3, TIMER1/T
    -> T1, UART2/U -> U2), then the element index for a counted group
    (DMA/DMA[0..7] -> DMA0, CLOCK/CLK[4..17] -> CLK6), then any nested
    group the same way (AD3/CH[0..7] -> AD3CH0), then the register's own
    name (CON1 -> AD3CH0CON1). Some groups' registers already carry the
    full name (INTCON/INTCON1, IEC/IEC0, GPIO/TRISA, SCCP1's CCP/CCP1CON1),
    so candidates() also offers the bare name and the chain minus its last
    part; a header SFR that matches none is looked up by address and
    reported as "name" (the ATDF's own naming differs, e.g. the ADC
    accumulator ACC6 the header calls AD3CH6ACC).

    Address: instance offset + group offset + register offset, mod 2^32
    (the GPIO group's register offsets are negative 32-bit values - TRISA
    is at 0x32D4 + 0xFFFFCF34 = 0x208). A counted group WITH `size`
    strides by it (right for DMA: 44; wrong for the ADC's CH: 28 where
    the gld and .PIC say 32 - `size` is the sum of the registers, not the
    pitch, so static_check() treats the pitch as the ATDF's claim, not as
    fact). A counted group WITHOUT `size` (the clock's CLK, PLL, VCO, CM)
    is written differently by the pack's generator: the register offset
    is (count - start_index) * pitch + element_offset, the pitch nowhere
    stated. It is recovered from the group's smallest register offset
    (CLK: 0x50 = 10 * 8, PLL: 0xC = 1 * 12, CM: 0x90 = 3 * 0x30) and the
    address check against the gld is what proves the recovery right."""
    root = ET.parse(atdf_path).getroot()
    modules = {m.get("name"): m for m in root.find("modules").findall("module")}
    per = root.find("devices/device/peripherals")
    regs = []
    notes = []

    def mkreg(r, addr, parts, path, grp=None, idx=None, start=None, elem_base=None, e_base=None, pitch=None):
        fields = {bf.get("name"): (int(bf.get("mask"), 0), bf.get("rw")) for bf in r.findall("bitfield")}
        regs.append(AtdfReg(name=r.get("name"), parts=list(parts), addr=addr,
                            size=_int(r.get("size")), initval=_int(r.get("initval")),
                            rw=r.get("rw"), fields=fields, path=path + "/" + r.get("name"),
                            grp=grp, idx=idx, start=start, elem_base=elem_base,
                            rel=((addr - e_base) & M32) if grp else _int(r.get("offset")), pitch=pitch))

    def emit(defn, base, parts, path, grp, idx, start, elem_base, e_base, pitch):
        """`base`: absolute address the register offsets add to; `e_base`:
        the base of the counted element this sits in (None outside one),
        `elem_base` the same for the group's first element."""
        for r in defn.findall("register"):
            mkreg(r, (base + _int(r.get("offset"))) & M32, parts, path, grp, idx, start, elem_base, e_base, pitch)
        for sub in defn.findall("register-group"):
            place(sub, defn.get("_module"), base, parts, path, grp, idx, start, elem_base, e_base, pitch)

    def place(ref, module, base, parts, path, grp, idx, start, elem_base, e_base, pitch):
        defn = None
        for g in modules[module].findall("register-group"):
            if g.get("name") == ref.get("name-in-module"):
                defn = g
                break
        if defn is None:
            notes.append("%s refers to group %s that module %s does not define"
                         % (path, ref.get("name-in-module"), module))
            return
        defn.set("_module", module)
        off = _int(ref.get("offset"), 0)
        count = _int(ref.get("count"))
        gstart = _int(ref.get("start-index"), 0)
        size = _int(ref.get("size"))
        name = ref.get("name")
        if count is None:
            # The OPA module's AMP group (three instances, 8 bytes apart)
            # carries the (count - start) * pitch bias of the counted
            # groups below on its register offsets (CON1 at 0x10, CON2 at
            # 0x14, while the gld and the .PIC put AMP1CON1 at the
            # instance's own base 0x3B08): removed, noted, and the address
            # check then says whether the removal was right.
            offs = [_signed(_int(r.get("offset"))) for r in defn.findall("register")]
            lo = min(offs) if offs else 0
            if lo > 0:
                notes.append("%s/%s: register offsets biased by 0x%X on a non-counted group - removed"
                             % (path, name, lo))
                off -= lo
            emit(defn, (base + off) & M32, parts + [name], path + "/" + name,
                 grp, idx, start, elem_base, e_base, pitch)
            return
        gkey = path + "/" + name
        offs = [_int(r.get("offset")) for r in defn.findall("register")]
        lo = min(offs) if offs else 0
        if size is not None:
            gpitch, lo = size, 0
        else:
            n = count - gstart
            if n > 0 and lo > 0 and lo % n == 0:
                gpitch = lo // n
            else:
                gpitch = None
                notes.append("counted group %s: no size and no recoverable pitch (count %d, start %d, "
                             "min offset 0x%X) - only element %d is checked" % (gkey, count, gstart, lo, gstart))
                lo = 0
        # For the sizeless groups the ATDF's register offsets carry the
        # (count - start) * pitch bias `lo`, so the element base is shifted
        # by -lo and the register's offset inside its element is offset - lo.
        first = (base + off - lo) & M32
        for i in range(count):
            if gpitch is None and i:
                break
            eb = (first + i * (gpitch or 0)) & M32
            emit(defn, eb, parts + [name + str(gstart + i)], "%s[%d]" % (gkey, gstart + i),
                 gkey, gstart + i, gstart, first, eb, gpitch)

    def group_def(module, name_in_module):
        for g in modules[module].findall("register-group"):
            if g.get("name") == name_in_module:
                return g
        return None

    def inst_digits(inst):
        m = re.search(r"(\d+)$", inst.get("name"))
        return m.group(1) if m else ""

    for pm in per.findall("module"):
        module = pm.get("name")
        # Three shapes of "several instances refer to one group":
        #  - per instance at its own base (ADC1..5/AD, SPI1..4/SPI): the
        #    instance's digits go after the group name (AD3CON, SPI2BUF);
        #  - shared, every instance at the SAME offset (CMP_DAC1..8/DACCTRL
        #    at 0x1D40): one register set, no digits (DACCTRL1 = the group
        #    name merged with CTRL1, see candidates());
        #  - flattened (ccp, CLC): the group lists every instance's
        #    registers under full names (CCP1CON1 at 0x0 ... CCP8BUF at
        #    0x170, then MCCP9's CCP9CON1 restarting at 0x0) and every
        #    instance refers to that one group at its own base. Placed
        #    per instance, eight of the nine copies would land on other
        #    peripherals (SCCP4's copy of CCP8CON2 on TMR1). Recognised
        #    by the register names being <group><digits>..., the digit
        #    sets of registers and instances being equal; each instance's
        #    block is placed at that instance's base, its registers at
        #    their offsets relative to the block's first - so for these
        #    two modules the ATDF's inter-instance offsets are not what is
        #    checked, only the instance bases and the order and spacing
        #    inside an instance.
        refs = {}
        for inst in pm.findall("instance"):
            for ref in inst.findall("register-group"):
                refs.setdefault(ref.get("name-in-module"), []).append((inst, ref))
        skip = set()
        for gname, lst in refs.items():
            defn = group_def(module, gname)
            if len(lst) < 2 or defn is None or any(ref.get("count") for _i, ref in lst):
                continue
            offs = {_int(ref.get("offset"), 0) for _i, ref in lst}
            if len(offs) == 1:
                skip.add(gname)
                place(lst[0][1], module, 0, [], module, None, None, None, None, None, None)
                continue
            blocks = {}
            for r in defn.findall("register"):
                m = re.match(r"^%s(\d+)" % re.escape(gname), r.get("name"))
                if m:
                    blocks.setdefault(m.group(1), []).append(r)
            idig = {inst_digits(i): ref for i, ref in lst}
            if blocks and set(blocks) == set(idig) and sum(len(b) for b in blocks.values()) == len(defn.findall("register")):
                skip.add(gname)
                notes.append("module %s: group %s lists all %d instances' registers under full names "
                             "(MCCP9-style restart of the offsets handled per instance block)"
                             % (module, gname, len(lst)))
                for d, rs in blocks.items():
                    base = _int(idig[d].get("offset"), 0)
                    rel0 = min(_int(r.get("offset")) for r in rs)
                    for r in rs:
                        mkreg(r, (base + _int(r.get("offset")) - rel0) & M32, [], "%s/%s" % (module, gname))
        for inst in pm.findall("instance"):
            digits = inst_digits(inst)
            for ref in inst.findall("register-group"):
                if ref.get("name-in-module") in skip:
                    continue
                gname = ref.get("name")
                if ref.get("count") is None and digits and not gname[-1].isdigit():
                    ref = ET.Element("register-group", dict(ref.attrib))
                    ref.set("name", gname + digits)
                place(ref, module, 0, [], inst.get("name"), None, None, None, None, None, None)

    by_name = {}
    by_addr = {}
    for r in regs:
        for c in r.candidates():
            by_name.setdefault(c, []).append(r)
        by_addr.setdefault(r.addr, []).append(r)
    return regs, by_name, by_addr, notes


# ---------------------------------------------------------------------------
# Header + gld (what gen_fake_sfr.py consumed)
# ---------------------------------------------------------------------------
def load_header(dfp, mcu):
    hdr = os.path.join(dfp, "h", "p%s.h" % mcu)
    gld = os.path.join(dfp, "gld", "p%s.gld" % mcu)
    gaddr = gen_fake_sfr.parse_gld(gld)
    with open(hdr, encoding="latin-1") as f:
        regs, _typedefs, maskdefs = gen_fake_sfr.parse_header(f.read().splitlines())

    def address_of(name):
        for key in (name, "_" + name):
            if key in gaddr:
                return gaddr[key]
        return None

    fields_of = {}
    for (r, f), md in maskdefs.items():
        if "MASK" in md:
            fields_of.setdefault(r, {})[f] = md["MASK"]
    sfrs = {}   # name -> (addr, {field: mask})
    for n, (kind, _typ) in regs.items():
        if kind != "reg":
            continue
        ad = address_of(n)
        if ad is not None:
            sfrs[n] = (ad, fields_of.get(n, {}))
    return sfrs


def match_sfr(name, ad, by_name, by_addr):
    """The ATDF register a header SFR (name, gld address) stands for:
    by name first (the candidate names resolve_atdf() derives; among
    several claimants the one at the same address), else by address
    alone. Returns (reg, "name" | "addr") or (None, None). The one rule
    both the static check below and gen_fake_sfr.py's reset-value table
    (P0.9) use, so a register's initval comes from exactly the ATDF entry
    whose address and fields the check verified."""
    cands = by_name.get(name, [])
    if len(cands) == 1:
        return cands[0], "name"
    if cands:
        return next((c for c in cands if c.addr == ad), cands[0]), "name"
    at = by_addr.get(ad, [])
    if at:
        return at[0], "addr"
    return None, None


# ---------------------------------------------------------------------------
# Static check
# ---------------------------------------------------------------------------
def static_check(mcu, dfp, atdf_dir, verbose, golden_names=()):
    t0 = time.time()
    atdf = os.path.join(atdf_dir, "dsPIC%s.atdf" % mcu)
    if not os.path.exists(atdf):
        print("check_fake_sfr: no ATDF at %s" % atdf)
        return 1
    _regs, by_name, by_addr, notes = resolve_atdf(atdf)
    sfrs = load_header(dfp, mcu)

    errors = []          # hard: exit 1
    verified = set()     # found by name, address exact, every header field agrees
    addr_ok = 0
    none = []            # header SFRs the ATDF has no register for (by name AND address)
    name_only = []       # same address, different name
    size2 = []           # ATDF size != 4, header uint32_t
    fields_ok = 0
    hdr_only = []        # (reg, field, mask) the header has, the ATDF not
    atdf_only = []       # (reg, field, mask) the ATDF has, the header not
    checked = 0
    pitch_obs = {}       # counted group -> {implied pitch: [regs]} for later elements
    pitch_first_bad = {}  # counted group -> first element mismatches

    for name in sorted(sfrs, key=lambda n: sfrs[n][0]):
        ad, hfields = sfrs[name]
        reg, how = match_sfr(name, ad, by_name, by_addr)
        if reg is None:
            none.append((name, ad))
            continue
        if how == "addr":
            name_only.append((name, ad, "".join(reg.parts) + reg.name))
        checked += 1
        full = how == "name"
        if reg.addr == ad:
            addr_ok += 1
        elif reg.grp is not None and reg.idx != reg.start:
            full = False
            n = reg.idx - reg.start
            d = ad - reg.elem_base - reg.rel
            if d % n == 0 and d // n > 0:
                pitch_obs.setdefault(reg.grp, {}).setdefault(d // n, []).append(name)
            else:
                errors.append("%s: gld address 0x%06X, ATDF 0x%06X (%s)" % (name, ad, reg.addr, reg.path))
        else:
            full = False
            errors.append("%s: gld address 0x%06X, ATDF 0x%06X (%s)" % (name, ad, reg.addr, reg.path))
            if reg.grp is not None:
                pitch_first_bad[reg.grp] = True
        if reg.size != 4:
            size2.append((name, reg.size))
        for f, mask in sorted(hfields.items()):
            if f in reg.fields:
                amask = reg.fields[f][0]
                if amask == mask:
                    fields_ok += 1
                else:
                    full = False
                    errors.append("%s.%s: header mask 0x%08X, ATDF 0x%08X" % (name, f, mask, amask))
            else:
                full = False
                hdr_only.append((name, f, mask))
        for f, (amask, _rw) in reg.fields.items():
            if f not in hfields:
                atdf_only.append((name, f, amask))
        if full:
            verified.add(name)

    pitch = []           # (group, gld pitch, ATDF pitch, n regs)
    for grp, obs in pitch_obs.items():
        if len(obs) == 1 and grp not in pitch_first_bad:
            p, names = next(iter(obs.items()))
            ap = next((r for r in by_addr.get(sfrs[names[0]][0], []) if r.grp == grp), None)
            pitch.append((grp, p, ap.pitch if ap else None, len(names)))
        else:
            for p, names in obs.items():
                for n in names:
                    errors.append("%s: element pitch inconsistent inside %s (implied %d)" % (n, grp, p))
    dt = time.time() - t0
    print("check_fake_sfr %s: %d header SFRs, %d found in the ATDF (%d by address only), "
          "%d not in the ATDF; addresses: %d agree, %d counted groups with a pitch the ATDF does not state; "
          "fields: %d agree, %d header-only, %d ATDF-only; %d registers the ATDF sizes below 4 bytes; "
          "%d ATDF notes; %d ERRORS; %.1f s"
          % (mcu, len(sfrs), checked, len(name_only), len(none), addr_ok, len(pitch), fields_ok,
             len(hdr_only), len(atdf_only), len(size2), len(notes), len(errors), dt))
    for e in errors:
        print("  ERROR " + e)
    bykind = {}
    for g, p, ap, n in pitch:
        bykind.setdefault((p, ap), []).append((g, n))
    for (p, ap), lst in sorted(bykind.items()):
        print("  pitch gld %d, ATDF size %s: %s (%d registers in elements after the first)"
              % (p, ap, ", ".join(g for g, _n in lst), sum(n for _g, n in lst)))
    if golden_names:
        gn = [n for n in golden_names if n in sfrs]
        weak = [n for n in gn if n not in verified]
        print("  golden traces write %d registers this device has: %d verified by name, address and every "
              "header field%s" % (len(gn), len(gn) - len(weak),
                                  ("; NOT fully: " + ", ".join(sorted(weak))) if weak else ""))
    if verbose:
        for p in notes:
            print("  note  " + p)
        for n, ad, an in name_only:
            print("  name  %s @0x%06X: the ATDF calls it %s" % (n, ad, an))
        for n, ad in none:
            print("  none  %s @0x%06X: not in the ATDF" % (n, ad))
        for n, sz in size2:
            print("  size  %s: ATDF size %d" % (n, sz))
        for n, f, m in hdr_only:
            print("  hdr   %s.%s (0x%08X): header only" % (n, f, m))
        for n, f, m in atdf_only:
            print("  atdf  %s.%s (0x%08X): ATDF only" % (n, f, m))
    return 1 if errors else 0


# ---------------------------------------------------------------------------
# Dynamic check
# ---------------------------------------------------------------------------
RE_W = re.compile(r"^W\s+(\w+)\s+(\S+)\s+->\s+(\S+)")
RE_SFRREF = re.compile(r"^&(\w+)\((0x[0-9A-Fa-f]+)\)$")


def trace_end_state(path):
    """name -> (final value or symbolic string, first old value), in
    first-write order."""
    end = {}
    with open(path, encoding="ascii", errors="replace") as f:
        for ln in f:
            m = RE_W.match(ln)
            if m:
                name, old, new = m.groups()
                end.setdefault(name, [old, None])[1] = new
    return {n: (v[1], v[0]) for n, v in end.items()}


def parse_value(s):
    m = RE_SFRREF.match(s)
    if m:
        return int(m.group(2), 16)
    if s.startswith("&"):
        return None                     # a RAM region: a host address
    return int(s, 0)


def load_dump(path):
    """sim_trap.py --dump-sfr output: 'NAME reset=<v> after=<v>' lines."""
    d = {}
    with open(path, encoding="ascii", errors="replace") as f:
        for ln in f:
            m = re.match(r"^(\w+)\s+reset=(\S+)\s+after=(\S+)", ln)
            if m:
                d[m.group(1)] = (m.group(2), m.group(3))
    return d


def as_int(s):
    try:
        return int(s, 0)
    except ValueError:
        return None


def dynamic_check(mcu, atdf_dir, dump_path, trace_path, not_written):
    atdf = os.path.join(atdf_dir, "dsPIC%s.atdf" % mcu)
    _regs, by_name, _by_addr, _notes = resolve_atdf(atdf)
    end = trace_end_state(trace_path)
    dump = load_dump(dump_path)
    counts = {"agree": 0, "status": 0, "not stored": 0, "reset value": 0, "not written": 0,
              "DISAGREE": 0, "unread": 0, "INITVAL": 0}
    rows = []
    for name, (final, _first) in end.items():
        host = parse_value(final)
        cands = by_name.get(name, [])
        reg = cands[0] if len(cands) == 1 else None
        if name not in dump:
            counts["unread"] += 1
            rows.append((name, "unread", "-", "-", "-", final, "MDB printed nothing for it"))
            continue
        reset_s, after_s = dump[name]
        reset, after = as_int(reset_s), as_int(after_s)
        if after is None or reset is None:
            counts["unread"] += 1
            rows.append((name, "unread", reset_s, after_s, "-", final, "MDB value is not a number"))
            continue
        why = ""
        if host is None:
            # a RAM address the host cannot state: judge only whether the
            # simulator wrote anything at all
            if name in not_written:
                cls, why = "not written", "sim build has no dma.c (sim_dma.c)"
            elif after == reset:
                cls, why = "not stored", "host wrote a RAM address, simulator unchanged"
            else:
                cls, why = "agree", "host wrote a RAM address, simulator holds one"
        elif after == host:
            cls = "agree"
        elif name in not_written:
            cls, why = "not written", "sim build has no dma.c (sim_dma.c)"
        elif after == reset:
            cls, why = "not stored", "simulator still at its reset value"
        else:
            diff = after ^ host
            names = []
            explained = 0          # read-only fields, self-clearing switch requests
            unexplained = []
            if reg:
                for f, (mask, rw) in sorted(reg.fields.items(), key=lambda kv: kv[1][0]):
                    if diff & mask:
                        names.append("%s(%s)" % (f, rw or "?"))
                        # *SWEN: OSWEN/DIVSWEN/FOUTSWEN/PLLSWEN, the clock
                        # switch requests hardware clears when the switch is
                        # done - the host model clears them, the simulator
                        # (no clock model) leaves them set
                        if rw == "R" or f.endswith("SWEN"):
                            explained |= (diff & mask)
                        else:
                            unexplained.append(f)
            rest = diff & ~explained
            if rest == 0 and names:
                cls = "status"
                why = "differs in " + ", ".join(names)
            elif (after & rest) == 0:
                # the write reached the register (it left its reset value)
                # but the simulator reads the written fields back as 0
                cls = "not stored"
                why = "simulator zeroes the written fields %s (reset 0x%08X, after 0x%08X)" % (
                    ", ".join(unexplained) or "0x%08X" % rest, reset, after)
            elif (rest & ~reset) == 0 and (after & rest) == (reset & rest):
                # bits set in the simulator's reset value that the host's
                # reset value lacks and that the firmware never wrote.
                # Before P0.9 (host reset all 0) this was AD3CON.RPTCNT;
                # since P0.9 the host starts from the ATDF's initval, so
                # this can only be an ATDF/simulator disagreement
                cls = "reset value"
                why = "simulator reset value 0x%08X keeps %s; the host's reset value (ATDF initval) lacks it%s" % (
                    reset, ", ".join(unexplained) or "0x%08X" % rest,
                    "; read-only " + ", ".join(n for n in names if "(R)" in n) if explained else "")
            else:
                cls = "DISAGREE"
                why = "differs in " + (", ".join(names) if names else "bits 0x%08X" % diff)
                why += " - unexplained bits 0x%08X" % rest
        counts[cls] += 1
        # the simulator's reset value against the ATDF's initval - the
        # evidence the P0.9 preset (gen_fake_sfr.py's sfr_reset[]) rests
        # on; a difference is a finding of its own, whatever the class
        iv = "-" if reg is None or reg.initval is None else (
            "same" if reg.initval == reset else "0x%08X" % reg.initval)
        if iv not in ("-", "same"):
            counts["INITVAL"] += 1
            why = ("ATDF initval %s != simulator reset 0x%08X; " % (iv, reset)) + why
        rows.append((name, cls, "0x%08X" % reset, "0x%08X" % after, iv, final, why))
    w = max(len(r[0]) for r in rows) if rows else 8
    print("%-*s  %-11s  %-10s  %-10s  %-10s  %-22s  %s"
          % (w, "register", "class", "sim reset", "sim after", "initval", "host end", "note"))
    for r in rows:
        print("%-*s  %-11s  %-10s  %-10s  %-10s  %-22s  %s" % ((w,) + r))
    print("summary: " + ", ".join("%s %d" % kv for kv in counts.items()))
    return 1 if (counts["DISAGREE"] or counts["reset value"] or counts["INITVAL"]) else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mcu", action="append", help="device(s); default: both boards' MCUs")
    ap.add_argument("--dfp", default=gen_fake_sfr.DFP_DEFAULT, help="the pack's xc16/support/dsPIC33A directory")
    ap.add_argument("--atdf-dir", default=None, help="the pack's atdf directory (default: derived from --dfp)")
    ap.add_argument("--sim-dump", help="sim_trap.py --dump-sfr output (dynamic check)")
    ap.add_argument("--trace", default=os.path.join(ROOT, "tests", "trace", "golden", "boot.trace"),
                    help="golden trace whose end state the dump is compared with")
    ap.add_argument("--not-written", default="DMACON,DMALOW,DMAHIGH,DMA0CH,DMA0SEL,DMA0SRC,DMA0DST,DMA0CNT,IEC2",
                    help="registers the simulator build never writes (dma.c is replaced by sim_dma.c)")
    ap.add_argument("--golden-dir", default=os.path.join(ROOT, "tests", "trace", "golden"),
                    help="static check: also say which registers the golden traces write are not fully "
                         "verified ('' to skip)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    atdf_dir = a.atdf_dir or gen_fake_sfr.atdf_dir_default(a.dfp)
    mcus = a.mcu or list(MCUS_DEFAULT)
    if a.sim_dump:
        return dynamic_check(mcus[0], atdf_dir, a.sim_dump, a.trace,
                             set(a.not_written.split(",")) if a.not_written else set())
    golden = set()
    if a.golden_dir and os.path.isdir(a.golden_dir):
        for fn in os.listdir(a.golden_dir):
            if fn.endswith(".trace"):
                golden.update(trace_end_state(os.path.join(a.golden_dir, fn)))
    rc = 0
    for mcu in mcus:
        rc |= static_check(mcu, a.dfp, atdf_dir, a.verbose, sorted(golden))
    return rc


if __name__ == "__main__":
    sys.exit(main())
