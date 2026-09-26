#!/usr/bin/env python3
"""fncmp.py - compare two dsPIC33A ELF files function by function.

    python tools\\fncmp.py A.elf B.elf [--show N] [--func NAME]
    python tools\\fncmp.py A.elf --count NAME
    python tools\\fncmp.py A.elf --indirect NAME
    python tools\\fncmp.py A.elf --list

Disassembles both ELFs with the toolchain's own objdump (`xc-dsc-objdump -d`, found
next to the `xc-dsc-gcc` that tools\\build.bat uses - both paths are read from
build.bat, `--objdump`/`--dfp` override them), cuts the disassembly into functions,
normalises what legitimately differs between two builds of the same code, and
reports the functions whose normalised bodies differ. Exit code 0 = no difference,
1 = differences found (or, with --indirect, an indirect call found), 2 = error.

Made for P0.6 of docs/IMPLEMENTATION-PLAN.md: the proof that a pure move (P1)
changed no function, and that an ISR still has no indirect call (P8.1).

What a "function" is
--------------------
A FUNC symbol from the ELF symbol table (`xc-dsc-readelf -s`), never objdump's
`<label>:` headers: objdump also prints a header for every local label (`.L29`,
`L1^B1`, `.LC5`, `.SS176` switch tables), and those would split a function into
pieces whose names change with every insertion. A function's extent is its symbol
size; a FUNC with size 0 (assembly-written library entries such as `__reset` or
`___strtol_internal`) runs to the next FUNC symbol. Instructions outside every
FUNC (interrupt vector table, `.dinit`, const tables disassembled as garbage) are
ignored: this tool compares code, not data.

A static function gets its name qualified with the source file it lives in
(`clock.c:_pll1_out_hz`), taken from the FILE symbol that precedes it in the symbol
table's local block. Two files may therefore have a static of the same name without
clashing. `--func`/`--count`/`--indirect` accept the qualified name, the assembler name or
the C name: the assembler symbol carries a leading underscore, so `_DMA0Interrupt`
in C is `__DMA0Interrupt` here, and both spellings find it.

Normalisation rules (what is and is not touched)
------------------------------------------------
Only addresses that move when a function or a variable moves are replaced. In
detail, for every hex operand token:

  KEPT VERBATIM
  - register operands, condition codes, indirect forms (`[w0+4]`, `[--w15]`), and
    every numeric operand that is not a hex address token;
  - immediates and direct addresses below 0x4000: this is the SFR space (the
    linker script's `data` region starts at 0x4000 - see the MEMORY block of
    p33AK512MPS512.gld). A register address is a fact of the silicon; a change
    there is a real change and must show;
  - immediates in the RAM range 0x4000..0x13fff that no data symbol covers: they
    are constants (0x8000, 0xffff, 0x13880 = 80000, ...); and, whatever they
    cover, immediates of every mnemonic other than `mov.sl`/`mov.l`, because the
    compiler loads an address only with those two forms (`movs.l #0x4c4b` is the
    constant 19531 even when a buffer happens to sit there - see
    ADDRESS_IMMEDIATE_MNEMONICS for the census behind this);
  - the mnemonic itself, in full.

  REPLACED
  - the address column at the left of every line (dropped);
  - objdump's own `<label>` annotations (dropped, then re-derived below);
  - a program-memory address (>= 0x800000) inside the function being compared:
    `+0x<offset from function start>`. Covers branches, `dtb` table branches, the
    `bra` jump tables of a switch, and a recursive call;
  - a program-memory address inside another FUNC: `<name>` (or `<name+off>`);
  - an address inside an OBJECT symbol (a variable, a const table, RAM or flash):
    `<name+off>`; an exact hit on a GLOBAL/WEAK NOTYPE symbol (e.g. `__SP_init`
    for the stack start) is `<name>`;
  - a program-memory address that no symbol covers - anonymous const data, mostly
    string literals - and a RAM address that is exactly a `.LC` label (the
    compiler puts some literals into `.data`): `<str:"first chars"#crc32>` when
    the bytes there are a printable NUL-terminated string (or a printable run of
    8 or more), else `<data:crc32 of 16 bytes>`. Anchoring on the content rather
    than on the address or on the compiler's `.LC5` label keeps the reference
    stable when the literal moves and still reports a changed literal. The
    16-byte window for non-string data is a guess at its size.
    `--ignore-strings` reduces every string label to `<str>`: needed when the two
    builds were made at different times, because the BUILD_ID line carries
    `__DATE__`/`__TIME__` and the function that prints it (`chaintest.c:_stage0`)
    would otherwise differ between any two builds.
  - the same rules apply to `.pword` data words that fall inside a function.

Known blind spots, accepted on purpose:
  - a `mov.sl`/`mov.l` constant in 0x4000..0x13fff that happens to fall inside a
    variable is shown as `<var+off>`; it stays stable unless that variable moves
    relative to the constant. None in today's builds; a real change still differs.
  - statics of the same name in the same file cannot be told apart (C forbids it
    at file scope; two function-local statics `static int use;` in one file are
    both `<cli.c:_use>` here, because GCC's `_use.27320` counter suffix, which
    changes with any edit or a different device header, is stripped).
  - data contents are not compared, only the code that references them.

Indirect calls
--------------
Taken from this firmware's own disassembly, not from the manual: an indirect call
is `call wN` (the command table in cli.c/cmd_parser.c dispatches through
`call w0` etc.); a computed jump is `bra wN` (the compiler's switch tables:
`.SS176: bra w0` followed by a table of `bra` instructions). `--indirect` reports
both kinds separately - `rcall`/`goto`/`jump` with a register or `[...]` operand
would be reported too if they ever appeared - and exits 1 if the function has an
indirect CALL (P8.1: "the ISR has no indirect call"). `--count` prints the
instruction count of the function (every mnemonic line, `neop` included, shown
separately as well).
"""

