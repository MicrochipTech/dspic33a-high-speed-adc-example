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
| polling loops | **a background "hardware model" thread (P0.4/P0.5), then a page-guarded read hook limited to the polled registers (P0.5b, below) - see "the hybrid, after all"** | read hook | read hook | read hook |
| accesses through pointers | seen by the diff | not hooked | wrong register | trapped like any other |
| extra machinery | none beyond a `CRITICAL_SECTION` and one background thread | none | C++ | `VirtualProtect`, a vectored exception handler, single-stepping |

Measured (spike): `trace_c1.txt` (38 lines) vs `trace_c3.txt` (59 lines) for the same
scenario; `timebase_init()` is 2 writes in mode (a) and the real 5 (`T1CON=0, TMR1=0,
PR1, TCKPS, ON`) in the guarded-array mode. Under P0.4's plain (a), `timebase_init()`'s
net writes are **T1CON 0x0 -> 0x8010, PR1 0x0 -> 0xFFFFFFFF**; `TMR1 0x0 -> 0x0` never
appears (unchanged), which is exactly the accepted consequence above, not a bug.

## P0.5b, 27.09.2026: the hybrid, after all - but only for polling

**The recorded trace format did not change.** Every consequence of the 26.09.2026
decision above still holds exactly as written: order between two `trace_point()`s is
still lost, a write that leaves a register's value unchanged is still invisible, there
is still no `R` line and never was one logged. What changed is *how a driver's poll of a
self-clearing switch-enable bit or a hardware-set ready bit gets answered* - the row the
table above calls "polling loops".

P0.4/P0.5 answered it with a background OS thread (the next section, kept below as
history) that repeatedly forced those bits to their expected state. It worked in the
limit, but a short enough wait (`DIVSW_WAIT_LIMIT`, 100 000 iterations) could race the
thread's own start-up on this host - about 1 attempt in 100-200, papered over with
silent retries at the scenario level (also kept below, as history, since the mechanism
that made them necessary is gone). The user's decision on 27.09.2026, once that residual
race would not close under any further tuning of the thread: bring back *part* of the
P0.3 spike's rejected guarded array - not to log accesses (that stays rejected, and nothing
below changes the trace format), but to answer a poll deterministically, on the driver's
own thread, with no second thread to race at all.

The hybrid, as built (`tests/trace/harness/hwmodel.c`, `recorder.c`):

- **Page-guard only the pages that contain a scenario's own polled registers** (plus
  `TMR1`, if the scenario uses `tmr1_step`) - `VirtualProtect(PAGE_NOACCESS)` on exactly
  those 4 KiB pages of `sfr_mem[]` (page-aligned, `recorder.c`), not the whole array the
  spike guarded. Because the real ~2039 SFRs are tightly packed into the first two pages
  of the (4-page) array, guarding "the page PLL1CON lives on" typically guards a few
  hundred *other* registers too as a side effect - harmless (see the next point) and not
  worth avoiding, since the target was never to minimise trapped accesses, only to answer
  polls.
- **A read of the *exact* register a rule names applies the rule** - clears the
  self-clearing bits, sets the hardware-set ready bits, or (for `TMR1`) advances it by
  the scenario's `tmr1_step` - via a vectored exception handler and single-step, adapted
  from the spike's `tests/trace/spike/recorder.c` (`git show f1c5ad5`). Unlike the model
  thread, this is not level-triggered-and-eventually-consistent; it is applied exactly
  once, synchronously, before the faulting read instruction re-executes - so the very
  first read of a polled register already sees the answer. **A write, or a read of any
  *other* register sharing the guarded page, just passes through** (unprotect, let the one
  instruction run, single-step, re-guard) - no rule applied, nothing logged.
- **The hook never writes to the trace.** It calls `hw_set_masked()`/`hw_set()`
  (`recorder.c`) exactly as the old model thread did, which update `sfr_mem[]` and
  `shadow[]` together - a hardware-driven change, not a driver write, invisible to
  `trace_flush()`'s diff, same as before. Approach (a)'s snapshot diff is the only thing
  that ever prints a trace line, unchanged.
- **`hwmodel_pause()`/`hwmodel_resume()` bracket `trace_flush()`'s own scan.** Found by
  testing, not by inspection: `trace_flush()` walks all `SFR_COUNT` entries of `sfr_mem[]`
  to diff them against `shadow[]` - which means it *reads* every polled register itself,
  and without this, that incidental read fired the rule exactly as a real poll would (the
  `clock` scenario's trace gained a spurious `W OSCCTRL 0x0 -> 0xC000` between
  `hwmodel_start()` and the first `trace_point()`, from the diff loop's own read of
  `OSCCTRL` landing before its read of `shadow[]` had caught up to the mutation it had
  just triggered). `hwmodel_pause()` un-guards every currently-guarded page for the
  duration of the scan (single-threaded - nothing else runs while it does), so the diff
  sees the true state, exactly as if no hook existed; `hwmodel_resume()` re-guards them
  afterward. A no-op when `hwmodel_start()` was never called, so `recorder.c` calls both
  unconditionally around every diff.
- **A reentrancy bug found and fixed the same way**: the handler must
  `VirtualProtect(PAGE_READWRITE)` the faulting page *before* calling `hw_get()`/
  `hw_set_masked()` to apply a rule, not after - those functions read and write
  `sfr_mem[idx]` directly, and applying the rule while the page is still guarded makes
  that access fault again, re-entering the handler for a fault it has no stack-based way
  to track (`g_reguard_page` is one global, not a stack) - observed as an infinite
  re-fault loop at the same instruction, killed only by the runaway watchdog (below).

