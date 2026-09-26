# Firmware Structure: Modules by Category

As of 26.09.2026, revision `28e88fa`. Pure code analysis: nothing built, nothing
run on silicon. Line counts rounded.

The firmware comprises about 10,500 lines of C in 17 modules, plus about 5,000 lines
of Python under `tools/`. This document assigns each module to a category, shows
where modules mix several categories, and proposes a target structure.

## Overview

| # | Category | Core modules |
|---|---|---|
| 1 | Drivers (registers) | clock, adc, dma, sccp, dac, timebase, led, config_bits, board.h |
| 2 | Application | capture (core), main, crc16 |
| 3 | Tests | chaintest, dactest, `test_*` in cli.c, measurement functions in capture.c |
| 4 | CLI / terminal | cli.c, console.h, cmd_parser |
| 5 | Data transport to the GUI | `blk` and `stream grab` in cli.c, `chain_stream_*` in chaintest.c, crc16, adc_gui.py |
| 6 | Diagnostics / error handling | diag.c, `*_regs_dump()` of every module |
| 7 | Measurement instruments | timebase, `clock_monitor_hz()`, counters in capture.c |
| 8 | Start-up / system integration | main.c, config_bits.c, `console_early_init()`, `clock_init()` |
| 9 | Board description / pin mapping | board.h, tools/boards.py, pins64.py, pins128.py |
| 10 | Simulator abstraction | sim.h, sim_dma.c |
| 11 | Build / versioning / tooling | tools/build.bat, version.*, Makefile, adc_dma_40msps.X |
| 12 | Host-side evaluation | eval_chain.py, spectrum in adc_gui.py |
| 13 | Signal source / stimulus | dac.c |
| 14 | Third-party code | cmd_parser.c/.h |
| 15 | Documentation as measurement record | docs/HARDWARE-LOG.md, docs/ANALYSIS.md |

Categories 1 to 5 are the firmware's main layers. Categories 6 to 10 are separate
responsibilities that today partly live inside other modules. Categories 11 to 15
sit at the edge of the firmware or outside it.

---

## 1. Drivers: modules that configure registers

Depends on the controller. This layer is clean: register access happens almost only
here. The exceptions are listed in the "Mixed modules" section.

| Module | Lines | Peripheral |
|---|---|---|
| `clock.c` | 675 | PLL1/PLL2, CLKGEN6/7/13, clock-fail interrupt, clock monitor 4 (by far the most register accesses) |
| `adc.c` | 320 | ADC core, `CH0CON1` (PINSEL, SAMC, MODE, trigger sources, IRQSEL), calibration |
| `dma.c` | 222 | DMA channel 0: address window, HALF/DONE, clearing status with `dma0_clear()` |
| `sccp.c` | 170 | SCCP1 as ADC trigger, timer/compare interrupts as counters |
| `dac.c` | 270 | DAC2 in triangle mode, CLKGEN7, UREF routing |
| `timebase.c` | 52 | Timer1 as a stopwatch (12.5 MHz) |
| `led.c` | 52 | LED0 |
| `config_bits.c` | 193 | Configuration words |
| `board.h` | 236 | Pins, ADC core/input, PLL dividers at boot, DAC route |

Rule from `CLAUDE.md`: nobody besides `dma.c` touches DMA registers, nobody besides
`adc.c` touches ADC registers, nobody besides `clock.c` reads `CLK1CON`. There is no
such rule for the UART, because no UART module exists (see cli.c below).

## 2. Application: logic independent of the controller

| Module | Content |
|---|---|
| `capture.c` (core) | Ping-pong logic `dma0_event()`, `capture_service()`, `process_buffer()`, `half_mean()`, counters, guard words |
| `main.c` | Main loop: `capture_service()`, status lines, stall detection. Writes no register itself |
| `crc16.c` | CRC-16/CCITT-FALSE, purely algorithmic, with a self-test at boot |

A real, controller-free application layer barely exists. The actual processing, a
sum over the buffer half, comes to about 30 lines. `capture.c` is mostly an
orchestration across adc, dma, sccp and clock.

## 3. Tests

