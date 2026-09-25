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
| `board.h` | two board profiles selected by `BOARD` (EV74H48A with the MPS512 DIM, default; EV17P63A Curiosity Nano with the MPS506): pins, ADC core/input, `ADC_PLL_POSTDIV1/2` (the boot rate, 7/7 = slowest), the DAC route constants | - |
| `config_bits.c` | every configuration word, with reasons | - |
| `clock.c/.h` | PLLs, clock generators, clock-fail interrupt, `clock_cpu_on_pll()`, `clock_adc_set_pll()`, `clock_adc_set_rate()`, the CLKGEN6 divider `clock_adc_set_div()` with its result codes, CLKGEN13 for the trigger (PLL1 out / 2 = 160 MHz), CLKGEN7 for the DAC (PLL1 VCO divider, 400 MHz), `clock_monitor_hz()` (clock monitor 4 as a frequency meter) | console, diag, chaintest |
| `adc.c/.h` | the ADC core: init, burst trigger, PINSEL/SAMC, the trigger-source registers, IRQSEL, calibration bits, core 5's CH0 interrupt as a counter (`adc_ch0_event()` in chaintest.c) - every register and vector core 5 uses is identical on the MPS506 (checked against the pack header 25.09.2026), so the chain test needs no board guard | console, diag, chaintest |
| `dma.c/.h` | DMA channel 0: window = the buffer, HALF/DONE interrupt shell, status flags, `dma0_remaining()` | console, diag; calls `dma0_event()` in capture.c |
| `sim_dma.c` | replaces `dma.c` in the simulator build; implements `dma.h` without a DMA | adc, capture, console |
| `capture.c/.h` | the measurement: the DMA buffer (private, with guard words), `dma0_event()`, the counters, start/stop/input, self-test, per-half processing and its cost, the variant table, the triggered stream `capture_chain_*()`, and `capture_chain_halt()`/`_resume()` - pausing and restarting an ALREADY RUNNING chain stream's trigger in place (DMA channel left armed, counters untouched), for the GUI's halt/grab/restart cycle | adc, dma, sccp, led, console, diag |
| `crc16.c/.h` | CRC-16 over a sample block, for the `blk` binary transfer | cli |
| `sccp.c/.h` | SCCP1 as a trigger source, with clock source, mode and event as parameters; its timer and compare interrupts as event counters - same registers and vectors on both boards, see `adc.c/.h` above | capture, chaintest |
| `led.c/.h` | LED0 | - |
| `timebase.c/.h` | Timer1 as a stopwatch (12.5 MHz) for measuring the delivered rate. It sits on the CPU branch (PLL2) while the ADC is on PLL1, so it cannot flatter the ADC. Not involved in producing the rate. | - |
| `dac.c/.h` | DAC1 and DAC2 in Triangle Wave mode (DACOUT1 = RA1, DACOUT2 = RA8), one unit table, shared CLKGEN7, plus the internal UREF route to any core; the known signal for `test dac`. `dac2_*()` are DAC2-only aliases for the chain test's fixed pin route and its low-latency ISR path (`dac2_set()`) | console, diag, chaintest |
| `dactest.c/.h` | judges captured halves against the DAC settings (min/max, reversals vs period, jumps); works with either DAC unit via `dac_active()` | capture, dac |
| `chaintest.c/.h` | the chain test `chain all` (S0..S9), the triangle evaluator (turning points by line fits, "slip"), the `@` log format, `chain run`, `chain_stream_on/off`, and `chain_stream_grab_begin()`/`_end()` - one halt/grab/restart cycle of the standing stream for `stream grab` (cli.c), the counters reported as the delta since the previous grab - builds and links unchanged for both boards (`tools\build.bat nano`, 25.09.2026); not yet run on Nano hardware, and the GUI cycle not yet run on either board | capture, adc, sccp, dac, clock, dma (register dumps), diag |
| `diag.c/.h` | `fail()` codes, trap handler, boot record in persistent RAM (including `chain_mark`, the chain test's stage), `RCON` report, `regs_dump()` | every module's `*_regs_dump()` |
| `cli.c`, `console.h` | UART2, the commands, the `sweep`, the `test` suite, the variant matrix, the chain/stream commands (`stream grab`'s `GRAB` frame builder among them), the binary block transfer (`snap`, `rate`, `blk`) | clock, capture, dactest, led, diag |
| `sim.h` | the hooks the simulator build needs; all empty on silicon | - |
| `tools/adc_gui.py` | NiceGUI front end for the triggered chain only (25.09.2026 on - back-to-back retired from this tool, owner's decision): one acquisition card drives `stream on <ksps> [core pinsel [samc]]` / `off` / `grab` in a loop, plots the time signal and FFT from each grab, and evaluates the test signal's triangle with `tools/eval_chain.py`'s `tri_eval`/`grid_ok` (imported, not re-implemented) when the frame's own `slp > 0`; `--fake` uses a built-in stand-in (the same triangle for the test signal, a configured sine with harmonics for any other input), `--selftest` runs the pipeline without GUI. On connect it reads the board from the `version` reply (`[build] board: EV...`) and switches profile and default input; `--fake --fake-board EV17P63A` makes the stand-in report the Curiosity Nano. `tools/gui_ui_test.py` drives the page itself with a headless browser (Playwright) against `--fake`, both board profiles, on free ports. `tools/gui_setup.bat` makes its venv (`tools/.venv`, ignored). `tools/boards.py`, `tools/pins64.py`, `tools/pins128.py` hold the board/pin tables the GUI's board tile reads | the console protocol only |
| `tools/eval_chain.py` | `chain all` log evaluator (`tri_eval`, `grid_ok`, `synth`) and CLI report; imported by `adc_gui.py` for the chain tile and by `FakeTarget.grab()` for its synthetic frames, so there is one triangle evaluator and one synthetic-triangle generator, not two | - |
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

MPLAB X project: configurations `EV74H48A_Curiosity_Platform_MPS512` (the Curiosity
Platform board, PKOB4, `dma.c`), `EV17P63A_Curiosity_Nano_MPS506` (the Curiosity Nano:
device dsPIC33AK512MPS506, `nEdbgTool`, `BOARD=2`) and `sim` (Simulator, `sim_dma.c`,
`__MPLAB_DEBUGGER_SIMULATOR=1`); hardware configurations are named after the evaluation
kit, order number first, so that the name in the IDE says what gets programmed. Command
line: `tools\build.bat`, `tools\build.bat nano`, `tools\build.bat sim`.
`tools\_test_mplabx.bat` builds the Curiosity Platform configuration from the command
line through MPLAB X's own makefile generator. Two pitfalls: the generator rewrites
`languageToolchainVersion` in `nbproject/configurations.xml` to whatever compiler it
finds first - restore that one line, never `git checkout` the whole file (that once
threw away a file-list change); and after any change to the file list delete
`adc_dma_40msps.X/build` and `dist`, otherwise make links stale objects and a missing
entry goes unnoticed.

GUI tool: `tools\gui_setup.bat` once, then `tools\adc_gui.bat --fake`; `python
tools\adc_gui.py --selftest` is its check.

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
  `CMD_PARSER_MAX_COMMANDS` is 32 instead of upstream's 16 (24 on 24.09.2026 for `core`,
  `dac`, `dactest`; 32 on 25.09.2026, when `chain` and `stream` had filled all 24 slots).
  4 bytes of RAM per slot. When updating the parser from github.com/zabooh/cmd_parser,
  re-apply that one line. `help` takes a slot too, and `cmd_register()` fails silently
  when the table is full: with `chain`/`stream` (the chain test) and `snap`/`rate`/`blk`
  (binary transfer, below) both merged in, 26 commands + help = 27 in use, 5 free.
  **`stream grab` (the GUI's halt/transfer/restart cycle, below) is a sub-command of
  `stream`, dispatched inside `cmd_stream_fn()`, exactly like `stream on`/`off` - it did
  not need a 27th slot, and was chosen that way on purpose to keep the 5 free.**

## Binary block transfer (`snap`, `rate`, `blk`) - firmware only, since 25.09.2026

`docs/PLAN-BINARY-TRANSFER.md`: a `blk <n>` command sends a contiguous block of up to
2048 samples as binary with a text header and a CRC-16 line (`crc16.c/.h`); `snap` fills
the buffer once and stops so the block being read is not being overwritten; `rate <ksps>`
sets the sample rate directly. These are the back-to-back commands; the firmware keeps
them for a terminal, but `tools/adc_gui.py` no longer sends any of them (owner's
decision, 25.09.2026: the triggered chain, `stream grab`'s `GRAB` frame below, is the
GUI's only data path now). An optional `baud` command to raise the UART rate is designed
(`docs/PLAN-BINARY-TRANSFER.md` step 6) but not yet implemented.

## The GUI's chain stream cycle (`stream grab`, since 25.09.2026)

The goal: from the GUI, configure and start the chain stream, and then, continuously and
automatically, halt it, transfer a contiguous window, restart it, visualise and analyse -
until the user stops it. Since 25.09.2026 this is the GUI's ONLY capture path: the
back-to-back capture tile, the PLL rate selector and the sweep tile were removed from
`tools/adc_gui.py` (owner's decision - back-to-back is obsolete), and what used to be a
second "chain stream" tile is now the one and only acquisition card, with one chart and
one set of evaluation chips. `chain_stream_on_input()` (chaintest.c, cli.c's
`stream on <ksps> <core> <pinsel> [<samc>]`) lets the GUI point the same cycle at any ADC
input, not only the DAC2 test triangle on RA8 - the DAC is then left alone (`slp=0` in
the frame) and the GUI plays a sine with harmonics through `--fake` instead, so
SNR/THD/harmonics still show something with a custom input. Design decisions, all
documented where the code that implements them lives, restated here because they were
not obvious:

- **The halt does not tear the chain down.** `capture_chain_halt()`/`_resume()`
  (`capture.c`) stop and restart the SCCP1 trigger only - the DMA channel stays armed,
  the ADC core and clock tree untouched. What is sent is the half that completed last
  (`capture_completed_half()`), the SAME mechanism `capture_service()` (the main loop)
  already reads from continuously - not a new "grab a few blocks and hope" scheme like
  `grab_window()` in the chain test's own S5 stage. Reusing it means the grab cycle
  inherits everything already proven about that half being complete, contiguous and not
  overwritten before it is read.
- **The frame is `blk`'s framing, with a wider header**, so the GUI's existing binary
  reader (`Target._read_line`/`_read_exact`, `crc16.c`) needed no changes, only a second
  regex: `GRAB n=<count> from=<from> ksps=<ksps> ov=<overrun> late=<late>
  missed=<missed> halves=<halves> xfer=<transfers> slp=<slpdat> dachz=<dac_hz>`, then the
  payload and a CRC line exactly like `blk` (`cmd_stream_grab()`, cli.c;
  `parse_grab_frame()`, `tools/adc_gui.py`).
- **The counters are per cycle, not the running total.** `chain_stream_grab_begin()`
  keeps its own baseline of `dma_overrun`/`late_service`/`proc_missed`/`blocks_done`/
  `capture_transfers()`, zeroed when `chain_stream_on()` starts the stream (it already
  calls `counters_clear()`), and reports the delta since the previous grab. A colleague
  watching the GUI wants to know what happened in the window just shown, not a number
  that only grows.
- **The restart always runs**, whether the transfer went out whole or was cut short
  (Ctrl+C, a disconnect): `cmd_stream_grab()` calls `chain_stream_grab_end()`
  unconditionally after the payload loop. If the restart itself fails,
  `chain_stream_off()` is called and the stream is left off cleanly - `stream` then
  reports it, and the GUI is expected to send `stream on` again. The chain is never left
  half-configured.
- **The DAC triangle's range is not in the frame** - only `slp` (SLPDAT) and `dachz` are.
  `triangle_for()` (chaintest.c) always picks the widest range the fixed limits in
  `dac.h` and `slp` allow, so the GUI reconstructs the same range from `slp` alone
  (`chain_triangle_range()`, `adc_gui.py`) rather than transmitting two more numbers.
- **One evaluator, one place.** The GUI's chain tile imports `tri_eval`/`grid_ok` from
  `tools/eval_chain.py` - the same module `chain all` logs are judged with - instead of a
  second implementation that could quietly disagree with the firmware's own. The fake
  target's synthetic triangle uses the same module's `synth()`.
- **Tested without a board**, per the design brief: `python tools/adc_gui.py --selftest`
  exercises `FakeTarget.grab()`/`parse_grab_frame()` for a refusal before `stream on`, a
  clean test-triangle grab that passes the grid check with the frame's own actual rate
  used as the FFT's fs, the next grab landing in the OTHER buffer half (`from > 0`), a
  lost-sample grab that correctly fails, a custom-input grab (`stream on <ksps> <core>
  <pinsel> <samc>`) with `slp=0` and a real FFT peak from the fake sine, a CRC mismatch, a
  truncated frame, and `Target.grab()` timing out against a serial stub that never
  answers - the same bounded-wait code path a real disconnected board would hit.
  `tools/gui_ui_test.py` (Playwright, headless Chrome) additionally drives the page
  itself against `--fake`: connect, LIVE with the test input (fs follows the rate, a PASS
  verdict), a rate change while LIVE, LIVE with a custom input (a spectrum with a
  fundamental, the triangle card hidden), STOP, SINGLE, no server-side exception. Not
  tested without a board: whether `capture_chain_halt()` really lands cleanly on real
  silicon timing, and whether the halt/grab/restart cycle holds up at the higher rates
  (`docs/HARDWARE-LOG.md`, 25.09.2026 entry).

## How the example is built, and what that costs

**Back-to-back is the working path.** The ADC runs in Integration mode (`MODE = 2`) with
`CNT` conversions per burst, `TRG1SRC = 1` (software start) and `TRG2SRC = 2`
(back-to-back), `IRQSEL = 0` so that every conversion raises the event the DMA triggers
on. The DMA is channel 0, Repeated One-Shot (`TRMODE = 1`, one transfer per trigger),
its address window exactly the buffer, with HALF and DONE interrupts and guard words
behind the buffer. **Never `TRMODE = 3`**: Repeated Continuous copies a whole block per
trigger at DMA speed - it was set until 25.09.2026 and was the cause of every "40 MSPS
whatever the setting", every overrun storm and the "few transfers per trigger"
(run 18, `dma.c`).

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

**The chain test (`chain all`, since 25.09.2026) is the run the colleague makes.**
It tests the target architecture of ANALYSIS.md C.8 - SCCP1 -> ADC core 5 in Single
mode -> DMA0 -> ping-pong -> CPU, DAC2 on RA8 as the signal - in stages S0..S9 within a
minute, and `tools/eval_chain.py` evaluates the log it sends back. Plan, stages and
the decisions behind them: `docs/CHAIN-TEST-PLAN.md`. Its triangle evaluator was
tested on the host before any board run (gcc on the code extracted from chaintest.c,
synthetic windows with DNL, noise and a modelled DAC filter): no false alarm in 2100
clean windows, a single lost or repeated sample found in 96-100 % of windows; the
Python port in eval_chain.py gives identical results on the same data. The `test ...`
suite below is the older back-to-back instrument and stays as it is.

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

## What the board has settled (as of run 19, 25.09.2026)

- **The chain streams up to 8 MSPS** (run 19): SCCP1 -> ADC core 5 -> DMA0 -> ping-pong ->
  CPU, 15 s at 8 MSPS, 120 M samples, overrun, late and missed 0, the CPU processing every
  half. The DAC triangle comes through without a lost or repeated sample up to 10 MSPS,
  and its slope matches the model to 0.1 % (open question 4 closed).
- **Limits:** a few hundred DMA overruns per 0.5 M transfers from 10 MSPS; lost triggers
  from 16 MSPS; above that the ADC converts at only 18 to 20 MSPS in triggered single
  mode (every second trigger lost at 40 MSPS).
- **The CLKGEN6 divider divides** (clock monitor: 320/160/80 MHz), and `CLK6CON.ON = 0`
  does not stop the generator. Back-to-back streaming follows the PLL setting (8 MSPS: one
  burst and the 100th give the same slope). Open questions 1 and 3 are closed.
- **The processing cost was the limit at 8 MSPS:** the first loop took ~23 cycles per
  sample, 92 % of the half period. Rewritten 25.09.2026 (32-bit reads, unrolled); run 20
  will say by how much.

- **The DMA was misconfigured from the first day.** `TRMODE = 3` (Repeated Continuous)
  makes one trigger start back-to-back transfers until the block is full (DS70005591D
  13.4.8.4/13.4.8.5, p833 f.). Run 18: 6144 transfers for 15 conversions at 100 kHz,
  about 41 M transfers/s at every rate. Fixed to `TRMODE = 1` (Repeated One-Shot). Every
  rate and overrun figure of runs 1 to 18 was taken with the wrong mode.
- **SCCP1 -> ADC works** (run 18, S2): one result per SCCP1 period at 1, 10 and 100 kHz,
  timer mode, CLKGEN13 measured at 160 MHz. The clock monitor reads CLKGEN6 at 320 MHz and
  CLKGEN7 at 400 MHz. Output-compare mode produced no events; its CM codes for the PLL
  outputs (0xB/0xC/0xE) do not select what the ATDF says - 0xC read 160 MHz, i.e.
  CLKGEN13.

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
- ~~The CLKGEN6 divider does not reach the converter~~ - **withdrawn** (ANALYSIS.md C.3):
  both observations behind it were made with instruments later found void. Every
  document names CLKGEN6 as the ADC clock. The chain test's S8 measures it at the
  generator itself with the clock monitor.
- **Two clocks ran out of specification until 25.09.2026** (Table 40-24, p2016): the DAC
  at 320 MHz (minimum 400) and SCCP1 at 320 MHz (maximum 200). Now DAC on the PLL1 VCO
  divider at 400 MHz, SCCP1 on CLKGEN13 = PLL1 out / 2 = 160 MHz. And the DAC's data
  registers need an update trigger (`UPDTRG`, p1409) that was never set; it is now 11
  (every write taken at once).
- **Every rate measured under load in runs 4 to 11 is wrong** by about a factor of ten -
  it was taken by a CPU drowning in the overrun interrupt. Do not quote those numbers.
- **`dma_overrun` is a lower bound, not a count of lost samples.** `OVERRUN` is one bit
  in `DMA0STAT` and the handler increments once per entry in which it finds it set;
  several losses between two entries count as one.

## Open questions (as of 25.09.2026, after run 19)

Questions 1, 3 and 4 below are answered by run 19 (see "settled" above and the
HARDWARE-LOG); question 2 is answered up to 8 MSPS. Still open: the DMA overruns from
10 MSPS (bus contention with the CPU?), the ADC's triggered ceiling of ~18-20 MSPS, the
processing budget, and two instrument faults (output-compare mode of SCCP1, the clock
monitor's CNTSEL codes for PLL outputs). The list as it stood before:

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
4. **`dac_period_ns()` is still wrong even after the two-slope fix.** 25.09.2026 fixed
   one factor of 2 (a period is two slopes, not one - `dac.c`), but run 13 measured 513
   us against a formula that claimed 54.9 us, a gap the factor of 2 does not close. And
   `DACLOW` is not reproduced - the upper end matches `DACDAT` exactly, the lower end
   does not. Nothing is judged against it any more (`dactest.c` compares against a
   *measured* period instead), but it should be corrected.