**Why this closes the race rather than narrowing it further:** there is no second thread
any more, so there is nothing left for the driver thread to race against. `hwmodel_start()`
resolves every rule's register (and `TMR1`, if used) to its `sfr_mem[]` index and page
*before* the driver runs a single instruction, and guards those pages immediately; by the
time the driver's own code reads one, the answer is already there, deterministically,
every time. `variants.c`'s "call three times unconditionally" trick and every scenario's
"retry up to 5x" loop (dac.c, sccp.c, b2b.c, clk.c, stream_on(_input).c) were removed
entirely (P0.5b) - every entry point is now called exactly once, as `docs/IMPLEMENTATION-
PLAN.md`'s plan says. Re-recording every golden trace against the new mechanism produced
**byte-identical output to every P0.5 golden, `variants` included**: the single
deterministic call now lands exactly where the old retried/tripled call always eventually
converged to, so there was nothing to explain in a diff - none was needed. Verified: 20
consecutive runs of the `dac` scenario (the shortest wait) and 5 full `tools\trace.bat`
check runs, all byte-identical (`tests/trace/README.md`'s own change log / the P0.5b
commit message has the exact counts).

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
trace_begin(name)     preset every SFR to its device reset value (sfr_reset[], the
                       ATDF's initval - P0.9; all 0 before that), print
                       "# scenario NAME", start the runaway watchdog (below)
trace_point(label)     diff every SFR against the shadow copy, address order, then "# label"
trace_end()             final diff, stop the watchdog, flush stdout
```

`trace_note(fmt, ...)` does the same diff, then one caller-formatted line - used by the
stubs for `C`/`D`/`F` lines. The diff loop (`trace_flush()`, static) walks `sfr_mem[]` in
address order (the index already is rank-of-address, from `gen_fake_sfr.py`), so two
scenarios that end up writing the same registers to the same values always list them in
the same order - the determinism the format needs does not depend on issuing order.

### The page-guarded read hook (`hwmodel.c`, P0.5b, 27.09.2026)

Approach (a) has no per-access read hook of its own (the trace format's decision above,
unchanged), so a driver's busy-wait on a self-clearing switch-enable bit
(`PLLxCON.PLLSWEN`, ...) or a hardware-set ready bit (`OSCCTRL.PLLxRDY`,
`CLKxCON.CLKRDY`, ...) needs *something* to answer it or it spins until `WAIT_WHILE`'s
bound (`diag.h`, `WAIT_LIMIT` = 2 000 000 iterations on the host build, or the shorter
`DIVSW_WAIT_LIMIT` = 100 000) and `fail()`s. `hwmodel_start(rules, n, tmr1_step)` (called
before the driver call, `hwmodel_stop()` after it) now installs that answer as a
page-guarded read hook instead of the background thread P0.4/P0.5 used (history, below):
it resolves every rule's register (and `TMR1`, if `tmr1_step != 0`) to its `sfr_mem[]`
index, works out which 4 KiB page(s) of the (page-aligned) array contain them, and
`VirtualProtect(PAGE_NOACCESS)`s exactly those pages. A Windows vectored exception
handler answers the resulting access violations:

```c
{ "PLL1CON", PLLSWEN|FOUTSWEN|OSWEN|DIVSWEN, 0 },   /* self-clearing: cleared on read */
{ "OSCCTRL", 0, PLL1RDY|PLL2RDY },                  /* hardware-set: set on read      */
```

- **A read of the exact register a rule names** applies the rule (`hw_set_masked()`) - or,
  for `TMR1`, advances it by `tmr1_step` (`hw_get()`+`hw_set()`) - *before* the faulting
  instruction re-executes: `VirtualProtect(PAGE_READWRITE)` the page, apply the rule (now
  safe - see the reentrancy bug below), set the trap flag, `EXCEPTION_CONTINUE_EXECUTION`
  so the one instruction runs and immediately single-steps; the `EXCEPTION_SINGLE_STEP`
  that follows clears the trap flag and re-guards the same page. The very first read
  already sees the answer - there is no second thread for a "how many times did it have to
  poll" race to happen in any more.
- **A write, or a read of any *other* register sharing the guarded page, just passes
  through** the same unprotect/single-step/re-guard dance with no rule applied and nothing
  logged - "not ours to touch". Because the ~2039 real SFRs are packed into the first two
  of `sfr_mem[]`'s four pages (`gen_fake_sfr.py` assigns indices by rank of device
  address, with no gaps), guarding "the page `PLL1CON` lives on" typically guards a few
  hundred *other* registers too; harmless, and not worth avoiding.
- **The hook never writes to the trace.** `hw_set_masked()`/`hw_set()` update `sfr_mem[]`
  and `shadow[]` together, exactly as they did for the old model thread - a hardware
  change, not a driver write, invisible to `trace_flush()`'s diff. Approach (a)'s snapshot
  diff remains the only thing that ever prints a line.
- **`hwmodel_pause()`/`hwmodel_resume()` bracket `trace_flush()`'s own scan** - found by
  testing: the diff loop reads every one of `SFR_COUNT` `sfr_mem[]` entries itself, which
  fired a rule on its own incidental read of a guarded register exactly as a real poll
  would (`clock`'s trace gained a spurious `W OSCCTRL 0x0 -> 0xC000` between
  `hwmodel_start()` and the first `trace_point()` before this was found - the diff's read
  of `OSCCTRL` triggered the mutation, but its read of `shadow[]` for the *same*
  comparison had already happened, so the comparison saw a stale 0 on one side and the
  freshly-mutated value on the other). Pausing un-guards every currently-guarded page for
  the scan (single-threaded - nothing else runs while it does) so the diff sees the true
  state; resuming re-guards them. A no-op when `hwmodel_start()` was never called.
- **A reentrancy bug, found the same way**: the page must be unprotected *before* calling
  `hw_get()`/`hw_set_masked()` to apply a rule, not after - those functions touch
  `sfr_mem[idx]` directly, and doing so while the page is still guarded faults again,
  re-entering the handler for an access it has no stack-based way to track (one global
  `g_reguard_page`, not a stack) - seen as an infinite re-fault loop at the same
  instruction, killed only by the runaway watchdog (below).
- **`windows.h` `#define`s `STRICT` to `1`**; the device header has an unrelated
  same-named bit field (e.g. `FICD`'s `STRICT`). Both `recorder.c` and `hwmodel.c`
  include `<windows.h>` before `<xc.h>` and must `#undef STRICT` in between, or the
  bit-field declaration fails to compile with "expected identifier ... before numeric
  constant" - the same fix in both files now, since `hwmodel.c` also needs `sfr_mem`'s
  extern declaration (from the generated `xc.h`) to compute page addresses.

**Why this closes the race rather than narrowing it further:** there is no second thread
any more, so there is nothing left for the driver thread to race against - the pages are
guarded before the driver runs a single instruction, and the very first read of a polled
register always sees the answer.

### History: the background thread (P0.4/P0.5) and its residual race

Kept for the record, since the retries it forced are what P0.5b removed. P0.4/P0.5
answered a poll with a second OS thread, started with `hwmodel_start()` and stopped with
`hwmodel_stop()`, that repeatedly forced the same bits to the same fixed state in a tight
loop - level-triggered and idempotent, so applying a rule before, during or after the
driver checked the bit gave the same final answer, and *which* iteration happened to win
was invisible to a trace format that only diffs at trace points anyway. It worked for
`clock_init()`'s and `adc_init()`'s `WAIT_LIMIT`-bound waits (2 000 000 iterations - 15+5
runs, byte-identical every time), but P0.5's shorter `DIVSW_WAIT_LIMIT`-bound waits
(100 000 iterations: `clock_dac_on()`, `clock_trig_on()`,
`clock_adc_set_pll()`/`_set_rate()`/`_set_div()`) could race the thread's own start-up on
this host (14 logical cores) - a throwaway repro of the shortest wait in the firmware
(`dac`'s `clock_dac_on()`) failed 50-95 times per 100 with the plain model, because
`CreateThread()`'s own latency (first scheduling, page faults for its stack) could exceed
the entire wait window. Three mitigations, each measurably helpful and each kept - an
event proving the thread had started (50-95/100 down to about 1/100), pinning the model
and driver threads onto different logical cores, and a heartbeat proving live throughput,
not just existence - never closed the gap: it stayed at roughly 1 failure per 100-200 runs
no matter how the warm-up was tightened. What actually made the golden traces reproducible
was a fourth layer that had nothing to do with `hwmodel.c` at all: every scenario driving
a `DIVSW_WAIT_LIMIT`-bound function retried it silently (no `trace_point()` between
attempts) up to 5 times on failure (`dac.c`, `sccp.c`, `b2b.c`, `clk.c`,
`stream_on(_input).c`), and `variants.c` called `capture_select_variant()` three times
**unconditionally** per variant (its own preamble discards
`clock_adc_set_div()`/`clock_adc_set_pll()`'s return codes, so the function's own return
value could not reliably say whether that preamble had raced - observed once as
`PLL1CON 0x0 -> 0x10000000`/`CLK6CON 0x80000000 -> 0x80400000` sitting in a trace for a
call that had returned `true`). `boot`, `nano`, `fail`, `regs` needed none of this - they
only ever reached the longer, never-observed-to-race `WAIT_LIMIT` waits.

P0.5b removed `hwmodel.c`'s thread (and every one of the retries above) outright, once the
read hook made them unnecessary - see the section above.

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

## Golden traces (P0.5)

One golden trace per row of `docs/IMPLEMENTATION-PLAN.md`'s P0.5 table, plus the two P0.4
working examples (kept - see below). `NAME.sources` lists the real firmware `.c` files
`tools/trace.bat` links in, unchanged, alongside the harness; `NAME.cflags` (only where a
scenario needs one) adds compiler flags; `NAME.mcu` (only `nano`) points the fake header
at a different device.

| Scenario | Entry points | Sources | Model rules | Left out / notes |
|---|---|---|---|---|
| `boot` | `led_init()`, `console_early_init()` (stub), `clock_init()`, `cli_init()` (stub), `timebase_init()`, `adc_init()`, `capture_init()`, in main.c's own order | clock, timebase, adc, capture, dma, led, sccp | PLL1CON, PLL2CON, OSCCTRL (both RDY), CLK1CON, CLK6CON (`clock_init()`'s waits, same shape as `clock`); AD3CON.ADRDY (`adc_init()`, board default core 3) | `console_early_init()`/`cli_init()` are cli.c-only (decision 1) - stubbed, their UART pin/PPS/baud/command-registration effects are not in the trace. `boot_mark()`, `diag_report_reset()`, `crc16_selfcheck()`, the console banner: not in the plan's entry-point list, left out as boot-order glue |
| `stream_on` | `chain_stream_on(1000)` (1 MSPS), `chain_stream_off()` | chaintest, capture, adc, dma, clock, sccp, dac, timebase, led | PLL1CON, OSCCTRL(PLL1RDY), CLK6CON, CLK7CON, CLK13CON, AD5CON.ADRDY (CHAIN_CORE), AD3CON.ADRDY (`restore()`'s default core); `tmr1_step=10000` (`wait_ticks()`, the two 25-tick waits) | PLL1DIV/VCO1DIV preset to their `clock_init()` boot values (setup() only rewrites PLL1's POSTDIV1/2; `clock_dac_hz()` needs PLLFBDIV/VCO1DIV already set or the triangle is refused) - `clock_init()` itself is `boot`'s/`clock`'s job. `chain_stream_on(1000)` called exactly once (P0.5b) - the read hook answers its waits deterministically |
| `stream_on_input` | `chain_stream_on_input(1000, core=2, pinsel=7, samc=1, test_signal=false)`, `chain_stream_off()` | same as `stream_on` | PLL1CON, OSCCTRL(PLL1RDY), CLK6CON, CLK13CON, AD2CON.ADRDY (the input asked for), AD3CON.ADRDY (`restore()`); `tmr1_step=10000` | `test_signal=false` (the GUI's real shape for a custom input, CLAUDE.md: "the DAC is then left alone") skips CLKGEN7/the DAC entirely - no CLK7CON rule needed. Same PLL1DIV/VCO1DIV preset as `stream_on`; `chain_stream_on_input(...)` also called exactly once (P0.5b) |
| `b2b` | `capture_set_pll(5, 5)`, `capture_start()`, `capture_stop()` | capture, adc, dma, clock, sccp, timebase, led | PLL1CON, OSCCTRL(PLL1RDY), CLK6CON, AD3CON.ADRDY | `capture_start()` triggers `capture_init()`/`dma0_init()` itself (first call, `dma_armed` starts false) - no separate call needed. No ISR ever fires (decision 2), so this traces the three control calls, not a completed capture. `capture_set_pll()` called exactly once (P0.5b) |
| `variants` | `capture_select_variant()` for all ten `capture_variant_t` values, want_ksps = 100 | capture, adc, dma, clock, sccp, timebase, led | PLL1CON, OSCCTRL(PLL1RDY), CLK6CON, CLK13CON, AD3CON.ADRDY | want_ksps = 100, not a rounder number: the SCCP-clocked variants compute `ticks = hz/1000/want_ksps` and refuse below 2 - at 8000 with no `clock_init()` run first (peripheral clock still the 4 MHz FRC/2), every SCCP variant failed (`ticks` truncated to 0); 100 keeps all ten comfortably above that floor. PLL1DIV preset to its boot value (PLLFBDIV/PLLPRE) for the same reason as `stream_on`. Each call made exactly once (P0.5b - P0.5 called it three times unconditionally against the background thread's residual race, see "History" below; the read hook removed the need) |
| `dac` | the DAC2 triangle exactly as `chain all`'s stage 8 sets it (`triangle_for(8000000u, &slp)` reproduced: slp=18, low=0xFF, high=0xF00), `dac2_off()` | dac, clock, timebase | CLK7CON (`clock_dac_on()`) | `triangle_for()` itself is `static` in chaintest.c, not linkable alone - its arithmetic is reproduced instead (see dac.c's own comment) and called through the public `dac2_triangle_start()`. PLL1DIV/VCO1DIV preset to their `clock_init()` boot values (`clock_dac_hz()` needs them). `dac2_triangle_start()` called exactly once (P0.5b) |
| `sccp` | `sccp1_start()` for every (clock, mode, event) combination the firmware uses: (PERIPHERAL,TIMER,SPECIAL), (GEN13,TIMER,SPECIAL), (PERIPHERAL,OC,SPECIAL), (GEN13,OC,SPECIAL), (PERIPHERAL,TIMER,ROLLOVER) | sccp, clock, timebase | CLK13CON (`clock_trig_on()`) | `CAP_VAR_SCCP_TRG2` uses the same (clk,mode,ev) tuple as `CAP_VAR_SCCP_T_G13` - five distinct tuples cover all six SCCP variants. `clock_trig_on()` called exactly once (P0.5b) |
| `clk` | `capture_set_clkdiv(500)`, `clock_adc_set_rate()` for 4000/8000/40000 ksps | capture, adc, dma, clock, sccp, timebase, led | PLL1CON, OSCCTRL(PLL1RDY), CLK6CON, AD3CON.ADRDY | Not `clock_init()` first - neither function needs a rate already configured, both derive their result purely from the argument. Every call made exactly once (P0.5b) |
| `fail` | `_CLKFInterrupt()` called directly | clock, timebase | none (`capture_halt()`/`console_force_up()` resolve to stubs, see below) | `capture_halt()` and `console_force_up()`: neither is "linkable" here without pulling in unrelated modules just for one line each (capture.c + adc/dma/sccp/led for `dma0_halt()`; cli.c, categorically excluded, for the other) - both stubbed, per the task's "use the real ones where linkable, stubs otherwise". diag.c is NOT linked: its real `fail()` never returns (blinks forever), which would hang this scenario until the watchdog kills it - `_CLKFInterrupt()`'s `fail(10u)` resolves to the stub (`longjmp` back) instead |
| `regs` | `regs_dump()`, called from the reset state (every SFR at its ATDF reset value since P0.9, 0 before) | diag, clock, adc, dma, capture, dac, sccp, timebase, led | none (nothing here waits) | The only scenario that links diag.c - its real `fail_code`/`boot_stage`/`chain_mark`/`fail()` take over from stubs.c's copies (`-DHAVE_DIAG`); nothing here calls `fail()`, so diag.c's real, never-returning one is never exercised, only linked. `console_sync_baud()`/`console_regs_dump()` (cli.c-only, referenced by diag.c's `fail()`/`regs_dump()`) are stubbed. Registers are at their reset assumption throughout - this scenario is about the dump's OWN output format/order, not about reproducing a booted system's values (`boot`/`b2b`/`variants`/`clk`/`stream_on(_input)` do that) |
| `nano` | `boot`'s exact sequence, `-DBOARD=2` (nano.cflags), the fake header generated from the MPS506's own device pack header (nano.mcu = `33AK512MPS506`) | same as `boot` | same shape as `boot`, but AD1CON.ADRDY (BOARD_EV17P63A's ADC_INSTANCE = 1) | Everything `boot` leaves out, left out here too. The MPS506 and MPS512 share every register `boot`'s entry points touch (CLAUDE.md: "every register and vector core 5 uses is identical on the MPS506, checked against the pack header 25.09.2026") - only which core/pins board.h names differs, which is exactly what the two golden traces, side by side, show |
| `clock` (P0.4, kept) | `clock_init()`, called twice (idempotency), plus `TRACE_HWMODEL=0` as a documented (not golden) fault-injection check | clock, timebase | PLL1CON, PLL2CON, OSCCTRL, CLK1CON, CLK6CON | Kept as an isolation test of `clock_init()` alone - `boot`'s value is the end-to-end boot order, not `clock_init()`'s own idempotency or a negative check that the model is what makes it succeed (`TRACE_HWMODEL=0`, not itself a golden trace) |
| `timebase` (P0.4, kept) | `timebase_init()` (twice), `timebase_check()`, `timebase_ticks()` (twice) | timebase | none | Kept for the same reason: `boot` calls `timebase_init()` once as part of a longer sequence; this is the acceptance trace P0.4's own "Done" criterion names ("a trace of `timebase_init()` matches the four writes in `timebase.c`") |

**Not covered by any P0.5 scenario, and why:** `test bursts`/`test sweep`/`test matrix`/`test
selftest`/`test clkoff`/`test dac`/`chain all`/`chain run`/`stream grab` and anything else
that waits on `blocks_done`/`burst_active`/an ISR-set RAM flag (decision 2, out of scope -
no entry point into any of them can be reached without such a wait); `clock_monitor_hz()`
(`CM4STAT.BUFV`, only called from `chain all`'s stage 8 and `capture_clkoff_probe()`'s
caller); AD4CON (nothing selects ADC core 4).

## Polling loops and how each is satisfied

The "Hook rule" column is exactly what each scenario's `rules[]` table
(`tests/trace/scenarios/*.c`) passes to `hwmodel_start()` - the same `{ reg, clear_mask,
set_mask }` shape since P0.4, applied by the page-guard read hook (P0.5b) instead of the
old background thread (see "The page-guarded read hook" above): a read of `reg` clears
`clear_mask`'s bits and sets `set_mask`'s bits, in `sfr_mem[]` and `shadow[]` together, so
it never appears as a driver write.

| Where | Waits on | Bit is | Hook rule (P0.5b) | Status |
|---|---|---|---|---|
| clock.c 107, 163 | `CLK1CON.OSWEN` | self-clearing | `CLK1CON`: clear `OSWEN,DIVSWEN` | **answered** (`clock`/`boot`/`nano` scenarios' rules) |
| clock.c 115/138, 590, 641 | `PLLxCON.PLLSWEN` | self-clearing | `PLL1CON`/`PLL2CON`: clear `PLLSWEN,FOUTSWEN,OSWEN,DIVSWEN` | **answered** (`clock`/`boot`/`nano`: 115/138; `clk`/`b2b`/`variants`/`dac`/`stream_on(_input)`: 590/641, `clock_adc_set_pll`/`_set_rate`) |
| clock.c 117/140, 594 | `PLLxCON.FOUTSWEN` | self-clearing | (same `PLLxCON` rule as above) | **answered** (as above) |
| clock.c 119/142 | `PLLxCON.OSWEN` | self-clearing | (same `PLLxCON` rule as above) | **answered** |
| clock.c 130/150 | `PLLxCON.DIVSWEN` | self-clearing | (same `PLLxCON` rule as above) | **answered** |
| clock.c 120/143, 597, 644 | `OSCCTRL.PLLxRDY` | set by hardware (lock) | `OSCCTRL`: set `PLL1RDY` and/or `PLL2RDY` | **answered** |
| clock.c 175, 350, 398 | `CLK6/7/13CON.OSWEN` | self-clearing | `CLK6CON`/`CLK7CON`/`CLK13CON`: clear `OSWEN` (`CLK6CON`/`CLK13CON` also clear `DIVSWEN` in the same rule, below) | **answered**, all three: CLK6CON (`clock`/`boot`/`clk`/`b2b`/`variants`/`nano`), CLK7CON (`dac`, `clock_dac_on()`), CLK13CON (`sccp`/`variants`/`stream_on(_input)`, `clock_trig_on()`) - the P0.4 open point closed |
| clock.c 274, 402 | `CLK6/13CON.DIVSWEN` | self-clearing | `CLK6CON`/`CLK13CON`: clear `DIVSWEN` (one rule per register, combined with `OSWEN` above) | **answered**: CLK6CON (`clk`'s `capture_set_clkdiv()`, `variants`' `CAP_VAR_CLKDIV`), CLK13CON (`sccp`/`variants`/`stream_on(_input)`) |
| clock.c 277, 315, 353, 405, 600, 647 | `CLKxCON.CLKRDY` | set by hardware | `CLK1CON`/`CLK6CON`/`CLK7CON`/`CLK13CON`: set `CLKRDY` (same rule as the register's `OSWEN`/`DIVSWEN` clear - one `hwmodel_rule_t` per register carries both a `clear_mask` and a `set_mask`) | **answered**: CLK1CON (`clock`/`boot`/`nano`), CLK6CON (all of the above), CLK7CON (`dac`, `stream_on(_input)`), CLK13CON (`sccp`/`variants`/`stream_on(_input)`) |
| clock.c 493 | `CM4STAT.BUFV` | set by hardware (window done) | none - no scenario guards `CM4STAT` | **still open**: no P0.5 scenario calls `clock_monitor_hz()` (only `chain all`'s stage 8 does, out of scope - see the scenario table below) |
| adc.c 131, 287 | `ADxCON.ADRDY` (via `adc_cur` pointer) | set by hardware after `ON` | `AD1CON`/`AD2CON`/`AD3CON`/`AD5CON` (whichever core the scenario selects): set `ADRDY` | **answered**: AD1CON (`nano`), AD2CON (`stream_on_input`), AD3CON (`boot`/`clk`/`b2b`/`variants`), AD5CON (`stream_on`) - the P0.4 open point closed for every core a P0.5 scenario actually selects; AD4CON stays open (nothing selects core 4) |
| cli.c 152, 164, 221, 256, 280 | `U2STAT` bits | hardware | none | moot: `cli.c` is not linked into any scenario (decision above) |
| capture.c 1130, 1154; chaintest.c 188; clock.c 519 (sim path only) | `TMR1` via `timebase_ticks()` | counts | not a `clear_mask`/`set_mask` rule - `TMR1`'s own index is guarded and every READ of it advances it by the scenario's `tmr1_step` (`hwmodel.c`'s `g_tmr1_idx`/`g_tmr1_step`, applied the same way, via `hw_get()`+`hw_set()`) | **answered**: `stream_on`/`stream_on_input` pass `tmr1_step = 10000` to `hwmodel_start()` (`chain_stream_on(_input)`'s `wait_ticks()`, `capture_chain_stop()`'s/`capture_chain_halt()`'s 25-tick waits) - the P0.4 open point closed; every other scenario still relies only on `__delay32()`'s own TMR1 advance |
| capture.c 1104; chaintest.c 267, 1072 | RAM flags/counters set by ISRs, with a `TMR1` timeout | software | none | **out of scope** (decision 2 above) |
| capture.c 257, 802; cli.c 826, 1282 | `burst_active`, `blocks_done` (RAM, ISR) | software | none | **out of scope** (decision 2 above) |

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
- **P0.5: `HAVE_DIAG`/`HAVE_CAPTURE`/`HAVE_CHAINTEST`** (each scenario's own `.cflags`,
  "How to run it" above) tell `stubs.c` which of its own copies to compile OUT because the
  real module is linked instead: `fail_code`/`boot_stage`/`chain_mark`/`fail()`
  (`HAVE_DIAG`, diag.c's real ones), `capture_halt()` (`HAVE_CAPTURE`, capture.c's real
  `dma0_halt()` wrapper), `adc_ch0_event()` (`HAVE_CHAINTEST`, chaintest.c's real one -
  adc.c's `_AD5CH0Interrupt` calls it whenever adc.c is linked, whether or not the
  scenario ever reaches the chain test's low-rate counting path). The obvious
  alternative, `__attribute__((weak))`, does NOT work on this toolchain: measured
  directly (a minimal two-file repro, one `weak` definition, one caller, no `.a` archive
  involved) - MinGW-w64 gcc 16.1.0 / GNU ld report "undefined reference" for a plain weak
  function with no other definition and no `alias(...)` target when every object is
  given to the linker directly. `console_early_init()`, `cli_init()`, `console_sync_baud()`,
  `console_regs_dump()` have no real alternative anywhere under test (cli.c only,
  excluded without exception) and are plain, unconditional stubs.
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
  every truncated host buffer address is "outside RAM" and `dma0_init()` fails(8). Not
  exercised by the P0.4 scenarios (neither linked `dma.c`); every P0.5 scenario that links
  capture.c does, and the widened range is what makes `dma0_init()` accept the real,
  truncated 64-bit host address of `capture.c`'s static `dma_buffer`.

## What the trace cannot see

- **Order between two `trace_point()`s.** Only the *net* change survives; a register
  written twice between two points shows only the second value, and two registers
  written in the opposite order to how the driver wrote them can print in address order
  instead. Accepted consequence of approach (a) (decision above); P10 adds trace points
  between the clock steps it needs ordered.
- **A write that leaves a register's value unchanged.** `TMR1 = 0` when TMR1 already
  reads 0 is indistinguishable from not writing it at all.
- **Reads.** There is still no `R` line at all in this format (decision 3 above) - not
  even for debugging. P0.5b's page-guard hook does read `sfr_mem[]` internally to answer
  a poll deterministically (the previous section), but it never prints anything; a read
  is exactly as invisible to the recorded trace as it always was.
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
- The **device reset values** of the SFRs the ATDF has no register for (445 / 441, the
  CPU's, `APG*`, `PMD*`, ... - see "P0.8 cross-check"): 0 assumed, none of them written
  by any golden. Every other SFR starts at the ATDF's `initval` since P0.9 (see "P0.9:
  reset values from the ATDF" below); until then every SFR started at 0, which P0.8
  caught in `AD3CON` (0x80008000 in the trace, 0x80488000 on the device: `RPTCNT` = 18
  at reset). And a write of a value EQUAL to the reset value is invisible - approach
  (a) diffs states, it does not count stores (`PR1 = 0xFFFFFFFF` in `timebase_init()`,
  listed under P0.9).
- Code under `#ifdef __MPLAB_DEBUGGER_SIMULATOR` (the host takes the hardware path,
  `WAIT_LIMIT` = 2 000 000 not the simulator's 20 000) and the other `BOARD`'s code
  unless built with `-DBOARD=2` against the MPS506 header.
- It is **Windows/x86-64 specific**: the page-guard read hook (`VirtualProtect`,
  `AddVectoredExceptionHandler`, the `CONTEXT` trap flag) and the runaway watchdog
  (`CreateThread`, `WaitForSingleObject`, `CreateEvent`/`SetEvent`) are all Win32 API. A
  Linux port would use `mprotect`+`sigaction(SIGSEGV)` and a `ucontext_t`'s trap flag, and
  `pthread_create`/a `timer_t` for the watchdog; not needed today.

## Simulator: the P0.3 probe (the question P0.8 answered below)

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

## P0.8 cross-check: the fake header against the ATDF and the simulator (27.09.2026)

The trace is only as good as `tools/gen_fake_sfr.py`'s output, and that generator reads
two hand-maintained files of the device pack: the C header `p<MCU>.h` (names, bit
fields, masks) and the linker script `p<MCU>.gld` (addresses). P0.8 checked both against
the pack's third description of the same silicon, the ATDF
(`<DFP>/atdf/dsPIC33AK512MPS512.atdf`, `...MPS506.atdf`; CLAUDE.md: "where the device
pack's ATDF and the datasheet disagree, the ATDF has been right every time"), and,
where the simulator can say anything at all, against what the simulator holds after
boot. Two parts, two tools, the first of them now part of `tools\trace.bat`.

### Part 1 - static: `tools/check_fake_sfr.py` (every `trace.bat` run, both devices)

**Method.** The checker re-reads the header and the gld through the generator's own
parsers (`gen_fake_sfr.parse_header()`/`parse_gld()`, factored out for this purpose;
the generated files are byte-identical before and after that refactor), so what it
checks is exactly what the generator consumed - it never reads the generated `xc.h`.
It resolves every register of the ATDF to an absolute address and a name the header
would use (instance -> register-group -> register chain: `ADC3`/`AD` + `CH[0]` +
`CON1` = `AD3CH0CON1` at 0xB40 + 0x18 + 0x0; `TIMER1`/`T` + `CON` = `T1CON`; `CLOCK`/
`CLK[6]` + `CON` = `CLK6CON`), then for every `extern volatile uint32_t X` with a gld
address: finds the ATDF register by name (else by address), compares the address, the
ATDF's `size`, and every `_X_F_MASK` of the header with the ATDF bitfield of the same
name. An address that differs or a same-named field whose mask differs is an error
(exit 1, and `trace.bat` fails). Everything else is a count, listed with `-v`.

**Result, both devices, 0 errors.** MPS512: 2047 header SFRs, 1602 found in the ATDF
(1560 by name, 42 by address only), 445 with no ATDF counterpart; 1357 addresses agree
exactly, the other 245 sit in the five ADC `CH` groups (below); 9221 fields agree, none
disagrees. MPS506: 1997 SFRs, 1556 found, 441 without; 1311 + 245 addresses; 8607 fields.
**Every one of the 47 registers the golden traces write is verified by name, exact
address and every header field, on both devices.** Runtime 0.2 s per device (about
1 s each with Python's start-up); `trace.bat` went from ~7 s to ~10 s.

**What the ATDF does not say, or says differently** - none of it an error, all of it
handled explicitly in `resolve_atdf()` so that the check stays honest about what it
verified:

- **The element pitch of a counted register group.** The ATDF's `size` attribute of a
  counted group is the sum of its registers' sizes, not the distance between two
  elements: the ADC's `CH` group says 28 (7 registers), the gld and the pack's own
  `.PIC` file (`edc/dsPIC33AK512MPS512.PIC`, what MPLAB X and the simulator use) put
  `AD1CH1CON1` at 0x838 = 0x818 + 32. For DMA the two happen to coincide (44). For the
  five `ADn/CH` groups the check therefore demands element 0 exactly and one
  consistent pitch for the 49 later registers per core, and reports that pitch
  (`pitch gld 32, ATDF size 28`) - the ATDF confirms the order and spacing inside a
  channel and the base of channel 0, nothing more. `AD3CH0*`, the only channel
  registers the firmware touches, are verified exactly.
- **Counted groups without a `size`** (`CLK` 4..17, `PLL` 1..2, `VCO` 1..2, `CM` 1..4)
  carry their register offsets biased by `(count - start_index) * pitch` with the pitch
  nowhere stated (`CLK/CON` at 0x50 = 10 * 8, `PLL/CON` at 0xC = 1 * 12, `CM/CON` at 0x90
  = 3 * 0x30). The checker recovers the pitch from the group's smallest offset; every
  `CLKnCON/DIV`, `PLLnCON/DIV`, `VCOnDIV`, `CMnX` of both devices then matches the gld,
  which is what proves the recovery right. The OPA module's `AMP` group has the same
  bias on a non-counted group shared by three instances (`CON1` at 0x10 = (3-1) * 8;
  gld and `.PIC`: `AMP1CON1` at the instance's own base 0x3B08); removed the same way.
- **Two flattened modules, `ccp` and `CLC`.** One group lists every instance's registers
  under full names (`CCP1CON1` at 0x0 ... `CCP8BUF` at 0x170, then MCCP9's `CCP9CON1`
  restarting at 0x0) and all nine (ten) instances refer to that one group at their own
  bases - placed per instance, eight of the nine copies land on other peripherals
  (SCCP4's copy of `CCP8CON2` on `TMR1`, which is how this was found). Each instance's
  block is placed at that instance's base; what the ATDF verifies for these two modules
  is the instance bases and the order and spacing inside an instance, not its own
  inter-instance offsets. `SCCP1`'s registers, the ones `sccp.c` writes, are verified.
- **Registers the ATDF sizes as 2 bytes** where the header declares `uint32_t`: 181
  (MPS512) / 135 (MPS506), all GPIO - `PORTx/LATx/TRISx/ANSELx/ODCx/CNxx`, `RPINRn`,
  `RPORn`, `RPCON`, `IOIMnCON/BCON/STAT`. The ATDF gives the implemented width; the
  header's 32-bit declaration matches the 4-byte SFR pitch and is what the compiler
  uses. Reported, not an error; `LATD` (`nano`) is one of them and its address and
  field masks agree.
- **Header SFRs the ATDF has no register for**: 445 / 441. The CPU's (`PC`, `SPLIM`,
  `W0..`, `CORCON`, `MODCON`, `XBREV`, ...), `APG*` (116), the PAC's `PRnCTRL/ST/END/LOCK`,
  `ITC*`, `SMATH*/SDATA*`, `HPCCNT*`, `PMD1..4`, `CLK1DIV..CLK3DIV` (the ATDF's `CLK1..3`
  groups define only `CON`), ADC5's channels 8..15 (the ATDF's `AD` group has 8),
  `ADxCH7ACC` (the ATDF's `ACC` group, count 2 from index 6 with `size` 4, puts `ACC7`
  on `CH7CON1`'s address), `C1FIFOUA1..6`, `FEX2`, `BMXCAN*`, the RAM ECC registers, and
  whatever the `Ext_Interrupt` group reference points at (the ATDF's own
  `ext_interrupt` module does not define it). Unverifiable by this check, listed by
  `-v`; **none of them is written by any golden trace.**
- **42 found by address with a different name**: `ADxCH6ACC` (ATDF: `ACC6`), the BISS
  `B1*` registers (ATDF: `B*`), `C1FIFOUA7`/`C2FIFOUA7` (ATDF: `CnFIFOUA`). Addresses and
  fields agree.
- **Fields known to one side only** (no same-named field disagrees): header-only 297 /
  293 - ADC5's `STAT/RSTAT/SWTRG/CMPSTAT` bits for channels 8..15, `ADxCMPSTAT`'s
  `CHnCMP`/`CHnFLG` alias pairs, `PACCON1/2.IOIMCONnLK/WR`, per-generator bits of
  `CLKFAIL`/`SCSFAIL`, `IOIMnCON`; ATDF-only 375 - almost all the whole-register
  pseudo-field (`CRCDAT.CRCDAT`, `AD1CH0DATA.DATA`, mask 0xFFFFFFFF), plus `B1IDSn.RDATAn`
  bytes and `SPInBRG.SPI1BRG`.

**ATDF vs C header, where they disagree**: only in what is listed above - the counted
groups' pitch (the ATDF's `size` is not one), the 2-byte widths, the naming of the ADC
accumulators and the BISS registers, and coverage (445 registers, the one-sided fields).
No address and no mask of a field both sides name differs. The pack's `.PIC` file
agrees with the gld in every case that was looked up (`ADxCHy` pitch, `AMPn`, `CCPn`,
`DMAn`, `CLK6CON`, `PLL1DIV`, `CM4STAT`).

### Part 2 - dynamic: `sim_trap.py --smoke --dump-sfr` + `check_fake_sfr.py --sim-dump`

**Method.** `tools\build.bat smoke`, then `python tools\sim_trap.py --smoke --dump-sfr
@tests\trace\golden\boot.trace` (one smoke run: 70 s in total - MDB 8 s, programming
12 s, 21 s to `[smoke] DONE`, the rest `Print`s; expected.log matched, no trap): the 23
registers the `boot` golden writes are `Print`ed once after programming (the
simulator's reset values) and once after the halt at `[smoke] DONE`, into
`build\sfr_dump.txt`. `python tools\check_fake_sfr.py --sim-dump build\sfr_dump.txt
--trace tests\trace\golden\boot.trace` then classifies each register against the host
trace's end state, using the ATDF's `rw` attribute per field.

**What differs between the two boots, and is accounted for.** The smoke build boots
through `main.c`, the golden through the `boot` scenario's entry-point list: the
console's UART/PPS/TRIS writes (`console_early_init()`/`cli_init()`, stubbed in the
scenario) are not in the trace and not compared; `capture_set_pll(7, 7)` after
`capture_init()` is a no-op in the simulator build (`clock.c`: "no PLL to retune"), so
`PLL1DIV` stays at `clock_init()`'s 0x0100C829 exactly as in the golden; `dma.c` is
replaced by `sim_dma.c`, so the DMA channel and `IEC2` are never written in the
simulator ("not written"); the smoke script's `help`/`version`/`status` write no SFR.

| class | n | registers | what it means |
|---|---|---|---|
| agree | 7 | `VCO1DIV`, `VCO2DIV`, `IEC0`, `T1CON`, `PR1`, `AD3CH0CON1`, `AD3CH0CNT` | the simulator holds exactly the host's end value |
| status | 4 | `PLL1CON`, `PLL2CON`, `CLK1CON`, `CLK6CON` | the writable, non-switch bits agree (`ON`, `NOSC`, `FSCMEN`, `BOSC`...); the rest is `COSC`/`CLKRDY` (read-only: the simulator shows its own, the host model sets `CLKRDY`) and `OSWEN`/`DIVSWEN`/`FOUTSWEN`/`PLLSWEN` (the switch requests: hardware and the host model clear them when the switch is done, the simulator has no clock model and leaves them set) |
| not stored | 2 | `PLL1DIV`, `PLL2DIV` | the P0.3 finding made precise: the write reaches the register (it leaves its reset value 0x0100C812) but `POSTDIV1/2` and `PLLFBDIV` read back 0; only `PLLPRE` = 1 survives (0x01000000). A simulator model artefact, not a header one: the layout of the reset value itself (`PLLFBDIV` = 200, `POSTDIV` 2/2 in the header's field positions) agrees with the header |
| reset value | 1 | `AD3CON` | differs in `ADRDY` (read-only, set by the host model) and in `RPTCNT` = 18 (0x480000): the simulator's reset value, equal to the ATDF's `initval`, which `adc_init()`'s bit-field write of `ON` preserves and the host's all-0 reset assumption lacks. See below. **Since P0.9 `AD3CON` is class "status" (`ADRDY` only) and this class is a finding** |
| not written | 9 | `IEC2`, `DMACON`, `DMALOW`, `DMAHIGH`, `DMA0CH`, `DMA0SEL`, `DMA0SRC`, `DMA0DST`, `DMA0CNT` | `sim_dma.c` |
| DISAGREE | 0 | - | the only class that would have been a finding |

**The one substantive result** is `AD3CON`. On silicon the register reads 0x80488000
after `adc_init()`; the golden says `0x80000000 -> 0x80008000`. The trace is right
about the write (the `ON` bit, a bit-field store) and about the address; it is wrong
about the absolute value in a field the code never touches, because every SFR starts
at 0 on the host. Nothing in the drivers branches on `RPTCNT`, so no golden trace is
wrong in what it records, and none was changed. But the same mechanism would bite a
scenario whose code reads a field with a non-zero reset value (`clock_init()` reads
`CLK1CONbits.COSC`, reset 1 - the simulator shows `CLK1CON` = 0x101 at reset, the host
0x0; the `boot`/`clock` scenarios preset it to 0x80000000 by hand). The dump also
settles how trustworthy the ATDF's `initval` is: **all 23 simulator reset values equal
the ATDF's `initval`** (`PLL1CON` 0x20101, `PLL1DIV` 0x0100C812, `CLKnCON` 0x101,
`AD3CON` 0x480000, `DMA0CNT` 1, `PR1` 0xFFFFFFFF, the rest 0). Presetting `sfr_mem[]`
from `initval` (the "reset values" open point below) would therefore be a change of
known effect: `AD3CON`'s golden lines would become `0x80480000 -> 0x80488000`, and
`CLK1CON`'s hand preset would become unnecessary. **P0.9 did exactly that** (next
section) - with one correction to the last sentence: there never was a hand preset of
`CLK1CON`; the 0x80000000 the `boot`/`clock` goldens showed "at entry" is the model
rule's `CLKRDY` set-mask applied on `clock_init()`'s first read, and that rule stays.

**Limitation - what the simulator cannot confirm**: the DMA registers and `IEC2` (never
written in that build), the divider fields of `PLLxDIV` (zeroed by the model), and any
read-only or self-clearing bit (`COSC`, `CLKRDY`, `ADRDY`, `*SWEN`) - 11 of the 23
registers in full, 4 more in some of their bits. For those the static check against
the ATDF (part 1) is the only confirmation, and it is complete for every register a
golden trace writes.

## P0.9: reset values from the ATDF (27.09.2026, user decision after P0.8)

Until P0.9 `trace_begin()` zeroed every SFR. The device does not: P0.8's dump showed
`AD3CON` = 0x480000 (`RPTCNT` = 18), `PLL1DIV` = 0x0100C812, `PR1` = 0xFFFFFFFF at reset,
and every one of the 23 simulator reset values it read equal to the ATDF's `initval`.
So the generator now writes a reset table and the harness starts from it. No firmware
source changed.

**The table.** `tools/gen_fake_sfr.py` emits `sfr_reset[SFR_COUNT]` into `sfr_table.c`
(declared in `sfr_table.h`), one `initval` per SFR, matched to the ATDF register through
`check_fake_sfr.match_sfr()` - the same name-then-address rule the P0.8 static check
verifies every address and field mask with, factored out of `static_check()` for this
purpose - so every reset value comes from an ATDF entry that check has confirmed is the
same register. An SFR the ATDF has no register for keeps 0 and is marked `not in the
ATDF` in the table. `trace_begin()` copies the table into `sfr_mem[]` and the shadow
alike, so the preset itself is never a `W` line. `trace_build.py` counts the ATDF and
`check_fake_sfr.py` among the generator's inputs for its freshness check.

| device | SFR addresses | from `initval` | non-zero | by address only | not in the ATDF (kept 0) |
|---|---|---|---|---|---|
| MPS512 | 2039 | 1594 | 253 | 34 | 445 |
| MPS506 | 1989 | 1548 | 246 | 34 | 441 |

The 445 / 441 are the classes P0.8 listed (`check_fake_sfr.py -v`, "none"): `APG*`
(116), ADC5's channels 8..15 and the `ACC7`s (62), the PAC's `PRn*` (32), `ITC*` (71),
`SDATACMD*`/`SMATHCMD*` (32), `C1FIFOUA*`/`BMXCAN*`, `HPCCNT*`, `PMD1..4`,
`CLK1DIV..CLK3DIV`, `FEX*`, and the CPU's (`PC`, `SPLIM`, `CORCON`, `MODCON`, ...).
None of them is written by any golden (the static check's "48 registers ... 48 verified
by name" line: a `none` register cannot be verified).

**Hand presets: none removed.** The only value presets in any scenario are
`PLL1DIV = 0x0100C829` / `VCO1DIV = 0x20000` in `stream_on`, `stream_on_input`, `dac`
and `variants` - the state `clock_init()` leaves at boot, not the reset value
(0x0100C812 / 0) - and they stay. The `CLK1CON` "preset" P0.8 expected to become
unnecessary never was one (see the correction in the P0.8 section above); the `CLKRDY`
rule stays, and its effect now sits on top of the initval (`CLK1CON at entry:
0x80000101`).

**Every golden changed - 89 lines, every one of a foreseen kind**, checked line by
line with a classifier (pairs old and new by register, verifies that a changed old value
equals the register's previous value in the new trace - its reset value or its last
write, plus the model-rule bits the old line already carried - and that a changed new
value differs only in bits the old value also gained):

| scenario | (a) old/new gain reset bits | (b) write of the reset value vanished | (c) console text | (d) newly visible |
|---|---|---|---|---|
| `b2b` | 3 | | | |
| `boot` | 8 | 1 | 1 | 1 |
| `clk` | 4 | | | |
| `clock` | 6 | | 1 | |
| `dac` | 4 | | | |
| `fail` | | | 3 | |
| `nano` | 8 | 1 | 1 | 1 |
| `regs` | | | 15 | |
| `sccp` | 3 | | | |
| `stream_on` | 11 | 1 | | |
| `stream_on_input` | 8 | 1 | | |
| `timebase` | | 1 | | |
| `variants` | 6 | | | |
| total | 61 | 5 | 21 | 2 |

- **(a)**, 61 lines: whole-word writes keep their new value and their old value becomes
  the reset value (`PLL1CON 0x00020101 -> 0x00008100`, `CLK1CON 0x80000101 ->
  0x80129600`, `CCP1PR 0xFFFFFFFF -> 0x00000063`, `CCP1CON2 0x01000000 -> 0x00100000`,
  `DMA0CNT 0x00000001 -> 0x00000800`); bit-field writes gain the untouched reset bits
  on both sides (`AD3CON 0x80480000 -> 0x80488000` - exactly as P0.8 predicted;
  `PLL1DIV 0x0100C812 -> 0x0100C82D` in `b2b`, `PLLPRE`/`PLLFBDIV` kept while
  `capture_set_pll()` writes `POSTDIV1/2`; `DACCTRL1 0x3F7F0000 -> 0x3F7F8000`).
- **(b)**, 5 lines, one register - `W PR1 0x00000000 -> 0xFFFFFFFF` is gone from
  `boot`, `nano`, `stream_on`, `stream_on_input` and `timebase`: `timebase_init()`
  writes `PR1 = 0xFFFFFFFF`, which is `PR1`'s reset value, and a write that changes
  nothing is invisible to a state diff. **A loss of visibility, accepted and listed
  here rather than countered**: the store is one line of `timebase.c`, guarded by the
  three `T1CON`/`TMR1`/`IEC0` lines around it that remain, and a scenario contrivance
  (presetting `PR1` to something else first) would trade a real reset state for a
  fake one to see a write whose value the trace could not distinguish from "not
  written" anyway.
- **(c)**, 21 lines: `[clk] CLK1CON at entry: 0x80000101` (`boot`, `clock`; `nano`:
  0x80028180, see the open point on the MPS506's ATDF); `fail`'s `[CLKF]` dump
  (`OSCCTRL` 0x300, `PLL2CON` 0x20101, `CLK1CON` 0x101); `regs`' 15 lines, now a printed
  list of reset values (`PLL1DIV` 0x0100C812, `IPC9` 0x44444444, `DACCTRL1` 0x3F7F0000,
  `INTCON1` 0x8000, ..., and `DAC clock Hz (read back): 8000000` - `clock_dac_hz()`
  computed from `PLL1DIV`'s reset dividers instead of from zeros).
- **(d)**, 2 lines, the mirror image of (b) and not on the task's list of expected
  kinds, so stated separately: `W TRISC 0x0000FFFF -> 0x0000FEFF` (`boot`) and
  `W TRISD 0x000001FF -> 0x000001FE` (`nano`) - `led_init()` clearing the LED pin's
  TRIS bit. All TRIS bits are 1 at reset (inputs); under the all-0 assumption the
  clear changed nothing and was invisible. The trace now shows the one write of
  `led_init()` it never showed before. `TRISC`/`TRISD` join the golden-written set
  (47 -> 48 registers, all verified by the static check on both devices).

**Simulator agreement, re-run** (`tools\build.bat smoke` ELF unchanged, one
`sim_trap.py --smoke --dump-sfr @tests\trace\golden\boot.trace`, 68 s, `[smoke]
PASS`, no trap, then `check_fake_sfr.py --sim-dump build\sfr_dump.txt`): 24 registers
(`TRISC` now among them), **`initval` = simulator reset value for all 24**, `AD3CON` is
class "status" (`ADRDY` only - the P0.8 "reset value" finding is gone), `TRISC` "agree"
(0xFFFF at reset in the simulator too, 0xFEFF after), summary `agree 7, status 5, not
stored 2, reset value 0, not written 9, DISAGREE 0, INITVAL 0`. The dynamic check now
fails on a "reset value" row or on any `initval` that differs from the simulator's
reset (new "INITVAL" count), since either would mean the preset is wrong.

**Mutation check** (`CCP1RA = 0u` -> `1u` in `sccp.c`): exactly `sccp`, `stream_on`,
`stream_on_input`, `variants` FAIL, the other nine PASS; reverted, firmware diff empty.
Three consecutive full `trace.bat` runs byte-identical in their output (13/13 PASS, the
static check 0 errors on both devices). `hosttest.bat` 1/1 PASS.

## How to run it

    tools\trace.bat            build (incrementally) + run every scenario, compare
                                against golden traces
    tools\trace.bat record     (re)write every scenario's golden trace from what it just
                                produced - use once a task's comment says a trace is
                                expected to change, review the diff, then commit it
    tools\trace.bat clean      wipe the object cache and every generated header dir
                                first, then run the check above from cold - see
                                "caching and how to force a clean run" below

    python tools\check_fake_sfr.py [-v]        the P0.8 static check on its own (both
                                                devices; trace.bat runs it after the
                                                scenarios, its failure fails trace.bat)
    tools\build.bat smoke
    python tools\sim_trap.py --smoke --dump-sfr @tests\trace\golden\boot.trace
    python tools\check_fake_sfr.py --sim-dump build\sfr_dump.txt --trace tests\trace\golden\boot.trace
                                                the P0.8 dynamic check (one smoke-length
                                                simulator run, ~70 s; on request only,
                                                like every simulator run)

`tools\trace.bat` is a thin wrapper; the actual work is `tools/trace_build.py` (P0.5b -
see "problem 2" of the P0.5b task and the script's own docstring). It exists because the
original all-in-one-invocation-per-scenario `trace.bat` took **~4-5 minutes** for these 13
scenarios, each of whose executables runs in well under a second: measured, the dominant
cost was not per-scenario source count (a 1-file scenario and the 9-file
`stream_on(_input)` compiled+linked in about the same time, ~65-75 s cold) but `gcc`
itself paying a large fixed cost on every fresh invocation - a second, identical
invocation right after dropped to ~15 s. `trace_build.py` sidesteps that instead of
chasing it: it compiles each firmware/harness `.c` file to one `.o` under
`build\trace\obj\<flavor>\` and reuses that object for every scenario that needs the same
file compiled the same way (most firmware files are shared by several scenarios - `clock.c`
alone by seven), only rebuilding an object when its source, the fake header it was built
against, or any repo-root/`tests/trace/harness` header is newer than the object already
there; it also compiles and links scenarios in parallel. Measured on this host: **~247 s
-> ~28 s cold** (`clean`, cache empty), **~247 s -> ~7 s** when nothing changed, **~7 s**
after touching one firmware `.c` file (only the objects that actually depend on it, and
therefore the scenarios that link them, get rebuilt - proven with the mutation check
below, all comfortably inside the "well under 60 s" target).

**Caching and how to force a clean run.** The cache is `build\trace\obj\` (compiled
objects, one subdirectory per "flavor" - which generated header a scenario uses, crossed
with its own `NAME.cflags`) plus the generated header directories themselves
(`build\trace\gen`, `build\trace\gen_<mcu>`). Both are freshness-checked by file mtime
(source newer than object, or device-pack header/`gen_fake_sfr.py` newer than the
generated header, forces a rebuild of whatever depends on it) - there should never be a
reason to distrust it, but `tools\trace.bat clean` (or deleting `build\trace` by hand)
wipes it and rebuilds from cold if one ever is; `docs/IMPLEMENTATION-PLAN.md`'s P0.5b
entry and `tests/trace/README.md`'s own "Remaining open points" below are where to note
it if a stale object is ever actually found (none was, in the runs this task's commit
records).

A scenario with **no golden file at all** is still built and run (so a broken build or a
crash is still caught) and reported `NEW`, but only in `record` mode, where that is
expected - the golden is about to be written for the first time. **In the default (check)
mode, a missing golden is a FAIL, not a PASS**: since P0.5 every scenario has a golden
trace, so a missing one means nobody has ever recorded it, and silently passing would hide
exactly that (P0.4's original behaviour - reporting `NEW` in both modes - is what this
fixed; a scenario added without ever running `record` for it used to count as passing).
`trace.bat` also lists, after the per-scenario results, any `tests/trace/golden/*.trace`
with no matching `tests/trace/scenarios/*.c` (a rename or removal left behind a stale
golden) - reported as a warning, not counted against the PASS/FAIL total.

Two per-scenario override files, read by `trace_build.py` (not by the scenario's own
code):

- `NAME.cflags` - extra compiler flags, one per line, appended to that scenario's build
  only (`nano.cflags`: `-DBOARD=2 -DHAVE_CAPTURE`; every scenario linking capture.c needs
  `-DHAVE_CAPTURE`, `regs` also `-DHAVE_DIAG`, `stream_on(_input)` also `-DHAVE_CHAINTEST`
  - see "Stubs and compiler features" below for why). `trace_build.py` caches a firmware/
  harness object (everything except `stubs.c` and the scenario's own main) under a
  REDUCED key that drops any `-DHAVE_*` token, since none of those files ever test one -
  only `stubs.c`'s own `#ifndef` guards do - so e.g. `clock.c` compiles once per header
  flavor and is shared by every scenario that uses it, regardless of which `HAVE_*`
  combination that scenario also happens to pass.
- `NAME.mcu` - a device name (`nano.mcu`: `33AK512MPS506`). `trace_build.py` generates a
  SECOND fake header set for that device, into its own `build\trace\gen_<mcu>` (the
  default `gen` stays 33AK512MPS512 for every other scenario), and links that scenario
  against it instead - `tools/gen_fake_sfr.py` already took `--mcu` since the P0.3 spike;
  only the build engine needed to learn to use a second one alongside the default, and to
  regenerate it only when stale (the same freshness check as the default header).

Environment variables, read by the scenarios themselves, not by `trace_build.py`:

- `TRACE_HWMODEL=0` - the `clock` scenario skips starting the model (P0.5b: installing
  the page-guard hook), so `clock_init()` runs its first `WAIT_WHILE` into the bound and
  `fail(1)`s. Used to prove the hook matters, not committed as a golden trace (there
  would be nothing useful to diff against - the fail happens on line 1 of `clock_init()`,
  before it can do anything the golden trace would want to check).
- `TRACE_TIMEOUT_MS` - the runaway watchdog's timeout, default 5000.

Every scenario still runs in well under a second by itself; see "caching and how to force
a clean run" above for what `tools\trace.bat`'s own per-invocation cost now is.

## Remaining open points (P0.5 / P10 / P0.8)

- **Ordered writes for P10.** Add `trace_point()` calls between the individual steps of
  whatever `clock.c` split P10 produces, so each step's writes are captured on their own
  and the split cannot silently reorder two steps that used to be adjacent statements in
  one function.
- **CM4STAT.BUFV** (`clock_monitor_hz()`) and **AD4CON.ADRDY** (nothing selects ADC core
  4) are the only P0.4 open points P0.5 did not close - see the polling-loop table above
  and the golden-trace table's "not covered" note. Every other P0.4 open point
  (`ADxCON.ADRDY` for the cores actually used, the CLK7CON/CLK13CON.OSWEN/DIVSWEN/CLKRDY
  sites, TMR1 as a continuous count, `capture.c`'s static `dma_buffer` via
  `trace_region()`) is now answered - see "Golden traces (P0.5)" and the polling-loop
  table.
- ~~The hardware model's residual scheduling race~~ - **closed (P0.5b, 27.09.2026)**: the
  background thread that raced is gone, replaced by the page-guarded read hook (see "The
  page-guarded read hook" above and "History" for what it replaced). Every scenario now
  calls every entry point exactly once; if `tools\trace.bat` ever reports a `FAIL` that a
  second run does not reproduce, that would be new information (none of the runs behind
  this task's commit ever saw one, including 20 consecutive `dac` runs and 5 full check
  runs), and worth its own investigation rather than assumed to be this race again.
- ~~**Reset values from the ATDF** (`initval`)~~ - **done (P0.9, 27.09.2026)**, see
  "P0.9: reset values from the ATDF": every SFR the ATDF describes starts at its
  `initval`, the 24 registers the simulator dump reads all agree with it, `AD3CON`'s
  golden lines are `0x80480000 -> 0x80488000` now.
- **The MPS506 ATDF's `CLK1CON`/`CLK2CON`/`CLK3CON` initval** (found by P0.9): 0x28180
  (`NOSC` = 1, `ON` = 1, `BOSC` = 2, `COSC` = 0 and a bit in no named field, bit 7)
  where the MPS512's says 0x101 (`COSC` = 1, `NOSC` = 1 - FRC, what the simulator
  confirmed for the MPS512). The two headers lay the register out identically; 0x28180
  with `COSC` = 0 and a reserved bit set looks like an error in the MPS506's ATDF, not
  a silicon difference. Consequence today: the `nano` golden's "CLK1CON at entry" line
  and the old value of its `CLK1CON` W line carry 0x28180; `clock_init()` writes the
  whole word and its only branch on the entry state (`COSC` in the PLL range, 4..8)
  goes the same way for 0 and 1, so nothing else depends on it. An MPS506 simulator
  run (the smoke dump against the nano build; not done in P0.9 - `sim_trap.py --smoke`
  runs the MPS512 smoke ELF) would settle which value the simulator holds. The other
  seven initvals that differ between the two ATDFs are real package differences
  (`TRISA/B/C` and `ANSELA/B` 0xFFF instead of 0xFFFF, `TRISD` 0x1FF: fewer pins) and
  `IPC39` (0 instead of 0x444400).
- ~~**Simulator cross-check (P0.8)**~~ - **done (27.09.2026)**, see "P0.8 cross-check":
  0 errors static on both devices, 0 real disagreements dynamic; `trace.bat` now runs
  the static check every time.
