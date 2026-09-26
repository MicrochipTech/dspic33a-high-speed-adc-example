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
- The model thread spins with no `Sleep()` at all (P0.4 called `Sleep(0)` once per
  iteration; P0.5 removed it - see "a residual race" below) so it does not depend on a
  scheduler quantum to be re-dispatched; on any machine with more than one logical core
  it and the driver thread simply run in parallel, which is why the 20 runs above never
  took a scheduler quantum into account and still came back instantly (see "runtime"
  below).

### The hardware model - a residual race, and how P0.5 lives with it

P0.4's two scenarios only ever waited on `clock_init()`'s and `adc_init()`'s bound
(`WAIT_LIMIT`, 2 000 000 iterations, `diag.h`) and were never observed to fail across the
15+5 runs the spike/P0.4 recorded. P0.5 adds scenarios that wait on a *shorter* bound
(`DIVSW_WAIT_LIMIT`, 100 000 iterations, `clock.c`: `clock_dac_on()`, `clock_trig_on()`,
`clock_adc_set_pll()`/`_set_rate()`/`_set_div()`'s per-step waits) - and that shorter wait
turned out to race the model thread's own start-up on this development host (14 logical
cores): a throwaway repro (`dac`'s `clock_dac_on()` call, the shortest wait in the
firmware) failed 50-95 times per 100 runs with the P0.4 `hwmodel.c` unchanged. The cause,
found by measuring rather than guessing: `CreateThread()`'s own latency (a brand new
thread's first scheduling, page faults for its stack, ...) can exceed the entire
100 000-iteration window, so the model thread had sometimes not executed a single
instruction by the time the driver's wait gave up.

`hwmodel.c` now, in order, for every fix that measurably helped and was kept:

1. **Waits for proof the thread has started** - an auto-reset event the thread sets as
   its first statement, `hwmodel_start()` blocks on it before returning. Brought the
   failure rate from 50-95/100 to about 1/100.
2. **Pins the model thread and the calling thread onto different logical cores**
   (`GetProcessAffinityMask`/`SetThreadAffinityMask`, restored in `hwmodel_stop()`), so
   the two never contend for the same core. Best effort: collapses to a no-op on a
   single-core host.
3. **Waits for proof of live THROUGHPUT, not just existence** - a heartbeat counter the
   model bumps every loop iteration; `hwmodel_start()` spins (bounded by wall clock only,
   `GetTickCount64`, not by a fixed iteration count) until it has advanced by a margin
   since the ready event fired.

None of the three, alone or together, closed the race on this host: measured repeatedly
at roughly 1 failure per 100-200 runs even with all three in place (a `Sleep(1)` instead
of the busy-wait in step 3 made it markedly *worse*, 30-46 failures per 150 - `Sleep(0)`'s
"yield only to equal-or-higher-priority threads" semantics interacting with the model's
now-continuous spin is the suspect, not chased further). **What actually makes the
golden traces reproducible is a fourth layer, at the scenario level, not in `hwmodel.c`
at all**: every scenario that drives a `DIVSW_WAIT_LIMIT`-bound function retries it
silently - no `trace_point()`/`trace_note()` between attempts - up to 5 times if it
returns failure (`dac.c`, `sccp.c`, `b2b.c`, `clk.c`, `variants.c`,
`stream_on(_input).c`). This is not a hack bolted onto approach (a); it follows directly
from what approach (a) already throws away: **a golden trace only ever shows the *net*
state at the next trace point**, so it cannot tell a clean first-attempt success from one
that needed a retry first - the two are, by construction, indistinguishable in the trace
format this project chose on 26.09.2026. Five retries at a roughly 1-4% single-attempt
failure rate (higher for `stream_on(_input)`, which chains three or four such waits in
one call) bring the chance of a golden trace ever being affected by this race to a
practically negligible level, without touching how `hwmodel.c` answers any individual
wait.

`boot`, `nano`, `fail`, `regs` need none of this: they only reach `WAIT_LIMIT`-bound
waits (`clock_init()`, `adc_init()`), which were never observed to race on this host, and
are not retried.

