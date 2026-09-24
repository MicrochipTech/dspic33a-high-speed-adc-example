# Working on this repository with Claude Code

A bare-metal demonstration example for the dsPIC33AK512MPS512 on the EV74H48A
(dsPIC33 Curiosity Platform Development Board, GP DIM). What it is meant to show is
one sentence, and the whole repository is measured against it:

> **At a sample rate you choose, the ADC streams samples through the DMA into RAM
> continuously, and the CPU processes them on the free half of a ping-pong buffer.**

That sentence has four parts - *chosen rate*, *continuously*, *through the DMA*, *CPU
processes* - and they are not equally far along. Some are proven on silicon, one is
currently not met at any rate. Do not describe the example as working without reading
`docs/ANALYSIS.md` first.

## Read these before changing anything

| Document | What it is |
|---|---|
| `docs/ANALYSIS.md` | **Start here.** Sorted by question: what was checked, with what instrument, what that instrument can and cannot say, which earlier results were withdrawn and why, what holds and what is open. The fastest way into the state of the project. |
| `docs/HARDWARE-LOG.md` | The dated diary of every run on the board, in order, including the predictions that turned out wrong. |
| `README.md` | The user-facing document: what the example does, how to build it, how to drive the console. |
| `docs/TROUBLESHOOTING.md` | The debugging guide, symptom first. |

The register writes in the code cite the datasheet (DS70005591D) page or table they
come from. That convention is kept.

## Modules and who may touch what

| File | Owns | May call |
|---|---|---|
| `main.c` | start-up order, main loop | everything below |
| `board.h` | pins, ADC core/input, `ADC_PLL_POSTDIV1/2` (the boot rate, 7/7 = slowest), the DAC route constants | - |
| `config_bits.c` | every configuration word, with reasons | - |
| `clock.c/.h` | PLLs, clock generators, clock-fail interrupt, `clock_cpu_on_pll()`, `clock_adc_set_pll()`, `clock_adc_set_rate()`, the CLKGEN6 divider `clock_adc_set_div()` with its result codes, CLKGEN13 for the trigger | console, diag |
| `adc.c/.h` | the ADC core: init, burst trigger, PINSEL/SAMC, the trigger-source registers | console, diag |
| `dma.c/.h` | DMA channel 0: window = the buffer, HALF/DONE interrupt shell, status flags | console, diag; calls `dma0_event()` in capture.c |
| `sim_dma.c` | replaces `dma.c` in the simulator build; implements `dma.h` without a DMA | adc, capture, console |
| `capture.c/.h` | the measurement: the DMA buffer (private, with guard words), `dma0_event()`, the counters, start/stop/input, self-test, per-half processing, the variant table | adc, dma, sccp, led, console, diag |
| `sccp.c/.h` | SCCP1 as a trigger source, with clock source, mode and event as parameters | capture |
| `led.c/.h` | LED0 | - |
| `timebase.c/.h` | Timer1 as a stopwatch (12.5 MHz) for measuring the delivered rate. It sits on the CPU branch (PLL2) while the ADC is on PLL1, so it cannot flatter the ADC. Not involved in producing the rate. | - |
| `dac.c/.h` | DAC2 Triangle Wave mode, CLKGEN7 as its clock; the known signal | - |
| `dactest.c/.h` | captures N bursts and judges the last one against the DAC settings (min/max, reversals, largest step, period in samples) | capture, dac |
| `diag.c/.h` | `fail()` codes, trap handler, boot record in persistent RAM, `RCON` report, `regs_dump()` | every module's `*_regs_dump()` |
| `cli.c`, `console.h` | UART2, the commands, the `sweep`, the `test` suite, the variant matrix | clock, capture, dactest, led, diag |
| `sim.h` | the hooks the simulator build needs; all empty on silicon | - |
| `cmd_parser.c/.h` | the command parser, unchanged from github.com/zabooh/cmd_parser (Apache 2.0) - do not edit | - |

Nobody outside `dma.c` touches a DMA register, nobody outside `adc.c` an ADC register,
nobody outside `clock.c` reads `CLK1CON`. The console never reads the buffer directly;
it uses `capture_completed_half()`, or `capture_oneshot_n()` when it needs a window that
nothing is writing.

## Build and verify - every change, both variants

Command-line build (paths in `tools/build.bat`; `-mdfp` must point at the pack's `xc16`
subdirectory, `-T` at the linker script inside the pack):

