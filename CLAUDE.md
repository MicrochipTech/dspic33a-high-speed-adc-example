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
| `board.h` | pins, ADC core/input, `AUTO_SWEEP` | – |
| `config_bits.c` | every configuration word, with reasons | – |
| `clock.c/.h` | PLLs, clock generators, clock-fail interrupt, `clock_cpu_on_pll()` | console, diag |
| `adc.c/.h` | the ADC core: init, burst trigger, PINSEL/SAMC register | console, diag |
| `dma.c/.h` | DMA channel 0: window = the buffer, HALF/DONE interrupt shell, status flags | console, diag; calls `dma0_event()` in capture.c |
| `sim_dma.c` | replaces `dma.c` in the simulator build; implements `dma.h` without a DMA | adc, capture, console |
| `capture.c/.h` | the measurement: the DMA buffer (private, with guard words), `dma0_event()`, counters, start/stop/input, self-test, per-half processing | adc, dma, led, console, diag |
| `led.c/.h` | LED0 | – |
| `timebase.c/.h` | Timer1 as a stopwatch (12.5 MHz) for measuring the delivered sample rate; not involved in producing it | – |
| `sccp.c/.h` | SCCP1 as a timer whose period rollover (AUXOUT = 01) is the ADC's "SCCP1 trigger", code 34: TRG1SRC for pacing 65, TRG2SRC for 34 | – |
| `diag.c/.h` | `fail()` codes, trap handler, boot record in persistent RAM, `RCON` report, `regs_dump()` | every module's `*_regs_dump()` |
| `cli.c`, `console.h` | UART2, the commands, the `sweep` | clock, capture, led, diag |
| `sim.h` | the hooks the simulator build needs; all empty on silicon | – |
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

MPLAB X project: configurations `EV74H48A_Curiosity_Platform_MPS512` (the board,
PKOB4, `dma.c`) and `sim` (Simulator, `sim_dma.c`, `__MPLAB_DEBUGGER_SIMULATOR=1`);
hardware configurations are named after the evaluation kit, order number first, so
that the name in the IDE says what gets programmed. `tools\_test_mplabx.bat` builds
the board configuration
from the command line through MPLAB X's own makefile generator. Two pitfalls: the
generator rewrites `languageToolchainVersion` in `nbproject/configurations.xml` to
whatever compiler it finds first - restore that one line, never `git checkout` the
whole file (that once threw away a file-list change); and after any change to the
file list delete `adc_dma_40msps.X/build` and `dist`, otherwise make links stale
objects and a missing entry goes unnoticed.

Simulator test, the acceptance check for anything that touches the buffer logic:

```
tools\build.bat sim
python tools\sim_trap.py --run-seconds 600 expect "[simtest] PASS"  (6-8 minutes; the boot alone - register snapshot, time base check, four pacing candidates - takes over 4)
python tools\sim_trap.py --fault 65536     expect "[simtest] FAIL" with one mismatch at index 0
```

The simulator has no PLL, no ADC, no DMA and dispatches no interrupts (any pending
interrupt aborts with E0110). It proves the ping-pong buffer logic and nothing else.

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
- Do not edit `cmd_parser.c/.h`.

## Open questions (as of 23.09.2026)

1. The ADC's repeat timer (`TRG2SRC = 3`, `RPTCNT`) does not pace a burst in
   Integration mode on the board: the register holds the written value and the DMA
   still receives the back-to-back rate (HARDWARE-LOG runs 5 and 6, registers
   decoded). The SCCP1-as-TRG2 result of those runs is void: code 32 is "PTG trigger
   12" (SCCP1 is 34, Tables 16-3/16-4) and `AUXOUT` was 00, so SCCP1 emitted no
   trigger at all; both fixed 23.09. evening. Since then the first candidate is
   Microchip's own mechanism, one conversion per SCCP1 trigger in Single Conversion
   mode (`ADC_PACE_SINGLE` = 65, `TRG1SRC = 34`, no burst, no restart), untested on
   the board. Since 23.09. evening the fourth candidate is the ADC
   clock divider (`ADC_PACE_CLKDIV` = 64, `clock_adc_set_div()`, CLKGEN6 `INTDIV`,
   ratios 1/2/4/6/8/10 = 40 … 4 MSPS), tried after the two trigger sources in the
   AUTO pacing; the boot sweep then steps the divider and **takes the highest rate
   whose `process` run had overrun 0 and missed 0** for the measurement. Whether the
   divider switch (`DIVSWEN`) works between bursts and the ADC stays calibrated at a
   lower clock is what the next log settles (`[ratetest]` for pacing 64, then the
   sweep rows). `pacing`/`period` change it at run time.
2. At 40 MSPS about 4 % of the samples are lost as OVERRUN, independent of what the
   CPU does, and every overrun raises the DMA interrupt (~1.6 million per second),
   which starves the main loop and probably the console (`rx=0` in run 6). The
   overrun interrupt has no enable bit (DS70005591D 13.6.1: any channel event flag
   raises the channel interrupt; `DMA0CH` has HALFEN/DONEEN/MATCHEN only), so it
   cannot be masked on its own - the remedy is to run the measurement at a rate
   without overruns, which is what the sweep now selects. Whether the DMA bus or
   something else limits the rate is the measurement the example exists for.
3. One build (23.09., 11:36) reset right after "measurement running"; the next build
   printed a full sweep. The boot line now reports `RCON`; watch for it.