import argparse
import binascii
import difflib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Memory map facts, from the pack's p33AK512MPS512.gld MEMORY block (identical on
# the MPS506): data region ORIGIN 0x4000 LENGTH 0x10000, program memory from
# 0x800000. Everything below RAM_LO is SFR space.
RAM_LO = 0x4000
RAM_HI = 0x14000
PROG_LO = 0x800000

HEX_TOKEN = re.compile(r"(#?)0x([0-9a-fA-F]+)")
ANNOTATION = re.compile(r"\s*<[^>]*>")
INSN_LINE = re.compile(r"^\s*([0-9a-fA-F]+):\t[0-9a-f ]+\t(\S+)\s*(.*)$")
DIS_HEADER = re.compile(r"^([0-9a-fA-F]+) <(.*)>:$")
CONTENT_LINE = re.compile(r"^ ([0-9a-fA-F]+) ((?:[0-9a-fA-F]{2,8} ?)+)")
SYM_LINE = re.compile(
    r"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+(\w+)\s+(\w+)\s+(\w+)\s+(\S+)\s+(.*)$"
)
CONTROL_MNEMONICS = ("call", "rcall", "goto", "bra", "jump")
# The only instructions whose RAM-range immediate is an address in this firmware
# (census over the hw/sim/nano ELFs, 27.09.2026: every immediate that hit a data
# symbol's start was a `mov.sl` - 134/69/134 of them - or the crt's `mov.l
# #<__SP_init>, w15`; the `movs.l`/`movs.w`/`cp.l`/`and.l`/`add.l` immediates in
# that range were constants such as 0x4c4b = 19531 and 0x4e20 = 20000, and two of
# them happened to land inside a large buffer). Immediates of other mnemonics in
# the RAM range are therefore kept verbatim.
ADDRESS_IMMEDIATE_MNEMONICS = ("mov.sl", "mov.l")


class Error(Exception):
    pass


# --------------------------------------------------------------------- toolchain

def toolchain_from_build_bat():
    """XC_DSC and DFP as tools\\build.bat sets them - one source of truth."""
    xc = dfp = None
    try:
        with open(os.path.join(HERE, "build.bat"), encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.match(r"\s*set\s+(XC_DSC|DFP)=(.*?)\s*$", line, re.I)
                if m:
                    if m.group(1).upper() == "XC_DSC":
                        xc = m.group(2)
                    else:
                        dfp = m.group(2)
    except OSError:
        pass
    return xc, dfp


def run(cmd):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                           errors="replace")
    except OSError as e:
        raise Error("cannot run %s: %s" % (cmd[0], e))
    if p.returncode != 0:
        raise Error("%s failed (%d): %s" % (os.path.basename(cmd[0]), p.returncode,
                                             (p.stderr or p.stdout).strip()[:400]))
    return p.stdout


# ----------------------------------------------------------------------- symbols

class Symbol:
    __slots__ = ("addr", "size", "kind", "bind", "name", "end")

    def __init__(self, addr, size, kind, bind, name):
        self.addr, self.size, self.kind, self.bind, self.name = addr, size, kind, bind, name
        self.end = addr + size