```
tools\build.bat        hardware  -> build\adc_dma_40msps.elf/.hex   (must be -Wall -Wextra clean)
tools\build.bat sim    simulator -> build\adc_dma_40msps_sim.elf    (same)
```

`version.h` (git-ignored) carries the git revision for the banner. `tools/version.bat`
writes it before every build: called by `build.bat`, by the `.build-pre` hook in
`adc_dma_40msps.X/Makefile` (which the IDE runs, so a colleague's build gets it too)
and, for `tools/Makefile`, by `tools/version.sh`. `board.h` includes it through
`__has_include` and falls back to "unknown". Two pitfalls, both hit on 23.09.2026 with
MPLAB X 6.35: do **not** put the step into `configurations.xml`
(`makeCustomizationPreStep`) - the headless makefile generator then silently writes no
`Makefile-*.mk` at all; and in the hook use exactly
`cmd /c "$(subst /,\,$(CURDIR))\..\tools\version.bat"` - the IDE's make reports
`SHELL=sh.exe` without having one, and neither a quoted relative path nor
`cd ../tools &&` reached cmd intact (`'..' is not recognized`).

MPLAB X project: configurations `EV74H48A_Curiosity_Platform_MPS512` (the board,
PKOB4, `dma.c`) and `sim` (Simulator, `sim_dma.c`, `__MPLAB_DEBUGGER_SIMULATOR=1`);
hardware configurations are named after the evaluation kit, order number first, so that
the name in the IDE says what gets programmed. `tools\_test_mplabx.bat` builds the board
configuration from the command line through MPLAB X's own makefile generator. Two
pitfalls: the generator rewrites `languageToolchainVersion` in
`nbproject/configurations.xml` to whatever compiler it finds first - restore that one
line, never `git checkout` the whole file (that once threw away a file-list change); and
after any change to the file list delete `adc_dma_40msps.X/build` and `dist`, otherwise
make links stale objects and a missing entry goes unnoticed.

The simulator has no PLL, no ADC, no DMA and dispatches no interrupts (any pending
interrupt aborts with E0110). It proves the ping-pong buffer logic and nothing else. It
also runs `__delay32()` at a small fraction of real time: the 100 ms time-base check
(20 M cycles) took longer than the whole test budget and looked like a hang after the
self-test (24.09.2026, in `tools/sim_trap.py` and in the MPLAB X simulator alike), so
the simulator build delays 1 % of that. Keep every other long wait out of the simulator
path the same way.

```
tools\build.bat sim
python tools\sim_trap.py --run-seconds 420        expect "[simtest] PASS"  (about 7 minutes)
tools\build.bat sim 256 && python tools\sim_trap.py --elf build\adc_dma_40msps_sim256.elf --run-seconds 420
                                                  the same at 256 samples per half
python tools\sim_trap.py --fault 65536            expect "[simtest] FAIL", one mismatch at index 0
```

**Do not start the simulator acceptance run on your own.** It takes about seven minutes
of wall clock, and the user asked on 24.09.2026 that it only ever run when he says so.
Build the simulator variant to prove it still compiles; run it when asked.

## Rules

- Say what has and has not run on silicon. `docs/HARDWARE-LOG.md` is the record; add a
  dated entry for every board run and for every change made in reaction to one,
  including predictions that turned out wrong.
- A board run costs a person their afternoon. Before asking for one, make the firmware
  answer as many open questions as it can in a single pass.
- Every register value gets the datasheet table or page it comes from, in the comment
  next to it. A value copied from MCC or from Microchip's example says so. Where the
  device pack's ATDF and the datasheet disagree, the ATDF has been right every time -
  see the SCCP trigger code below.
- Status flags are cleared by writing the word once with 0 only in the bits to clear
  (`dma0_clear()`), never with bit-field read-modify-write; interrupt flags are cleared
  at the start of the handler, not the end. Both were real bugs on the board.
- Two configuration-bit names are pack-version dependent (`FICD_NOBTSWP`,
  `FWDT_RCLKSEL`) and are written as numbers. Keep it that way.
- Console output for humans goes through `console_puts()`/`console_kv()`; command replies
  through the parser's sink. A line longer than a buffer is a stack overrun: size buffers
  from the longest possible line and say so in the comment.
- Commit messages: what changed, why, what was verified. **No attribution trailers of any
  kind**, in this repository without exception.
- Do not edit `cmd_parser.c/.h` - with one deliberate exception:
  `CMD_PARSER_MAX_COMMANDS` is 24 instead of upstream's 16 (24.09.2026, for the 17th to
  19th commands `core`, `dac`, `dactest`; 32 bytes of RAM). When updating the parser from
  github.com/zabooh/cmd_parser, re-apply that one line.

