# Working on this repository with Claude Code

Bare-metal example for the dsPIC33AK512MPS512 on the EV74H48A (dsPIC33 Curiosity
Platform Development Board, GP DIM): one ADC core at 40 MSPS, DMA into a double
buffer, counters for every error the hardware reports, a console. It exists to
measure what the device really sustains, for a customer evaluation. `README.md` is
the user-facing document, `docs/TROUBLESHOOTING.md` the debugging guide,
`docs/HARDWARE-LOG.md` the dated record of every run on the board. Read those three
before changing anything; the register writes in the code cite the datasheet
(DS70005591D) page they come from, and that convention is kept.

## Modules and who may touch what

| File | Owns | May call |
|---|---|---|
| `main.c` | start-up order, main loop | everything below |
| `board.h` | two board profiles selected by `BOARD` (EV74H48A with the MPS512 DIM, default; EV17P63A Curiosity Nano with the MPS506): pins, ADC core/input, `ADC_CLKDIV` (the boot sample rate) | – |
| `config_bits.c` | every configuration word, with reasons | – |
| `clock.c/.h` | PLLs, clock generators, clock-fail interrupt, `clock_cpu_on_pll()`, the ADC clock divider `clock_adc_set_div()` with its result codes | console, diag |
| `adc.c/.h` | the ADC core: init, burst trigger, PINSEL/SAMC register | console, diag |
| `dma.c/.h` | DMA channel 0: window = the buffer, HALF/DONE interrupt shell, status flags | console, diag; calls `dma0_event()` in capture.c |
| `sim_dma.c` | replaces `dma.c` in the simulator build; implements `dma.h` without a DMA | adc, capture, console |
| `capture.c/.h` | the measurement: the DMA buffer (private, with guard words), `dma0_event()`, counters, start/stop/input, self-test, per-half processing | adc, dma, led, console, diag |
| `led.c/.h` | LED0 | – |
| `timebase.c/.h` | Timer1 as a stopwatch (12.5 MHz) for measuring the delivered sample rate; not involved in producing it | – |
| `dac.c/.h` | DAC1 and DAC2 in Triangle Wave mode (DACOUT1 = RA1, DACOUT2 = RA8), one unit table, shared CLKGEN7, plus the internal UREF route to any core; the known signal for `test dac` | – |
| `dactest.c/.h` | judges captured halves against the DAC settings (min/max, reversals vs period, jumps) | – |
| `diag.c/.h` | `fail()` codes, trap handler, boot record in persistent RAM, `RCON` report, `regs_dump()` | every module's `*_regs_dump()` |
| `cli.c`, `console.h` | UART2, the commands, the `sweep` | clock, capture, led, diag |
| `sim.h` | the hooks the simulator build needs; all empty on silicon | – |
| `tools/adc_gui.py` | NiceGUI front end: sets rate/pacing/SAMC/input over the console, captures a buffer half per cycle, plots time signal and FFT; `--fake` uses a built-in stand-in, `--selftest` runs the pipeline without GUI. `tools/gui_setup.bat` makes its venv (`tools/.venv`, ignored) | the console protocol only |
| `cmd_parser.c/.h` | the command parser, unchanged from github.com/zabooh/cmd_parser (Apache 2.0) – do not edit | – |

Nobody outside `dma.c` touches a DMA register, nobody outside `adc.c` an ADC register,
nobody outside `clock.c` reads `CLK1CON`. The console never reads the buffer directly;
it uses `capture_completed_half()`.

## Build and verify - every change, both variants

Command-line build (paths in `tools/build.bat`; `-mdfp` must point at the pack's `xc16`
subdirectory, `-T` at the linker script inside the pack):

```
tools\build.bat        hardware  -> build\adc_dma_40msps.elf/.hex   (must be -Wall -Wextra clean)
tools\build.bat sim    simulator -> build\adc_dma_40msps_sim.elf    (same)
```