| Location | Lines | Content |
|---|---|---|
| `chaintest.c` | 1,596 | `chain all` (stages S0..S9), triangle evaluation (`fit_line`, `tri_eval`), the `@` log format, `chain run` |
| `dactest.c` | 312 | capture N bursts, judge against the DAC setting. No register accesses |
| `cli.c`, `test_*` / `matrix_*` / `sweep_*` | ~700 | `test self/clock/clkoff/bursts/sweep/rate/matrix/dac` |
| `capture.c`, measurement part | ~250 | `capture_selftest`, `capture_clkoff_probe`, `capture_oneshot_n`, `capture_measure_rate`, variant table |
| `tools/sim_trap.py` | 220 | simulator run, ping-pong acceptance |
| `tools/eval_chain.py` | 458 | evaluation of the `chain all` log |
| `tools/gui_ui_test.py` | 417 | GUI test |

## 4. CLI / terminal

| Module | Content |
|---|---|
| `cli.c` | Commands (`cmd_*_fn`), `console_puts/kv/kv_hex`, number formatting, status line, registration with the parser |
| `console.h` | Console interface |
| `cmd_parser.c/.h` | Parser, prompt, ACK/NAK |

## 5. Data transport to the GUI

| Location | Content |
|---|---|
| `cli.c` `cmd_blk_fn` | `blk <n>`: `BIN` header line, binary block in chunks, CRC line |
| `cli.c` `cmd_stream_grab` | quiet half of the running stream as a binary frame |
| `cli.c` `console_write_raw` | sending binary data (including 0x00, which the parser cannot) |
| `crc16.c` | checksum of the frame |
| `chaintest.c` `chain_stream_on/off/grab_begin/grab_end/state` | provides the stream for the GUI |
| `tools/adc_gui.py` | the other side: `Target`, `parse_grab_frame`, `crc16_ccitt_false` |
| `docs/PLAN-BINARY-TRANSFER.md` | frame format |

There is no dedicated module for the transport. It runs as a special case over the
CLI channel: same UART, same prompt and ACK/NAK synchronization, with a binary
payload in between.

## 6. Diagnostics / error handling

`diag.c` (327 lines) contains the `fail()` codes, the trap handler
`_DefaultInterrupt`, the boot record in persistent RAM (`boot_mark`, `chain_mark`),
the RCON report and `regs_dump()`. `regs_dump()` calls each module's
`*_regs_dump()`.

This category is neither driver nor test. It cuts across every module and must
still work even when everything else has failed: `console_force_up()` rebuilds the
UART without assumptions. Register accesses (RCON, INTCON1..5, INTTREG) are
deliberate here and fine.

## 7. Measurement instruments

| Instrument | Location |
|---|---|
| Stopwatch | `timebase.c` (Timer1, sits on PLL2 and therefore cannot flatter the ADC rate on PLL1) |
| Frequency meter | `clock_monitor_hz()` in `clock.c` |
| Event counters | `dma_overrun`, `late_service`, `proc_missed` etc. in `capture.c`; SCCP and ADC interrupts as counters |
| Processing cost | `proc_ticks_*`, `capture_process_bench()` in `capture.c` |
| Rate measurement | `capture_measure_rate()` in `capture.c` |

Tests ask the questions, instruments answer them. `docs/ANALYSIS.md` separates the
two: several earlier results were withdrawn because the instrument was unfit, not
the test. Example: the rates from runs 4 to 11, measured by a CPU drowning in the
overrun interrupt.

## 8. Start-up / system integration

`main.c` (boot order with `boot_mark`), `config_bits.c`, `console_early_init()`
(UART on FRC before the PLL switch), `clock_init()`, `cli_init()`. Here the order
matters: `console_sync_baud()` and `console_flush()` exist only because a clock
change in the middle of an output shifts the baud rate.

## 9. Board description / pin mapping

Firmware: `board.h`. Host: `tools/boards.py`, `tools/pins64.py`, `tools/pins128.py`
(pin assignment, board SVG in the GUI). The same knowledge is stated twice, in C and
in Python, with no shared source.

## 10. Simulator abstraction

`sim.h` with the hooks `SIM_CHECK_HALF`, `SIM_DMA_TICK` and `SIM_CHECK_RUNNING`,
which are empty on silicon. `sim_dma.c` replaces `dma.c` behind the same `dma.h`
interface. Plus `#ifdef __MPLAB_DEBUGGER_SIMULATOR` in `main.c`. The contract: the
simulator proves the ping-pong logic and nothing else. It has neither PLL nor ADC
nor DMA and dispatches no interrupts.