## How the example is built, and what that costs

**Back-to-back is the working path.** The ADC runs in Integration mode (`MODE = 2`) with
`CNT` conversions per burst, `TRG1SRC = 1` (software start) and `TRG2SRC = 2`
(back-to-back), `IRQSEL = 0` so that every conversion raises the event the DMA triggers
on. The DMA is channel 0, Repeated Continuous, its address window exactly the buffer,
with HALF and DONE interrupts and guard words behind the buffer.

**The rate comes from PLL1, not from the CLKGEN6 divider.** `capture_set_pll(p1, p2)`
sets PLL1's two output dividers; the ADC clock is 1600 MHz / (p1 * p2), p1 >= p2, both
1..7, which is 40 down to 4.08 MSPS with 5/5 = 8 MSPS exactly. `clock_adc_set_rate()`
searches `PLLFBDIV` (63..200, VCO 500-1600 MHz) on top of that gear for a finer step.
PLL1 feeds nothing but the ADC path, and its output-divider switch is what
`clock_init()` does at every boot, so the mechanism is known to work on this silicon.
What is **not** settled is whether it reaches the converter during continuous streaming -
see the open questions.

The CLKGEN6 divider (`capture_set_clkdiv()`, the `clk` command) is kept for the record
only. Every ratio was written, read back and confirmed by `DIVSWEN` and `CLKRDY` - with
the generator switched off around the write and with it left running as Example 12-2
prescribes - and the rate never changed. **Do not build a rate on it.**

Both switches run in the boot order and report the step that failed rather than a bool:
DMA channel down, ADC core off, clock changed and read back, core on, DMA from scratch.

**The pacing variants are in the code again, as a matrix, not as a claim.** They were
deleted after run 7 and brought back on 24.09.2026, because the reason they had failed
turned out to be three errors of ours rather than the silicon:

- the SCCP trigger code was "corrected" from 32 to 34 on the strength of datasheet
  Table 16-4; the pack's ATDF names `0x20` = 32 "SCCP1 OC/IC Event" and `0x22` = 34
  **SCCP3**, so the ADC was told to listen to a module nothing had configured;
- `CCP1CON2.AUXOUT` carried the timer rollover (1) instead of the special event trigger
  (**2**), which is what the ADC's trigger input expects;
- the module was clocked from the peripheral clock (PLL2, `CLKSEL = 0`) while the ADC
  runs off PLL1; it must be CLKGEN13 (`CLKSEL = 1`).

Each one alone was enough for "no conversion at all", which is exactly what runs 5 to 7
reported. **The triggered family has therefore never really been tested.** `capture.h`'s
`capture_variant_t` lists all of them and `test matrix` walks them.

**Nothing runs by itself.** The firmware boots, brings the console up, sets the slowest
rate (`ADC_PLL_POSTDIV1/2` = 7/7) and waits: `[boot] READY - nothing is converting`.
Everything else is typed. The reason for the idle boot is that through seven runs the
console never received a byte (`rx = 0`) and it could not be told whether the bytes never
arrived or whether the receive interrupt (priority 1) was starved behind the DMA
interrupt (priority 4, over a million entries per second). With nothing converting, that
question answers itself - and it did: the console is fine.

**The sweep runs from the slowest rate up**, not from the fastest down. The slowest point
is the one the DMA should manage, so the first row is the row most likely to pass - and a
failure there means the chain is broken, not the rate. The ladder mixes integer and
fractional ratios on purpose: a row that lands halfway between its neighbours proves the
fractional part works, one that snaps to a neighbour proves it is ignored.

## The test suite

`test` on its own lists the parts. `test all` is the whole examination in one pass,
because a board run costs a person:

| Subcommand | The question it answers |
|---|---|
| `test self` | is the chain wired up (it samples a constant, so it cannot tell a working converter from a frozen result register) |
| `test clock` | do divider writes arrive - and only that; the clock that comes out is a different question |
| `test clkoff` | does the ADC still convert with CLKGEN6 switched off entirely |
| `test bursts` | **does the rate depend on how many bursts run** - 1, 10 and 100 at the same setting, the bridge between "one burst follows the setting" and "a thousand do not" |
| `test sweep` | the rate ladder with all counters per row |
| `test rate` | one rate point, measured |
| `test matrix` | every variant: does it convert, does the rate follow, **does it stream** (the acceptance test), are the data intact |
| `test dac` | the DAC triangle through the internal UREF route |

`matrix_stream()` in `cli.c` is the acceptance test and it is the one that matters: the
stream runs for `MATRIX_STREAM_HALVES` halves with `capture_service()` called throughout,
exactly as the main loop does, and it passes only when `overrun`, `late` and `missed` are
all zero. `missed` above zero means the CPU never saw a half, which is precisely the
failure the example must not have. The matrix ends either with `USE THIS ONE: <variant>`
or with the plain statement that none of them streams cleanly.

The DAC test is the only instrument that looks at data rather than counting events. It
goes through the chip, not over a pin: `UREFCON.INSEL = 7` puts DAC2 on the internal
reference line, which every ADC core can read as `ANn7`. It runs twice per variant - as
an isolated burst and as the 100th burst of a stream - because the triangle's period in
seconds is a property of the DAC and cannot change. If the measured rate rises by ten and
the period in samples rises by ten as well, the samples in the stream are repeats and the
converter never sped up.

## What the board has settled (as of run 16, 24.09.2026)

- **The chain carries data, complete and in order.** Run 14 captured the DAC triangle
  through UREF: a clean monotonic fall from 3728 to 629, no reversal, largest step 90
  counts out of a swing of 3240 against a limit of 405. Across the half boundary and
  across the burst restart.
- **A single burst runs at the rate the PLL is set to**, better than 0.5 % over all
  fourteen settings from 4 to 40 MSPS, with a constant residual of 4.2-5.3 us that a
  rate-dependent error could not have produced.
- **The counters are honest.** Run 16: `half + done = 2 x bursts` in every row. There is
  no double booking.
- **The console works.** `rx = 0` in runs 6 and 7 was interrupt starvation.
- **The CLKGEN6 divider does not reach the converter**, with either switching sequence,
  and the ADC keeps converting with the generator switched off.
- **Every rate measured under load in runs 4 to 11 is wrong** by about a factor of ten -
  it was taken by a CPU drowning in the overrun interrupt. Do not quote those numbers.
- **`dma_overrun` is a lower bound, not a count of lost samples.** `OVERRUN` is one bit
  in `DMA0STAT` and the handler increments once per entry in which it finds it set;
  several losses between two entries count as one.

## Open questions (as of 24.09.2026)

1. **Is the rate selectable in continuous streaming?** This is the example's central
   claim and it is not settled. A single burst follows the setting; a thousand bursts
   delivered about 40 MSPS at every setting, three per cent apart while the setting spans
   a factor of ten. Two readings remain: the converter really runs flat out under
   streaming, or each conversion lands in the buffer more than once - which Microchip
   acknowledges for this silicon ("ADC triggers for DMA on this device have an issue. A
   few transfers are possible per one trigger."). `test bursts` and the twice-run DAC
   test are built to decide it.
2. **Can any variant stream without loss while the CPU processes?** No variant has done
   so in any run. At every rate the sweep walks, 1614 to 1977 of 2000 halves were missed
   by the main loop, because every lost sample raises the DMA channel interrupt and that
   event has no enable bit of its own (DS70005591D 13.6.1 - `DMA0CH` has HALFEN, DONEEN
   and MATCHEN only). The handler brakes itself past `OVERRUN_LIMIT` and reports the rate
   as unusable. **Until one variant streams cleanly, the example does not demonstrate
   what it says it demonstrates, and that belongs on the console rather than in a
   footnote.**
3. **Why does the ADC ignore CLKGEN6?** Table 16-1 names it as the clock source, its
   divider has no effect in either switching sequence, and the converter keeps running
   with the generator off. A question for the product line; the register evidence is
   complete in the log.
4. **`dac2_period_ns()` is wrong.** The computed triangle period is about eight times off
   what a capture shows, and `DACLOW` is not reproduced - the upper end matches `DACDAT`
   exactly, the lower end does not. Nothing is judged against it any more, but it should
   be corrected.