def read_symbols(readelf, elf):
    """FUNC symbols (statics qualified with their file), data symbols, exact-hit symbols."""
    funcs, objects, exact, lc = [], [], {}, set()
    cur_file = None
    for line in run([readelf, "-sW", elf]).splitlines():
        m = SYM_LINE.match(line)
        if not m:
            continue
        addr, size, kind, bind, ndx, name = (int(m.group(1), 16), int(m.group(2)),
                                             m.group(3), m.group(4), m.group(6),
                                             m.group(7).strip())
        if kind == "FILE":
            cur_file = os.path.basename(name.replace("//", "/").replace("\\", "/"))
            continue
        if kind == "SECTION" or not name:
            continue
        if bind == "GLOBAL" or bind == "WEAK":
            cur_file_for = None
        else:
            cur_file_for = cur_file
        if kind == "FUNC":
            qname = "%s:%s" % (cur_file_for, name) if cur_file_for else name
            funcs.append(Symbol(addr, size, kind, bind, qname))
        elif kind == "OBJECT":
            # a function-local static is `_name.NNNN`; NNNN is a compiler counter
            # that changes with any edit in the file - drop it (see the header)
            name = re.sub(r"\.\d+$", "", name)
            qname = "%s:%s" % (cur_file_for, name) if cur_file_for else name
            objects.append(Symbol(addr, size, kind, bind, qname))
        elif kind == "NOTYPE" and bind in ("GLOBAL", "WEAK") and ndx != "UND":
            exact.setdefault(addr, []).append(name)
        elif kind == "NOTYPE" and bind == "LOCAL" and name.startswith(".LC"):
            lc.add(addr)          # a string literal - some land in .data (RAM)
    if not funcs:
        raise Error("no FUNC symbols in %s" % elf)
    # zero-size functions run to the next FUNC symbol
    funcs.sort(key=lambda s: (s.addr, -s.size, s.name))
    for i, s in enumerate(funcs):
        if s.size == 0:
            nxt = next((t.addr for t in funcs[i + 1:] if t.addr > s.addr), s.addr)
            s.end = nxt
    objects.sort(key=lambda s: (s.addr, -s.size, s.name))
    return funcs, objects, exact, lc


def dedupe_aliases(funcs):
    """Several FUNC symbols at one address (e.g. __reset/__resetPRI): keep one per
    address as the function; the others still resolve as names via `lookup`."""
    seen, out = set(), []
    for s in funcs:
        if s.addr in seen:
            continue
        seen.add(s.addr)
        out.append(s)
    return out


# --------------------------------------------------------------------- contents

def read_contents(objdump, dfp, elf):
    """address -> bytes for every loaded section (objdump -s), for literal labels."""
    chunks = []  # (start, bytearray)
    for line in run([objdump, "-mdfp=" + dfp, "-s", elf]).splitlines():
        m = CONTENT_LINE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        data = bytes.fromhex(m.group(2).replace(" ", ""))
        if chunks and chunks[-1][0] + len(chunks[-1][1]) == addr:
            chunks[-1][1].extend(data)
        else:
            chunks.append((addr, bytearray(data)))
    return chunks


STRINGS_GENERIC = False     # --ignore-strings: every string literal is just <str>


def _printable(b):
    # text, plus the control characters a console string carries: \a \b \t \n
    # \v \f \r and ESC (ANSI sequences in cmd_parser.c)
    return 32 <= b < 127 or 7 <= b <= 13 or b == 27


def literal_label(chunks, addr):
    for start, data in chunks:
        if start <= addr < start + len(data):
            off = addr - start
            window = bytes(data[off:off + 1024])
            run = 0
            while run < len(window) and _printable(window[run]):
                run += 1
            # a string: printable up to a NUL, or a long printable run (the build
            # line is over 100 characters and may cross the window)
            if run > 0 and (run < len(window) and window[run] == 0 or run >= 8):
                if STRINGS_GENERIC:
                    return "<str>"
                text = window[:run].decode("ascii")
                crc = binascii.crc32(text.encode()) & 0xFFFFFFFF
                short = text[:16] + ("..." if len(text) > 16 else "")
                short = short.replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t")
                return '<str:"%s"#%08x>' % (short, crc)
            # not text: hash up to a NUL within 64 bytes, else 16 bytes
            blob = bytes(data[off:off + 64])
            nul = blob.find(b"\0")
            blob = blob[:nul + 1] if 0 <= nul < 16 else blob[:16]
            crc = binascii.crc32(blob) & 0xFFFFFFFF
            return "<data:%08x>" % crc
    return None


# ---------------------------------------------------------------------- resolve