`version.h` (git-ignored) carries the git revision for the banner. `tools/version.bat`
writes it before every build: called by `build.bat`, by the `.build-pre` hook in
`adc_dma_40msps.X/Makefile` (which the IDE runs, so the colleague's build gets it too)
and, for `tools/Makefile`, by `tools/version.sh`. `board.h` includes it through
`__has_include` and falls back to "unknown". Two pitfalls, both hit on 23.09.2026 with
MPLAB X 6.35: do **not** put the step into `configurations.xml`
(`makeCustomizationPreStep`) - the headless makefile generator then silently writes no
`Makefile-*.mk` at all; and in the hook use exactly
`cmd /c "$(subst /,\,$(CURDIR))\..\tools\version.bat"` - the IDE's make reports
`SHELL=sh.exe` without having one, and neither a quoted relative path nor
`cd ../tools &&` reached cmd intact (`'..' is not recognized`).

MPLAB X project: configurations `EV74H48A_Curiosity_Platform_MPS512` (the Curiosity
Platform board, PKOB4, `dma.c`), `EV17P63A_Curiosity_Nano_MPS506` (the Curiosity Nano:
device dsPIC33AK512MPS506, `nEdbgTool`, `BOARD=2`) and `sim` (Simulator, `sim_dma.c`,
`__MPLAB_DEBUGGER_SIMULATOR=1`); hardware configurations are named after the evaluation
kit, order number first, so that the name in the IDE says what gets programmed. Command
line: `tools\build.bat`, `tools\build.bat nano`, `tools\build.bat sim`. `tools\_test_mplabx.bat` builds
the Curiosity Platform configuration
from the command line through MPLAB X's own makefile generator. Two pitfalls: the
generator rewrites `languageToolchainVersion` in `nbproject/configurations.xml` to
whatever compiler it finds first - restore that one line, never `git checkout` the
whole file (that once threw away a file-list change); and after any change to the
file list delete `adc_dma_40msps.X/build` and `dist`, otherwise make links stale
objects and a missing entry goes unnoticed.

GUI tool: `tools\gui_setup.bat` once, then `toolsdc_gui.bat --fake`; `python toolsdc_gui.py --selftest` is its check.

Simulator test, the acceptance check for anything that touches the buffer logic:

```
tools\build.bat sim
python tools\sim_trap.py --run-seconds 420 expect "[simtest] PASS"  (about 7 minutes)
tools\build.bat sim 256 && python tools\sim_trap.py --elf build\adc_dma_40msps_sim256.elf --run-seconds 420
                                           the same at 256 samples per half (run-time buffer length)
python tools\sim_trap.py --fault 65536     expect "[simtest] FAIL" with one mismatch at index 0
```

The simulator has no PLL, no ADC, no DMA and dispatches no interrupts (any pending
interrupt aborts with E0110). It proves the ping-pong buffer logic and nothing else.
It also runs `__delay32()` at a small fraction of real time: the 100 ms time-base check
(20 M cycles) took longer than the whole test budget and looked like a hang after the
self-test (24.09.2026, in tools/sim_trap.py and in the MPLAB X simulator alike), so the
simulator build delays 1 % of that. Keep every other long wait out of the simulator path
the same way.

## Rules

- Say what has and has not run on silicon. `docs/HARDWARE-LOG.md` is the record; add
  a dated entry for every board run and for every change made in reaction to one.
- Every register value gets the datasheet table or page it comes from, in the comment
  next to it. A value copied from MCC or from Microchip's example says so.
- Status flags are cleared by writing the word once with 0 only in the bits to clear
  (`dma0_clear()`), never with bit-field read-modify-write; interrupt flags are
  cleared at the start of the handler, not the end. Both were real bugs on the board.
- Two configuration-bit names are pack-version dependent (`FICD_NOBTSWP`,
  `FWDT_RCLKSEL`) and are written as numbers. Keep it that way.
- Console output for humans goes through `console_puts()`/`console_kv()`; command
  replies through the parser's sink. A line longer than a buffer is a stack overrun:
  size buffers from the longest possible line and say so in the comment.
- Commit messages: what changed, why, what was verified. No attribution trailers.
- Do not edit `cmd_parser.c/.h` - with one deliberate exception: `CMD_PARSER_MAX_COMMANDS`
  is 24 instead of upstream's 16 (24.09.2026, the 17th..19th commands `core`, `dac`,
  `dactest`; 32 bytes of RAM). When updating the parser from github.com/zabooh/cmd_parser,
  re-apply that one line.

## Planned, not built

`docs/PLAN-BINARY-TRANSFER.md`: a `blk <n>` command that sends a contiguous block of up
to 2048 samples as binary with a text header and a CRC-16 line, the client side in
`tools/adc_gui.py`, an optional `baud` command - ordered so that everything but the
baud limit is proven in the simulator and with the fake target before a board run.

## What this firmware does, and what it deliberately no longer does

**Back-to-back only.** On 24.09.2026, after run 7, everything that tries to pace the
conversions was removed from the code: the ADC repeat timer (`TRG2SRC = 3`, `RPTCNT`),
the SCCP1 trigger as second trigger (34) and as first trigger in Single Conversion mode
(65), `sccp.c/.h`, the pacing selection and the `pacing`/`period` commands. All of them
were configured correctly, read back correctly and ignored by the hardware - the two
timer sources delivered the unpaced rate, the two SCCP1 sources delivered no conversion
at all (`docs/HARDWARE-LOG.md`, runs 4 to 7). `SAMC` does not change the rate either.
Do not reintroduce them without a board run that shows one of them working.