**`variants.c` needed a different shape of retry, found the hard way.**
`capture_select_variant()`'s own preamble (capture.c) discards
`clock_adc_set_div()`/`clock_adc_set_pll()`'s return codes
(`(void)clock_adc_set_div(100u); (void)clock_adc_set_pll(5u, 1u);`) before running the
per-variant switch - so the function's own return value does not reliably say whether
that preamble's clock switch raced. Observed once, in a five-run check: `variants`
`FAIL`ed with `PLL1CON 0x0 -> 0x10000000` (FOUTSWEN stuck) and
`CLK6CON 0x80000000 -> 0x80400000` (DIVSWEN stuck) sitting in the trace, for a call that
had returned `true` - "retry until it returns true" never triggered, because there was
nothing to retry against. The fix is not a bigger retry count on the same condition: it
is calling `capture_select_variant()` three times **unconditionally** per variant,
keeping only the last call's result - each call redoes the same preamble from scratch, so
even a call whose *return value* said nothing was wrong still gets another, fully-warmed
chance to leave the clock in a clean state, and a clean run's trace is unaffected (a
second identical call is an idempotent no-op diff). Worth remembering for any *other*
scenario built later on a function with a similarly discarded internal return code: check
that "retry on failure" can actually see the failure before trusting it.

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
| `stream_on` | `chain_stream_on(1000)` (1 MSPS), `chain_stream_off()` | chaintest, capture, adc, dma, clock, sccp, dac, timebase, led | PLL1CON, OSCCTRL(PLL1RDY), CLK6CON, CLK7CON, CLK13CON, AD5CON.ADRDY (CHAIN_CORE), AD3CON.ADRDY (`restore()`'s default core); `tmr1_step=10000` (`wait_ticks()`, the two 25-tick waits) | PLL1DIV/VCO1DIV preset to their `clock_init()` boot values (setup() only rewrites PLL1's POSTDIV1/2; `clock_dac_hz()` needs PLLFBDIV/VCO1DIV already set or the triangle is refused) - `clock_init()` itself is `boot`'s/`clock`'s job. `chain_stream_on(1000)` retried up to 5x (silent) - see "a residual race" |
| `stream_on_input` | `chain_stream_on_input(1000, core=2, pinsel=7, samc=1, test_signal=false)`, `chain_stream_off()` | same as `stream_on` | PLL1CON, OSCCTRL(PLL1RDY), CLK6CON, CLK13CON, AD2CON.ADRDY (the input asked for), AD3CON.ADRDY (`restore()`); `tmr1_step=10000` | `test_signal=false` (the GUI's real shape for a custom input, CLAUDE.md: "the DAC is then left alone") skips CLKGEN7/the DAC entirely - no CLK7CON rule needed. Same PLL1DIV/VCO1DIV preset and retry as `stream_on` |
| `b2b` | `capture_set_pll(5, 5)`, `capture_start()`, `capture_stop()` | capture, adc, dma, clock, sccp, timebase, led | PLL1CON, OSCCTRL(PLL1RDY), CLK6CON, AD3CON.ADRDY | `capture_start()` triggers `capture_init()`/`dma0_init()` itself (first call, `dma_armed` starts false) - no separate call needed. No ISR ever fires (decision 2), so this traces the three control calls, not a completed capture. `capture_set_pll()` retried up to 5x (silent) |
| `variants` | `capture_select_variant()` for all ten `capture_variant_t` values, want_ksps = 100 | capture, adc, dma, clock, sccp, timebase, led | PLL1CON, OSCCTRL(PLL1RDY), CLK6CON, CLK13CON, AD3CON.ADRDY | want_ksps = 100, not a rounder number: the SCCP-clocked variants compute `ticks = hz/1000/want_ksps` and refuse below 2 - at 8000 with no `clock_init()` run first (peripheral clock still the 4 MHz FRC/2), every SCCP variant failed (`ticks` truncated to 0); 100 keeps all ten comfortably above that floor. PLL1DIV preset to its boot value (PLLFBDIV/PLLPRE) for the same reason as `stream_on`. Each call made three times UNCONDITIONALLY (not "until true" like every other scenario) - `capture_select_variant()`'s own preamble discards `clock_adc_set_div()`/`clock_adc_set_pll()`'s return codes, so a racy wait in there can leave a stray bit in PLL1CON/CLK6CON while the function still returns true; see variants.c's own comment and "hardware model - a residual race" |
| `dac` | the DAC2 triangle exactly as `chain all`'s stage 8 sets it (`triangle_for(8000000u, &slp)` reproduced: slp=18, low=0xFF, high=0xF00), `dac2_off()` | dac, clock, timebase | CLK7CON (`clock_dac_on()`) | `triangle_for()` itself is `static` in chaintest.c, not linkable alone - its arithmetic is reproduced instead (see dac.c's own comment) and called through the public `dac2_triangle_start()`. PLL1DIV/VCO1DIV preset to their `clock_init()` boot values (`clock_dac_hz()` needs them). `dac2_triangle_start()` retried up to 5x (silent) |
| `sccp` | `sccp1_start()` for every (clock, mode, event) combination the firmware uses: (PERIPHERAL,TIMER,SPECIAL), (GEN13,TIMER,SPECIAL), (PERIPHERAL,OC,SPECIAL), (GEN13,OC,SPECIAL), (PERIPHERAL,TIMER,ROLLOVER) | sccp, clock, timebase | CLK13CON (`clock_trig_on()`) | `CAP_VAR_SCCP_TRG2` uses the same (clk,mode,ev) tuple as `CAP_VAR_SCCP_T_G13` - five distinct tuples cover all six SCCP variants. `clock_trig_on()` retried up to 5x (silent) |
| `clk` | `capture_set_clkdiv(500)`, `clock_adc_set_rate()` for 4000/8000/40000 ksps | capture, adc, dma, clock, sccp, timebase, led | PLL1CON, OSCCTRL(PLL1RDY), CLK6CON, AD3CON.ADRDY | Not `clock_init()` first - neither function needs a rate already configured, both derive their result purely from the argument. Every call retried up to 5x (silent) |
| `fail` | `_CLKFInterrupt()` called directly | clock, timebase | none (`capture_halt()`/`console_force_up()` resolve to stubs, see below) | `capture_halt()` and `console_force_up()`: neither is "linkable" here without pulling in unrelated modules just for one line each (capture.c + adc/dma/sccp/led for `dma0_halt()`; cli.c, categorically excluded, for the other) - both stubbed, per the task's "use the real ones where linkable, stubs otherwise". diag.c is NOT linked: its real `fail()` never returns (blinks forever), which would hang this scenario until the watchdog kills it - `_CLKFInterrupt()`'s `fail(10u)` resolves to the stub (`longjmp` back) instead |
| `regs` | `regs_dump()`, called from the reset state (every SFR 0) | diag, clock, adc, dma, capture, dac, sccp, timebase, led | none (nothing here waits) | The only scenario that links diag.c - its real `fail_code`/`boot_stage`/`chain_mark`/`fail()` take over from stubs.c's copies (`-DHAVE_DIAG`); nothing here calls `fail()`, so diag.c's real, never-returning one is never exercised, only linked. `console_sync_baud()`/`console_regs_dump()` (cli.c-only, referenced by diag.c's `fail()`/`regs_dump()`) are stubbed. Registers are at their reset assumption throughout - this scenario is about the dump's OWN output format/order, not about reproducing a booted system's values (`boot`/`b2b`/`variants`/`clk`/`stream_on(_input)` do that) |
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

| Where | Waits on | Bit is | Status (P0.5) |
|---|---|---|---|
| clock.c 107, 163 | `CLK1CON.OSWEN` | self-clearing | **answered** (`clock`/`boot`/`nano` scenarios' rules) |
| clock.c 115/138, 590, 641 | `PLLxCON.PLLSWEN` | self-clearing | **answered** (`clock`/`boot`/`nano`: 115/138; `clk`/`b2b`/`variants`/`dac`/`stream_on(_input)`: 590/641, `clock_adc_set_pll`/`_set_rate`) |
| clock.c 117/140, 594 | `PLLxCON.FOUTSWEN` | self-clearing | **answered** (as above) |
| clock.c 119/142 | `PLLxCON.OSWEN` | self-clearing | **answered** |
| clock.c 130/150 | `PLLxCON.DIVSWEN` | self-clearing | **answered** |
| clock.c 120/143, 597, 644 | `OSCCTRL.PLLxRDY` | set by hardware (lock) | **answered** |
| clock.c 175, 350, 398 | `CLK6/7/13CON.OSWEN` | self-clearing | **answered**, all three: CLK6CON (`clock`/`boot`/`clk`/`b2b`/`variants`/`nano`), CLK7CON (`dac`, `clock_dac_on()`), CLK13CON (`sccp`/`variants`/`stream_on(_input)`, `clock_trig_on()`) - the P0.4 open point closed |
| clock.c 274, 402 | `CLK6/13CON.DIVSWEN` | self-clearing | **answered**: CLK6CON (`clk`'s `capture_set_clkdiv()`, `variants`' `CAP_VAR_CLKDIV`), CLK13CON (`sccp`/`variants`/`stream_on(_input)`) |
| clock.c 277, 315, 353, 405, 600, 647 | `CLKxCON.CLKRDY` | set by hardware | **answered**: CLK1CON (`clock`/`boot`/`nano`), CLK6CON (all of the above), CLK7CON (`dac`, `stream_on(_input)`), CLK13CON (`sccp`/`variants`/`stream_on(_input)`) |
| clock.c 493 | `CM4STAT.BUFV` | set by hardware (window done) | **still open**: no P0.5 scenario calls `clock_monitor_hz()` (only `chain all`'s stage 8 does, out of scope - see the scenario table below) |
| adc.c 131, 287 | `ADxCON.ADRDY` (via `adc_cur` pointer) | set by hardware after `ON` | **answered**: AD1CON (`nano`), AD2CON (`stream_on_input`), AD3CON (`boot`/`clk`/`b2b`/`variants`), AD5CON (`stream_on`) - the P0.4 open point closed for every core a P0.5 scenario actually selects; AD4CON stays open (nothing selects core 4) |
| cli.c 152, 164, 221, 256, 280 | `U2STAT` bits | hardware | moot: `cli.c` is not linked into any scenario (decision above) |
| capture.c 1130, 1154; chaintest.c 188; clock.c 519 (sim path only) | `TMR1` via `timebase_ticks()` | counts | **answered**: `stream_on`/`stream_on_input` pass `tmr1_step = 10000` to `hwmodel_start()` (`chain_stream_on(_input)`'s `wait_ticks()`, `capture_chain_stop()`'s/`capture_chain_halt()`'s 25-tick waits) - the P0.4 open point closed; every other scenario still relies only on `__delay32()`'s own TMR1 advance |
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
- It is **Windows/x86-64 specific**: the hardware model thread, its `CRITICAL_SECTION`,
  the runaway watchdog and the P0.5 race mitigations are all Win32 API (`CreateThread`,
  `WaitForSingleObject`, `CreateEvent`/`SetEvent`, `GetTickCount64`,
  `GetProcessAffinityMask`/`SetThreadAffinityMask`). A Linux port would use
  `pthread_create`, a `pthread_mutex_t`, a `pthread_cond_t` and `sched_setaffinity`; not
  needed today.

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

Two per-scenario override files, read by `trace.bat` itself (not by the scenario's own
code):

- `NAME.cflags` - extra compiler flags, one per line, appended to that scenario's build
  only (`nano.cflags`: `-DBOARD=2 -DHAVE_CAPTURE`; every scenario linking capture.c needs
  `-DHAVE_CAPTURE`, `regs` also `-DHAVE_DIAG`, `stream_on(_input)` also `-DHAVE_CHAINTEST`
  - see "Stubs and compiler features" below for why).
- `NAME.mcu` - a device name (`nano.mcu`: `33AK512MPS506`). `trace.bat` generates a
  SECOND fake header set for that device, into its own `build\trace\gen_<mcu>` (the
  default `gen` stays 33AK512MPS512 for every other scenario), and links that scenario
  against it instead - `tools/gen_fake_sfr.py` already took `--mcu` since the P0.3 spike;
  only `trace.bat` needed to learn to use a second one alongside the default.

Environment variables, read by the scenarios themselves, not by `trace.bat`:

- `TRACE_HWMODEL=0` - the `clock` scenario skips starting the hardware model, so
  `clock_init()` runs its first `WAIT_WHILE` into the bound and `fail(1)`s. Used to
  prove the model matters, not committed as a golden trace (there would be nothing
  useful to diff against - the fail happens on line 1 of `clock_init()`, before it can
  do anything the golden trace would want to check).
- `TRACE_TIMEOUT_MS` - the runaway watchdog's timeout, default 5000.

Every scenario still runs in well under a second by itself; `tools\trace.bat`'s own
per-invocation cost (regenerating the fake header(s), rebuilding all thirteen scenarios
from scratch every time - there is no incremental build) is on the order of a minute,
dominated by the two scenarios that link the most firmware (`stream_on(_input)`, nine
files each).

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
- **The hardware model's residual scheduling race** (a `DIVSW_WAIT_LIMIT`-bound wait can
  still occasionally outrun the model on this host, about 1 per 100-200 attempts) is
  mitigated, not eliminated - see "a residual race" above. A future host or toolchain
  might make it worse or better; if `tools\trace.bat` ever reports a `FAIL` that a second
  run does not reproduce, this is where to look first, and raising the retry count in the
  affected scenario(s) is the first thing to try before re-opening `hwmodel.c`.
- **Reset values from the ATDF** (`initval`), for scenarios whose code branches on a
  register's state at entry (`clock_init()` reads `CLK1CONbits.COSC`) - still assumed
  all-0, as in the spike.
- **Simulator cross-check (P0.8)**, unchanged from the spike's findings above.