## 11. Build / versioning / tooling

`tools/build.bat`, `tools/Makefile`, `version.bat/.sh` (generates `version.h`),
`_test_mplabx.bat`, `adc_dma_40msps.X/` (MPLAB X project with two configurations),
`setup.py`, `gui_setup.*`. The pitfalls for these are in `CLAUDE.md`.

## 12. Host-side evaluation

`tools/eval_chain.py` (triangle evaluation, same results as `tri_eval` in C) and the
spectrum analysis in `adc_gui.py` (`spectrum`, `analyze_spectrum`). It can be
separated from the GUI display and from the transport.

## 13. Signal source / stimulus

`dac.c` is technically a driver, but serves only as a known test signal (triangle
over UREF on `ANn7` or over RA8). Functionally it belongs to the test
infrastructure, not to the application.

## 14. Third-party code

`cmd_parser.c/.h` from github.com/zabooh/cmd_parser (Apache 2.0), unchanged except
for `CMD_PARSER_MAX_COMMANDS = 32`. It is its own category because of licensing and
the update path: the one change must be reapplied with every update.

## 15. Documentation as measurement record

`docs/HARDWARE-LOG.md` and `docs/ANALYSIS.md` do not describe the code; instead
they record, bindingly, what has run on silicon, including disproven predictions
and withdrawn results. This is more a process category than a code category.

---

## Mixed modules

### cli.c (1,988 lines): four categories in one file

| Category | Content |
|---|---|
| Driver | UART2 setup (`U2CON`, `U2BRG`, `U2STAT`), PPS (`RPCON`), interrupt priorities, ISR `_U2RXInterrupt` |
| CLI | all commands |
| Tests | the `test` suite, `sweep`, `matrix` |
| GUI transport | `blk`, `stream grab`, `console_write_raw` |

### chaintest.c (1,596 lines): test, algorithm and application

- **Test:** stages S0..S9 and the summary.
- **Hardware-free algorithm:** `fit_line` and `tri_eval`. Already being extracted
  and tested on the host, so it belongs in its own module.
- **Application / GUI transport:** `chain_stream_*`, i.e. the "stream on"
  operation. This means the example's target operation (SCCP1 → ADC → DMA →
  ping-pong → CPU) sits inside a test module.
- **Register accesses outside the drivers:** reads `IPC6/9/12/30` directly. Also
  implements the ADC callback `adc_ch0_event()`.

### capture.c (1,257 lines): application, rate control, measurement instruments

- Ping-pong application (category 2).
- Rate switching `capture_set_pll/rate/clkdiv`, largely a forwarding to `clock.c`.
- About 250 lines of measurement instruments and test functions (categories 3 and
  7).

### diag.c: cross-cutting

The trap handler and RCON are deliberately close to the registers. `regs_dump()`
pulls in every module. That is acceptable for diagnostics, but it creates most of
the dependencies in the project.

---

## Proposal for a target structure

| Layer | Modules (**bold** = new or extracted) |
|---|---|
| Drivers | clock, adc, dma, sccp, dac, timebase, led, **uart** (from cli.c) |
| Application | capture (ping-pong and service only), **stream** (`chain_stream_*` from chaintest.c), **process** |
| Algorithms (host-capable) | **tri_eval** (from chaintest.c), crc16 |
| Measurement instruments | **meter** (counters, cost and rate measurement from capture.c) |
| Tests | chaintest (stages only), dactest, **bench** (`test_*`, `matrix`, `sweep` from cli.c) |
| CLI | cli (commands and formatting only), cmd_parser |
| GUI transport | **gui_link** (`blk`, `grab`, frame construction, `console_write_raw`) |
| Diagnostics | diag |
| Simulator | sim.h, sim_dma.c |

Two steps bring the greatest benefit:

1. **Extract `stream` from chaintest.c.** This is the core of what the example is
   supposed to show, and it does not belong in a test module.
2. **Extract `uart` from cli.c.** After that every register peripheral has exactly
   one owner, and the rule from `CLAUDE.md` holds without exception.

Every restructuring changes the file list. Then `nbproject/configurations.xml`
must be updated and `adc_dma_40msps.X/build` as well as `dist` must be deleted
(see `CLAUDE.md`), and both variants (`tools\build.bat`, `tools\build.bat sim`)
must build without warnings.
