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

MPLAB X project: configurations `default` (PKOB4, `dma.c`) and `sim` (Simulator,
`sim_dma.c`, `__MPLAB_DEBUGGER_SIMULATOR=1`). `tools\_test_mplabx.bat` builds `default`
from the command line through MPLAB X's own makefile generator. Two pitfalls: the
generator rewrites `languageToolchainVersion` in `nbproject/configurations.xml` to
whatever compiler it finds first - restore that one line, never `git checkout` the
whole file (that once threw away a file-list change); and after any change to the
file list delete `adc_dma_40msps.X/build` and `dist`, otherwise make links stale
objects and a missing entry goes unnoticed.

Simulator test, the acceptance check for anything that touches the buffer logic:

```
tools\build.bat sim
python tools\sim_trap.py                   expect "[simtest] PASS"  (about 4 minutes)
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

1. The rate is now set by the ADC's repeat timer (`TRG2SRC = 3`, period `RPTCNT` in
   TAD, DS70005591D Table 16-4 p1227 and 16.4.5 p1322) instead of the back-to-back
   trigger, after the sweep showed the delivered rate did not follow `SAMC`. Whether
   the hardware counts `RPTCNT` or `RPTCNT + 1` TAD per period, and whether 40 MSPS
   (`RPTCNT = 2`) is reached at all, is what the measured column of the next sweep
   settles. Rates below 1.27 MSPS need a divided ADC clock (`CLK6DIV`) or a CCP timer
   as trigger (`TRG2SRC = 32` = CCP1, Example 16-8 p1334).
2. At 40 MSPS about 4 % of the samples are lost as OVERRUN, independent of what the
   CPU does, and every overrun raises the DMA interrupt (~1.6 million per second),
   which starves the main loop and the console. Whether the DMA bus or something
   else limits the rate is the measurement the example exists for.
3. One build (23.09., 11:36) reset right after "measurement running"; the next build
   printed a full sweep. The boot line now reports `RCON`; watch for it.