**The rate comes from PLL1, not from the CLKGEN6 divider.** `capture_set_pll(p1, p2)`
sets PLL1's two output dividers; the ADC clock is 1600 MHz / (p1*p2), p1 >= p2, both
1..7, which is 40 down to 4.08 MSPS with 5/5 = 8 MSPS exactly. PLL1 feeds nothing but
the ADC path, and its output-divider switch is what `clock_init()` does at every boot,
so it is known to work on this silicon.

The CLKGEN6 divider (`capture_set_clkdiv()`, the `clk` command) is kept for the record
only. Through runs 8 and 9 every ratio was written, read back and confirmed by `DIVSWEN`
and `CLKRDY` - with the generator switched off around the write and with it left running
as Example 12-2 prescribes - and the ADC converted at 40 MSPS at every single one of
them. **Do not build a rate on it.** `test clkoff` asks the remaining question: switch
CLKGEN6 off entirely and see whether the ADC still converts.

Both switches run in the boot order and report the step that failed rather than a bool:
DMA channel down, ADC core off, clock changed and read back, core on, DMA from scratch.

**Nothing runs by itself.** The firmware boots, brings the console up, sets the slowest
rate and waits. Everything else is typed: `test` lists the parts, `test all` runs them
in the order self, clock, sweep, dac. `test self` failing stops `test all`; nothing else
does. The reason for the idle boot is that through seven runs the console never received
a byte (`rx = 0`) and it could not be told whether the bytes never arrived or whether the
receive interrupt (priority 1) was starved behind the DMA interrupt (priority 4, 1.6
million per second at full rate). With nothing converting, that question answers itself.

**The sweep runs from the slowest rate up**, not from the fastest down. The slowest point
is the one the DMA should manage, so the first row is the row most likely to pass - and a
failure there means the chain is broken, not the rate. The ladder mixes integer and
fractional ratios on purpose (2.5 between 2 and 3, 4.5 between 4 and 5): a row that lands
halfway between its neighbours proves `FRACDIV` works, one that snaps to a neighbour
proves it is ignored. 500 = 8 MSPS is in the ladder because that is the rate the
customer's application needs.

## What the board has settled (24.09.2026, runs 8 to 13)

- **The chain works.** Run 13 captured one buffer with the on-chip DAC's triangle on the
  input and the triangle is in it: clean rise, one turning point, clean fall, largest step
  113 counts out of a swing of 1440, no jump, no gap. Conversions are real, complete and
  in order, across the half boundary and the burst restart.
- **The rate follows the PLL.** One clean burst delivered 3990 kSPS against 4081 nominal.
- **Every rate measured under load in runs 4 to 11 is wrong** by about a factor of ten -
  it was taken by a CPU drowning in the overrun interrupt. The sweep measures a clean
  single burst now and prints both columns.
- **The console works.** `rx = 0` in runs 6 and 7 was the receive interrupt starving
  behind the DMA interrupt, not the wiring.
- **The CLKGEN6 divider does not reach the converter**, with either switching sequence,
  and the ADC keeps converting with the generator switched off (`test clkoff`).

## Open questions (as of 24.09.2026)

1. **Up to what rate does the chain stay lossless?** The sweep table from a board is the
   one thing still missing, and it is the customer's question: `postdiv 5/5` is 8 MSPS.
2. **Why does the ADC ignore CLKGEN6?** Table 16-1 names it as the clock source, its
   divider has no effect in either switching sequence, and the converter keeps running
   with the generator off. This is a question for the product line; the register evidence
   is complete in the log.
3. **`dac_period_ns()` is wrong.** The computed triangle period is about eight times off
   what a capture shows, and `DACLOW` is not reproduced - the upper end matches `DACDAT`
   exactly, the lower end does not. Nothing is judged against it any more, but it should
   be corrected.
5. At 40 MSPS about 4 % of the samples are lost as OVERRUN, and every overrun raises the
   DMA interrupt - 1.6 million per second, one every 625 ns, against an interrupt entry
   that costs about as much. That is why runs 7 and 8 stopped dead with the console gone:
   the CPU never left the handler. The event has no enable bit of its own (DS70005591D
   13.6.1; `DMA0CH` has HALFEN/DONEEN/MATCHEN only), so the handler brakes itself past
   `OVERRUN_LIMIT` and the rate is reported as unusable. The real remedy is to measure
   where there are no overruns, which is what the sweep looks for.
