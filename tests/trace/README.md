# Register trace - decision (P0.3 spike, overridden by the user 26.09.2026)

The question: how does a driver, compiled unchanged on the host with MinGW gcc,
record what it writes to the SFRs, so that later tasks can prove "trace unchanged" -
**in order**, because P10 splits `clock.c` and the order of the clock writes is the
thing that must not change.

## Decision: (a) snapshot diff in plain C - the user's call, 26.09.2026

**The spike (below) recommended a hybrid page-guarded array** (per-access trapping via
a Windows vectored exception handler and `VirtualProtect`), because it is the only one
of the four candidates tried that keeps write order and shows unchanged-value writes.
**The user rejected that recommendation on 26.09.2026 and chose plain approach (a)
instead**: the fake `xc.h` declares every SFR as an ordinary variable (no page guard, no
exception handling, no single-stepping); the harness diffs all SFRs against the last
snapshot after each call and at every `trace_point()`. Accepted consequences, spelled
out at the time: **order between two trace points is lost**, and **a write that leaves
a register's value unchanged is invisible**. P10 will add trace points between the
individual clock steps so that phase still gets the ordering guarantee it needs, one
step at a time, without needing per-access trapping for the whole harness.

The measurements below (from the spike, still valid - they are what the recommendation
was based on) are kept as the record of why the hybrid existed at all and what plain
(a) gives up by not being it.

| | (a) snapshot, plain array **(chosen 26.09.2026)** | (a') access function per use | (b) C++ proxies | guarded array (spike's recommendation, **rejected**) |
|---|---|---|---|---|
| order | only between trace points; `dma0_init()`'s 24 writes came out as 9 lines in *index* order (IEC2 before DMACON) | per access | per access | per access |
| unchanged-value writes (`DMA0STAT = 0`, `T1CON = 0`) | invisible | invisible | visible | visible |
| drivers compile unchanged | yes (C) | **no**: 36 register names are also bit-field names (`PC`, `SPLIM`, `FSCL`...) and break as macros; `&X` in `adc.c`'s static table is no constant | `timebase.c`, `clock.c`, `sccp.c`, `dac.c`, `chaintest.c` yes; **`dma.c` no** (3x `(uint32_t)ptr` is an error in C++ on 64 bit), **`adc.c` no** (`ADCBITS()` casts a pointer to the bit type: the proxy is bound to AD3's index, not to the pointer), `capture.c` no (`_Static_assert`) | yes, all 17 files (only `-Wno-pointer-to-int-cast`) |
| polling loops | **a background "hardware model" thread (below) - no per-access hook needed** | read hook | read hook | read hook |
| accesses through pointers | seen by the diff | not hooked | wrong register | trapped like any other |
| extra machinery | none beyond a `CRITICAL_SECTION` and one background thread | none | C++ | `VirtualProtect`, a vectored exception handler, single-stepping |

Measured (spike): `trace_c1.txt` (38 lines) vs `trace_c3.txt` (59 lines) for the same
scenario; `timebase_init()` is 2 writes in mode (a) and the real 5 (`T1CON=0, TMR1=0,
PR1, TCKPS, ON`) in the guarded-array mode. Under P0.4's plain (a), `timebase_init()`'s
net writes are **T1CON 0x0 -> 0x8010, PR1 0x0 -> 0xFFFFFFFF**; `TMR1 0x0 -> 0x0` never
appears (unchanged), which is exactly the accepted consequence above, not a bug.

## How the harness works (P0.4)