class Image:
    """One ELF: its functions, their instruction lines, and an address resolver."""

    def __init__(self, path, objdump, readelf, dfp):
        self.path = path
        funcs, self.objects, self.exact, self.lc = read_symbols(readelf, path)
        self.all_funcs = funcs
        self.funcs = dedupe_aliases(funcs)
        self.by_name = {}
        for s in self.funcs:
            self.by_name[s.name] = s
        self.chunks = read_contents(objdump, dfp, path)
        self.body = {s.name: [] for s in self.funcs}  # raw (mnemonic, operands)
        self._split(run([objdump, "-mdfp=" + dfp, "-d", path]))

    def _split(self, text):
        starts = [s.addr for s in self.funcs]
        import bisect
        for line in text.splitlines():
            m = INSN_LINE.match(line)
            if not m:
                continue
            addr = int(m.group(1), 16)
            i = bisect.bisect_right(starts, addr) - 1
            if i < 0:
                continue
            f = self.funcs[i]
            if not (f.addr <= addr < f.end):
                continue
            self.body[f.name].append((m.group(2), m.group(3).rstrip()))

    def find(self, name):
        """By qualified name, bare assembler name, or C name (assembler = '_' + C)."""
        for cand in (name, "_" + name):
            if cand in self.by_name:
                return self.by_name[cand]
        hits = [s for s in self.funcs
                if s.name.endswith(":" + name) or s.name.endswith(":_" + name)]
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1:
            raise Error("%s is ambiguous in %s: %s" % (name, self.path,
                                                       ", ".join(h.name for h in hits)))
        return None

    def _func_at(self, addr):
        for s in self.all_funcs:
            if s.addr <= addr < s.end:
                return s
        return None

    def _object_at(self, addr):
        for s in self.objects:
            if s.addr <= addr < max(s.end, s.addr + 1):
                return s
        return None

    def resolve(self, value, immediate, cur, mnem=""):
        """The normalised text for one hex operand, or None to keep it verbatim."""
        if value < RAM_LO:
            return None                     # SFR space, or a small constant
        if immediate and RAM_LO <= value < RAM_HI and mnem not in ADDRESS_IMMEDIATE_MNEMONICS:
            return None                     # a constant, whatever it lands on
        if RAM_LO <= value < RAM_HI or value >= PROG_LO:
            if cur.addr <= value < cur.end:
                return "+0x%x" % (value - cur.addr)
            f = self._func_at(value)
            if f is not None:
                off = value - f.addr
                return "<%s>" % f.name if off == 0 else "<%s+0x%x>" % (f.name, off)
            o = self._object_at(value)
            if o is not None:
                off = value - o.addr
                return "<%s>" % o.name if off == 0 else "<%s+0x%x>" % (o.name, off)
            if value in self.exact:
                return "<%s>" % sorted(self.exact[value])[0]
            if value >= PROG_LO or value in self.lc:
                return literal_label(self.chunks, value)
            return None                     # RAM-range constant, no symbol: keep
        return None

    def normalised(self, name):
        cur = self.find(name)
        out = []
        for mnem, ops in self.body[cur.name]:
            ops = ANNOTATION.sub("", ops)

            def repl(m):
                imm = m.group(1) == "#"
                r = self.resolve(int(m.group(2), 16), imm, cur, mnem)
                if r is None:
                    return m.group(0)
                return ("#" if imm else "") + r

            out.append((mnem + " " + HEX_TOKEN.sub(repl, ops)).rstrip())
        return out

    def indirect(self, name):
        cur = self.find(name)
        calls, jumps = [], []
        for i, (mnem, ops) in enumerate(self.body[cur.name]):
            base = mnem.split(".")[0]
            if base not in CONTROL_MNEMONICS:
                continue
            target = ops.split(",")[-1].strip()
            if re.match(r"^(w\d+|\[.*\])$", target):
                (calls if base in ("call", "rcall") else jumps).append(
                    "%d: %s %s" % (i, mnem, ops))
        return calls, jumps


# -------------------------------------------------------------------------- main