`tools/gen_fake_sfr.py` (evolved from the spike prototype, unchanged in its essentials)
turns the DFP's device header into a host `xc.h`, `sfr_syms.ld` and `sfr_table.c`: every
SFR is one element of `sfr_mem[]`, one contiguous array, `--style symbols` (the only
style the harness uses) keeps `extern volatile T X;` so `&X` stays a link-time constant
(needed by `adc.c`'s static tables) and no SFR name becomes a macro. `--cxx` and
`--style macros` still exist in the generator only to document why they were rejected;
nothing under `tests/trace/` builds them.

`tests/trace/harness/recorder.c` implements three calls (`recorder.h`):

```
trace_begin(name)     zero every SFR (the reset value assumed throughout), print
                       "# scenario NAME", start the runaway watchdog (below)
trace_point(label)     diff every SFR against the shadow copy, address order, then "# label"
trace_end()             final diff, stop the watchdog, flush stdout
```

`trace_note(fmt, ...)` does the same diff, then one caller-formatted line - used by the
stubs for `C`/`D`/`F` lines. The diff loop (`trace_flush()`, static) walks `sfr_mem[]` in
address order (the index already is rank-of-address, from `gen_fake_sfr.py`), so two
scenarios that end up writing the same registers to the same values always list them in
the same order - the determinism the format needs does not depend on issuing order.

### The hardware model (background thread, `hwmodel.c`)

Approach (a) has no per-access read hook, so a driver's busy-wait on a self-clearing
switch-enable bit (`PLLxCON.PLLSWEN`, ...) or a hardware-set ready bit
(`OSCCTRL.PLLxRDY`, `CLKxCON.CLKRDY`, ...) would spin until `WAIT_WHILE`'s bound
(`diag.h`, `WAIT_LIMIT` = 2 000 000 iterations on the host build) and `fail()`. The
model is a second OS thread, started with `hwmodel_start(rules, n, tmr1_step)` before
the driver call and stopped with `hwmodel_stop()` after it, that repeatedly forces a
small set of bits to a fixed state:

```c
{ "PLL1CON", PLLSWEN|FOUTSWEN|OSWEN|DIVSWEN, 0 },   /* self-clearing: force to 0 */
{ "OSCCTRL", 0, PLL1RDY|PLL2RDY },                  /* hardware-set: force to 1  */
```

**Why this is deterministic despite being a real, unsynchronised OS thread:** every rule
is level-triggered and idempotent - applying it before, during or after the driver sets
the bit it waits on produces the same final state (switch-enable bit back at 0, ready bit
at 1). There is no ordering between the model thread and the driver thread for a race to
disagree about; the only thing that varies run to run is how many extra times the
driver's spin loop re-read the bit before the model's next iteration cleared it, and
approach (a) cannot see that anyway - it only diffs at trace points, after the call has
returned. Proven, not just argued: the `timebase` and `clock` scenarios each ran **15**
times (`clock`, with the model) and **5** times (`clock`, without it; `timebase`, which
needs no model) with byte-identical stdout every time.

Two bugs found while building this, both worth keeping here so nobody re-discovers them:

- **`windows.h` `#define`s `STRICT` to `1`**; the device header has an unrelated
  same-named bit field (e.g. `FICD`'s `STRICT`). `recorder.c` includes `<windows.h>`
  before `<xc.h>` (it needs `CRITICAL_SECTION`) and must `#undef STRICT` in between, or
  the bit-field declaration fails to compile with "expected identifier ... before
  numeric constant".
- **A masked hardware-set is required, not a whole-word one.** The first version of
  `hw_set()` copied the model's *entire* new register word into both `sfr_mem[]` and the
  diff's shadow copy. That is correct only for a register nothing else ever writes
  (`TMR1`), and wrong for `PLL1CON`/`CLK1CON`/etc., which also carry driver-written bits
  (`ON`, `NOSC`, ...) that `trace_flush()` had not diffed yet: the model's next bit-clear
  silently copied those undiffed bits into shadow too, and the driver's own write (e.g.
  `PLL1CON = 0x8100`) vanished from the trace entirely - not garbled, just gone, because
  shadow and the live register agreed by the time anyone looked. Fixed by
  `hw_set_masked(idx, mask, value)`: only the rule's own bits are copied into shadow;
  every other bit is left for the ordinary diff to find. `hw_set(idx, v)` is
  `hw_set_masked(idx, ~0u, v)` - correct only where nothing else ever writes that SFR.
- **A `CRITICAL_SECTION` serialises `hw_get()`/`hw_set_masked()` with the diff loop.**
  Both threads touch `sfr_mem[]` and `shadow[]`; without a lock, the diff could read a
  register mid-update by the model thread. The register count is small (~1500 words) and
  trace points are infrequent, so the lock is held only briefly.
- The model thread calls `Sleep(0)` once per iteration (yield without a fixed delay) so
  it does not spin at 100% CPU longer than it has to; on any machine with more than one
  logical core it and the driver thread simply run in parallel, which is why the 20 runs
  above never took the scheduler's ~15 ms quantum into account and still came back
  instantly (see "runtime" below).

### Runaway guard

Every `WAIT_WHILE()` is bounded in loop iterations, not wall-clock time, so a bit the
model answers always lets the loop finish quickly - but a scenario that calls into code
with a *software* wait the model does not cover (an ISR-set RAM flag; decision below:
out of scope, so such entry points are left out of scenarios) could still spin for a long
time. `trace_begin()` starts a watchdog thread (`TRACE_TIMEOUT_MS`, default 5000) that
aborts the process with `trace: scenario exceeded N ms wall clock - aborting.` and exit
code 3 if `trace_end()` has not stopped it by then - proven with a scenario that spins
forever on purpose (killed at the configured timeout, message as designed, not used by
either committed scenario).

## Decisions on the P0.3 open points, made by the user, 26.09.2026

1. **Console output.** The spike recommended linking `cli.c` and turning `U2TXB` writes
   into `C` lines. **Overridden**: `console_*` are stubbed directly into `C` lines
   (`tests/trace/harness/stubs.c`); `cli.c` is not linked into any scenario.
2. **Software waits** on ISR-set RAM (`blocks_done`, `burst_active`, `adc_events`) are
   out of scope. No hook fires an ISR from the harness; entry points that need one of
   these are left out of scenarios (unchanged from the spike's second, more cautious
   recommendation).
3. **Golden traces compare writes (as snapshot diffs), console lines, stub lines (e.g.
   `__delay32`) and `fail()` lines - no reads.** There is no `R` line in the P0.4 format
   at all (approach (a) has no read hook to log a read from); the spike's `R` lines were
   specific to the rejected per-access modes.

## Trace format (fixed by P0.4)

    W PLL1CON      0x00000000 -> 0x00008100      write: the net change since the
                                                  previous trace point, address order
    C [clk] PLL1 locked, 320 MHz                 console output of the driver (stub)
    D __delay32(20000000)                        other stubs (delay, capture_halt, ...)
    F fail(1)                                    fail() - longjmp back to the scenario
    # clock_init()                               the scenario's own comments / trace_point()

Values that are host addresses are printed symbolically: `&AD5CH0RES(0x000DA4)` for an
SFR (the device address from the gld), `&buf+0x0` for a RAM region the scenario
registered (`trace_region()`). Otherwise the truncated 64-bit host address would be
neither the target's value nor deterministic.

## Polling loops and how each is satisfied

| Where | Waits on | Bit is | P0.4 status |
|---|---|---|---|
| clock.c 107, 163 | `CLK1CON.OSWEN` | self-clearing | **answered** (`clock` scenario's rules) |
| clock.c 115/138, 590, 641 | `PLLxCON.PLLSWEN` | self-clearing | **answered** (lines 115/138; 590/641 belong to `clock_adc_set_div`/`_set_rate`, not exercised by the `clock` scenario) |
| clock.c 117/140, 594 | `PLLxCON.FOUTSWEN` | self-clearing | **answered** (115/140) |
| clock.c 119/142 | `PLLxCON.OSWEN` | self-clearing | **answered** |
| clock.c 130/150 | `PLLxCON.DIVSWEN` | self-clearing | **answered** |
| clock.c 120/143, 597, 644 | `OSCCTRL.PLLxRDY` | set by hardware (lock) | **answered** |
| clock.c 175, 350, 398 | `CLK6/7/13CON.OSWEN` | self-clearing | CLK6CON **answered**; CLK7/13CON not exercised (clock_init() does not touch them) |
| clock.c 274, 402 | `CLK6/13CON.DIVSWEN` | self-clearing | not exercised (`clock_adc_set_div`, not called by P0.4's scenario) |
| clock.c 277, 315, 353, 405, 600, 647 | `CLKxCON.CLKRDY` | set by hardware | CLK1CON/CLK6CON's rule sets it as a side effect, but nothing in `clock_init()` waits on it; the other call sites are not exercised |
| clock.c 493 | `CM4STAT.BUFV` | set by hardware (window done) | open (P0.5, whichever scenario calls `clock_monitor_hz()`) |
| adc.c 131, 287 | `ADxCON.ADRDY` (via `adc_cur` pointer) | set by hardware after `ON` | open (no ADC scenario in P0.4) |
| cli.c 152, 164, 221, 256, 280 | `U2STAT` bits | hardware | moot: `cli.c` is not linked into any scenario (decision above) |
| capture.c 1130, 1154; chaintest.c 188; clock.c 519 (sim path only) | `TMR1` via `timebase_ticks()` | counts | `hwmodel_start()`'s `tmr1_step` parameter exists for this (advance TMR1 by a fixed amount per model iteration); not exercised by either P0.4 scenario - `__delay32()`'s own TMR1 advance (below) is the only TMR1 model in use so far |
| capture.c 1104; chaintest.c 267, 1072 | RAM flags/counters set by ISRs, with a `TMR1` timeout | software | **out of scope** (decision 2 above) |
| capture.c 257, 802; cli.c 826, 1282 | `burst_active`, `blocks_done` (RAM, ISR) | software | **out of scope** (decision 2 above) |

`timebase.c`, `dma.c`, `dac.c`, `sccp.c`, `led.c` have no SFR polling loop.
`diag.c`'s `for (;;)` blink loops only run after `fail()`, which the host stubs.

## Stubs and compiler features

- `__delay32(n)`: a `D` line, and TMR1 advances by n/16 (200 MHz CPU, 12.5 MHz Timer1),
  so `timebase_check()` returns a fixed value (1 250 000 for the 100 ms call in
  `timebase.c`).
- `__attribute__((interrupt, no_auto_psv))`, `persistent`: `sfr_host.h` defines
  `interrupt`, `no_auto_psv`, `persistent` as `__unused__` (x86 gcc knows `interrupt`
  with another signature and refuses `void f(void)`). The words occur in no driver as
  identifiers (grep). ISRs are then ordinary functions the scenario could call directly
  (the `clock` scenario does not call `_CLKFInterrupt()`, but it still has to compile).
- `Nop()`, `ClrWdt()`, `__builtin_nop()` -> `((void)0)`.
- `__asm__ volatile ("reset")` (cli.c, not linked into any P0.4 scenario): the host
  assembler would reject it; `sfr_host.h` defines an empty assembler macro `reset` for
  the day a scenario does link something that uses it.
- `console_*`, `capture_halt()`: stubs write `C`/`D` lines (`tests/trace/harness/stubs.c`,
  decision 1 above - no `cli.c` in the link). `fail()` writes `F` and `longjmp`s back to
  the scenario's `setjmp()`.
- Compiler flags that are required, not taste: **`-mno-ms-bitfields`** (MinGW's default
  ms_struct layout makes 916 of 1568 bit-field typedefs larger than 4 bytes; the generated
  `sfr_layout_check()` compares all 11755 named fields with the pack's `_MASK` values -
  0 differ with the flag, confirmed again in P0.4: `# layout check: 0 fields differ`),
  `-fno-strict-aliasing` (`X` and `Xbits` alias), `-Wl,--disable-dynamicbase` (the placed
  symbols are absolute; with ASLR the first SFR access segfaults), `-Wno-pointer-to-int-cast`
  (dma.c's `(uint32_t)ptr`). The linker prints "stripping non-representable symbol" for
  the placed symbols: harmless, expected on every build.
- `#undef STRICT` between `<windows.h>` and `<xc.h>` in `recorder.c` - see "two bugs"
  above.
- `__DATA_BASE/__DATA_LENGTH` are widened to 0x4 / 0xFFFFFFF8 in the host header, else
  every truncated host buffer address is "outside RAM" and `dma0_init()` fails(8).
  (Not exercised by the P0.4 scenarios, which do not link `dma.c`; kept because
  `gen_fake_sfr.py` still generates it and a later scenario will need it.)

## What the trace cannot see

- **Order between two `trace_point()`s.** Only the *net* change survives; a register
  written twice between two points shows only the second value, and two registers
  written in the opposite order to how the driver wrote them can print in address order
  instead. Accepted consequence of approach (a) (decision above); P10 adds trace points
  between the clock steps it needs ordered.
- **A write that leaves a register's value unchanged.** `TMR1 = 0` when TMR1 already
  reads 0 is indistinguishable from not writing it at all.
- **Reads.** There is no read hook and no `R` line at all in this format (decision 3
  above) - not even for debugging.
- **Timing.** No cycles; delays only as `D` lines; TMR1 is a model, not real time.
- **Hardware side effects** beyond the model's rules: no clock switches, DMA transfers,
  conversions or interrupts happen unless a rule or the scenario makes them.
- **ISR-driven software waits** (`blocks_done`, `burst_active`, `adc_events`): out of
  scope (decision 2 above); scenarios that would need one are simply not written yet.
- **Register semantics.** An SFR is a memory cell: write-1/0-to-clear (`DMA0STAT = ~flags`
  stores 0xFFFFFFEF), read-to-clear (`CHxRDY` on reading `CHxDATA`), read-only bits and
  `SET/CLR/INV` behaviour are not modelled; later reads see what was written.
- **Access width.** A bit-field store may be a byte store on the host and a 32-bit or
  `bset` on the dsPIC; the trace shows the register value, not the width.
- The **device reset values** (all 0, as in the spike; the gld/header have none - not
  revisited in P0.4).
- Code under `#ifdef __MPLAB_DEBUGGER_SIMULATOR` (the host takes the hardware path,
  `WAIT_LIMIT` = 2 000 000 not the simulator's 20 000) and the other `BOARD`'s code
  unless built with `-DBOARD=2` against the MPS506 header.
- It is **Windows/x86-64 specific**: the hardware model thread, its `CRITICAL_SECTION`
  and the runaway watchdog are all Win32 API (`CreateThread`, `Sleep`, `WaitForSingleObject`).
  A Linux port would use `pthread_create` and a `pthread_mutex_t`; not needed today.

## Simulator (question for P0.8, not touched by P0.4)

`tests/trace/spike/sim_sfr_probe.py` (MDB, sim build, 58 s Run, 99 s in total, not the
acceptance run): after boot (`boot_stage = 9`, `fail_code = 0`):

| SFR | reset (before Run) | after boot | firmware wrote |
|---|---|---|---|
| `T1CON` | 0x00000000 | 0x00008010 | 0x00008010 - kept |
| `VCO1DIV` | 0x00000000 | 0x00020000 | 0x00020000 - kept |
| `CLK6CON` | 0x00000101 | 0x00829501 | 0x00029500 + OSWEN - kept, status bits the simulator's |
| `PLL1DIV` | 0x0100C812 | **0x01000000** | 0x0100C829 - **not kept** |
| `PLL2DIV` | 0x0100C812 | **0x01000000** | 0x01007D29 - **not kept** |

So the simulator stores writes to some unmodelled SFRs and not to others (both PLL
divider registers lose the feedback and post-divider fields). P0.8 can compare values
only register by register where the simulator keeps them, and needs the reduced check
(addresses and bit positions against the ATDF) for the rest. The probe needed the same
`Set uart2io...`/`oscillator` settings as `sim_trap.py`; without them MDB answered no
`Print` after `Halt` (W0101-SIM NullPointerException). Boot to stage 9 takes ~55 s of
simulation because of the UART banner.

## How to run it

    tools\trace.bat            build + run every scenario, compare against golden traces
    tools\trace.bat record     (re)write every scenario's golden trace from what it just
                                produced - use once a task's comment says a trace is
                                expected to change, review the diff, then commit it

A scenario with no golden file yet (both `timebase` and `clock`, as of P0.4 - "not
golden yet", the golden traces are P0.5's job) is still built and run, so a broken build
or a crash is still caught, and reported `NEW` rather than `PASS`/`FAIL`.

Environment variables, read by the scenarios themselves, not by `trace.bat`:

- `TRACE_HWMODEL=0` - the `clock` scenario skips starting the hardware model, so
  `clock_init()` runs its first `WAIT_WHILE` into the bound and `fail(1)`s. Used to
  prove the model matters, not committed as a golden trace (there would be nothing
  useful to diff against - the fail happens on line 1 of `clock_init()`, before it can
  do anything the golden trace would want to check).
- `TRACE_TIMEOUT_MS` - the runaway watchdog's timeout, default 5000.

Both scenarios run in well under a second (`timebase`: ~0.15 s; `clock`, with the model:
~0.16 s) - the generator itself (`tools\trace.bat`'s first step) is the slower part, at
about a second.

## Remaining open points (P0.5 / P10 / P0.8)

- **Golden traces.** None committed yet - P0.5's job, per
  `docs/IMPLEMENTATION-PLAN.md`'s scenario table (`boot`, `stream_on`, ..., `regs`,
  `nano`). Note that table's scenarios do not exactly match P0.4's `timebase`/`clock`
  working examples (P0.5's `boot` calls `timebase_init()` as one of several init calls;
  its `clk` scenario calls `capture_set_clkdiv()`/`clock_adc_set_rate()`, not
  `clock_init()` directly) - whoever writes P0.5 decides whether to keep, extend or
  replace `tests/trace/scenarios/timebase.c` and `clock.c`.
- **Ordered writes for P10.** Add `trace_point()` calls between the individual steps of
  whatever `clock.c` split P10 produces, so each step's writes are captured on their own
  and the split cannot silently reorder two steps that used to be adjacent statements in
  one function.
- **CM4STAT.BUFV, ADxCON.ADRDY, the CLKRDY call sites in `clock_adc_set_div`/
  `_set_rate`, and TMR1 as a continuous count** (the `tmr1_step` parameter already
  exists in `hwmodel.h` for this) are not answered by any P0.4 rule; add rules when a
  scenario needs them (see the polling-loop table above).
- **Static buffers** (`capture.c`'s `static dma_buffer`, section `.dma_buffer`): not
  reachable by name for `trace_region()`; a host linker script symbol around the section
  would make `DMA0DST` print as `&dma_buffer+0x0`. Needed once a DMA/capture scenario
  exists.
- **Reset values from the ATDF** (`initval`), for scenarios whose code branches on a
  register's state at entry (`clock_init()` reads `CLK1CONbits.COSC`) - still assumed
  all-0, as in the spike.
- **Simulator cross-check (P0.8)**, unchanged from the spike's findings above.