def compare(a, b, only=None, show=20, out=sys.stdout):
    names_a = {s.name for s in a.funcs}
    names_b = {s.name for s in b.funcs}
    if only:
        fa, fb = a.find(only), b.find(only)
        if fa is None and fb is None:
            raise Error("no function %s in either ELF" % only)
        names_a = {fa.name} if fa else set()
        names_b = {fb.name} if fb else set()
    only_a = sorted(names_a - names_b)
    only_b = sorted(names_b - names_a)
    differ, same = [], 0
    for n in sorted(names_a & names_b):
        la, lb = a.normalised(n), b.normalised(n)
        if la == lb:
            same += 1
            continue
        differ.append(n)
        out.write("--- %s (%s: %d insns, %s: %d insns)\n"
                  % (n, os.path.basename(a.path), len(la), os.path.basename(b.path), len(lb)))
        lines = list(difflib.unified_diff(la, lb, "A", "B", lineterm="", n=1))[2:]
        for line in lines[:show]:
            out.write("    " + line + "\n")
        if len(lines) > show:
            out.write("    ... (%d more diff lines, --show N for more)\n" % (len(lines) - show))
    for n in only_a:
        out.write("only in A: %s\n" % n)
    for n in only_b:
        out.write("only in B: %s\n" % n)
    out.write("fncmp: %d functions in A, %d in B; %d same, %d differ, %d only in A, "
              "%d only in B\n" % (len(names_a), len(names_b), same, len(differ),
                                  len(only_a), len(only_b)))
    return 0 if not (differ or only_a or only_b) else 1


def main(argv):
    ap = argparse.ArgumentParser(description="compare two dsPIC33A ELFs per function")
    ap.add_argument("elf", nargs="+", help="one ELF (--count/--indirect/--list) or two")
    ap.add_argument("--show", type=int, default=20, help="diff lines per function (20)")
    ap.add_argument("--func", help="compare only this function")
    ap.add_argument("--count", metavar="NAME", help="print the instruction count of NAME")
    ap.add_argument("--indirect", metavar="NAME",
                    help="report indirect calls/jumps in NAME; exit 1 if it has an indirect call")
    ap.add_argument("--list", action="store_true", help="list the functions with their counts")
    ap.add_argument("--dump", metavar="NAME", help="print the normalised body of NAME")
    ap.add_argument("--ignore-strings", action="store_true",
                    help="label every string literal <str> without its content "
                         "(for the BUILD_ID line, which differs between any two builds)")
    ap.add_argument("--objdump", help="path to xc-dsc-objdump (default: from build.bat)")
    ap.add_argument("--dfp", help="DFP xc16 directory for -mdfp (default: from build.bat)")
    args = ap.parse_args(argv)
    global STRINGS_GENERIC
    STRINGS_GENERIC = args.ignore_strings

    xc, dfp = toolchain_from_build_bat()
    objdump = args.objdump or (os.path.join(xc, "bin", "xc-dsc-objdump.exe") if xc else None)
    dfp = args.dfp or dfp
    if not objdump or not os.path.exists(objdump):
        raise Error("xc-dsc-objdump not found (%s); use --objdump" % objdump)
    if not dfp or not os.path.isdir(dfp):
        raise Error("DFP directory not found (%s); use --dfp" % dfp)
    readelf = os.path.join(os.path.dirname(objdump), "xc-dsc-readelf.exe")
    if not os.path.exists(readelf):
        raise Error("xc-dsc-readelf not found next to objdump")
    for e in args.elf:
        if not os.path.exists(e):
            raise Error("no such file: %s" % e)

    single = args.count or args.indirect or args.list or args.dump
    if single:
        rc = 0
        for path in args.elf:
            img = Image(path, objdump, readelf, dfp)
            if args.list:
                for s in img.funcs:
                    print("%6d  %s" % (len(img.body[s.name]), s.name))
            for name in (args.count, args.dump):
                if not name:
                    continue
                f = img.find(name)
                if f is None:
                    raise Error("no function %s in %s" % (name, path))
                if name == args.dump:
                    print("\n".join(img.normalised(f.name)))
                else:
                    body = img.body[f.name]
                    neop = sum(1 for m, _ in body if m == "neop")
                    print("%s: %s: %d instructions (%d of them neop), %d bytes"
                          % (os.path.basename(path), f.name, len(body), neop, f.end - f.addr))
            if args.indirect:
                f = img.find(args.indirect)
                if f is None:
                    raise Error("no function %s in %s" % (args.indirect, path))
                calls, jumps = img.indirect(f.name)
                print("%s: %s: %d indirect call(s), %d computed jump(s)"
                      % (os.path.basename(path), f.name, len(calls), len(jumps)))
                for line in calls:
                    print("    call   " + line)
                for line in jumps:
                    print("    jump   " + line)
                if calls:
                    rc = 1
        return rc

    if len(args.elf) != 2:
        raise Error("two ELF files are needed for a comparison")
    a = Image(args.elf[0], objdump, readelf, dfp)
    b = Image(args.elf[1], objdump, readelf, dfp)
    return compare(a, b, only=args.func, show=args.show)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except Error as e:
        print("fncmp: error: %s" % e, file=sys.stderr)
        sys.exit(2)
