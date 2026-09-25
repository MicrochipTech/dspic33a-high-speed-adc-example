# Implementation plan: the chain test

Status: implemented 25.09.2026 (`chaintest.c`, `tools/eval_chain.py`), not yet run on
the board. Basis: `docs/ANALYSIS.md` sections C.8 to C.12. Section 9 lists where the
implementation departs from this plan and why.

## 1. What is being built

One console command, `chain all`, on master. The colleague who has the board pulls,
builds, programs, types it once and sends back the UART log. The log is evaluated here.

It checks, link by link and then as a whole, the chain the example is meant to show:

```
SCCP1 (timer) ──► ADC core 5, Single Conversion ──► DMA0, Repeated One-Shot ──► ping-pong buffer ──► CPU
   sample clock      one conversion per trigger        one transfer per conversion      HALF / DONE        processing
```

The chain runs continuously, the rate is set by `CCP1PR`, and the test signal is
always DAC2 (DACOUT2 = RA8 = AD5AN3). The last step is not a test but the attempt at
the real thing: `chain run <ksps> <seconds>` runs the chain as the example would, with
the main loop processing every half, and says whether it held.

The existing back-to-back code and `test ...` stay untouched. The chain gets its own
code path next to them, so that nothing that has been proven on silicon changes under
the new test.

## 2. Principles for the run

- **One pass answers everything.** `chain all` walks every stage in order. A stage that
  fails reports FAIL and the run continues with the next one. Only a stage that the
  following ones depend on (the clock tree, the trigger) turns the dependent stages into
  SKIP with a reason.
- **Safe to risky.** Low rates first, where the CPU can count every event; high rates,
  long streams and the old open questions last. A hang late in the run leaves the early
  results in the log.
- **Where it died.** Before each stage the stage id goes into the persistent boot record
  (`diag.c`). After a trap, a reset or a hang with a manual reset, the next boot prints
  it. `chain from <stage>` resumes there.
- **Machine-readable log.** Every result is one line
  `@S<stage>.<n> key=value ... -> PASS|FAIL|SKIP|INFO`. `@END` closes the run with the
  summary. A human can read it; `tools/eval_chain.py` parses it and recomputes every
  verdict offline, so that a wrong threshold in the firmware does not cost another
  board run.
- **Raw data only on FAIL** (decided 25.09.2026). A window whose verdict is FAIL goes
  out as a `@DUMP` block in hex. For a PASS window the log carries the numbers the
  verdict rests on - turning-point positions, samples per slope, smallest and largest
  step, min/max - so that a threshold can still be re-judged offline even though the
  samples themselves are not there.
- **Budget: at most 1 minute** for the whole of `chain all` (decided 25.09.2026), one
  delivery, one board run. The console stays silent while a stream runs.
- **Every register value cites its source**, as the rest of the code does. Where the ATDF
  and the datasheet disagree, the ATDF wins.

## 3. Target clock tree

All from one VCO, so that the ADC, the trigger and the DAC are coherent (C.8, C.11,
C.12.1):

| Consumer | Generator | Source | Frequency | Limit (Table 40-24) |
|---|---|---|---|---|
| ADC | CLKGEN6 | PLL1 out (NOSC 5), no divider | 320 MHz | 32-320 MHz |
| SCCP1 | CLKGEN13 | PLL1 out, `CLK13DIV.INTDIV = 1` (÷2) | 160 MHz | ≤ 200 MHz |
| DAC2 | CLKGEN7 | PLL1 VCO divider (NOSC 7), `VCO1DIV.INTDIV = 2` | 400 MHz | 400-500 MHz |
| CPU, Timer1 | CLKGEN1 | PLL2 | 200 MHz | unchanged |

PLL1 sits at 5/1 (VCO 1600 MHz, out 320 MHz) for the whole chain test. The boot keeps
7/7 for the back-to-back path; `chain` sets 5/1 at its start.

Two assumptions are checked by the test, not trusted:
- `VCOxDIV` has only `INTDIV[30:16]` (ATDF, register group VCO, offset 0xC) and divides
  like a CLKGEN: F = VCO / (2 · INTDIV). Today's `0x10000` would then give 800 MHz, which
  matches the ATDF's `MAX_PLLO_FREQ_HZ` = 800 MHz. INTDIV = 2 → 400 MHz.
- The CLKGEN13 divider really divides (the same question C.3 asks about CLKGEN6).

**New instrument: the clock monitors.** The ATDF lists four (`CM1..4`, base 0x3200).
Their count source `CM_SEL.CNTSEL` can be PLL1 out (0xB), the PLL1 VCO divider (0xC),
CLKGEN6 (0x5) and CLKGEN7 (0x6). That measures the ADC's and the DAC's clock directly -
the first valid instrument for C.3. CLKGEN13 is not selectable; it is measured through
the SCCP1 counter against Timer1. The CM register layout (window, reference, count
register) has to be read from DS70005591D chapter 12 before coding.

## 4. Changes per module

The ownership rules of `CLAUDE.md` hold: DMA registers only in `dma.c`, ADC registers
only in `adc.c`, clock registers only in `clock.c`.

| File | Change |
|---|---|
| `clock.c/.h` | `clock_chain_init()`: the tree of section 3, every step checked and reported. `clock_dac_source()` (PLL1 VCO divider or FRC, for the cross-check). `clock_monitor_hz(sel, window)` over a CM. Fix `clock_dac_hz()` and `clock_trig_hz()` to follow the source and divider actually selected. |
| `sccp.c/.h` | CCP1 interrupt (IRQ 52) as an event counter for the low rates, handler in `sccp.c`, counter exported. `sccp1_tmr()` for the clock measurement. Timer and OC mode stay parameters. |
| `adc.c/.h` | Single mode on core 5 as it exists. New: `adc_set_irqsel()`, the CH0 interrupt of core 5 (IRQ 241) as a counter with its own handler, `adc_calib_state()` (ACALEN, CALREQ, CALRATE read back), `adc_other_channels()` (TRG1SRC of every channel ≠ 0 on the core, which must be 0). |
| `dma.c/.h` | DMA1 as a hardware event counter: trigger `CHSEL` = SCCP1 (0x18), a fixed source and a fixed destination word, count 65535, Repeated Continuous. Its DONE interrupt counts the wraps: total = wraps · 65535 + (65535 − `DMA1CNT`). `dma0_init()` already takes the source; the IRQSEL = 1 variant passes `ADnCH0DATA`. |
| `sim_dma.c` | Stubs for everything new in `dma.h`, so that the simulator build compiles. The chain test prints SKIP in the simulator. |
| `capture.c/.h` | A second run mode, **triggered stream**: `capture_stream_start()` arms DMA0, turns the ADC on and starts SCCP1 **last**. It never writes `SWTRG`, and DONE restarts nothing. `capture_stream_stop()` stops SCCP1 **first**. A one-shot variant stops SCCP1 from the DMA ISR after N blocks, leaving one contiguous window. Processing per half is timed with Timer1 (maximum and mean ticks). The back-to-back path stays as it is. |
| `dac.c/.h` | Fix `dac2_period_ns()` (two slopes, C.12.3). Clamp the limits to `0xCD + SLPDAT` … `0xF32 − SLPDAT`. `dac2_level(code)` for DC levels and CPU-stepped codes. `dac2_slope_len()` gives the exact number of samples per slope as a fraction, from the DAC clock, SLPDAT and the trigger period. |
| **`chaintest.c/.h`** (new) | The stages of section 5, the log format, the dumps and the summary. Calls capture, dac, clock and the counters. |
| `cli.c` | Command `chain`: `all`, `from <stage>`, `<stage>`, `run <ksps> [s]`, `rate <ksps>`. The parser has 21 of 24 slots in use; this takes one. |
| `board.h` | `CHAIN_ADC_CORE 5`, `CHAIN_PINSEL 3`, the rate ladder. The boot comments that still say "CLKGEN6 does not work" are corrected to C.3. |
| `diag.c` | Stage id in the boot record. Read DEVID/REVID (0x7C2000/0x7C2004) and print the silicon revision (errata #21 applies to A1 only). |
| project, `build.bat`, `tools/Makefile` | `chaintest.c` in all file lists; delete `build/` and `dist/` in the .X afterwards (`CLAUDE.md`). |
| **`tools/eval_chain.py`** (new) | Parses `@` lines and dumps, recomputes every verdict, turning-point fits on the dumps, optional PNG per dump. |

## 5. The stages of `chain all`

Numbers in brackets are the analysis points they answer (C.9 table 1-17, C.10 1-10).

A property of the DAC shapes the stages. The slowest triangle (SLPDAT = 1, full
swing) has a period of about 0.6 ms at 400 MHz. Below about 100 kSPS the triangle is
aliased. The low-rate stages therefore do not use the triangle. The ADC handler writes
the next code into `DACDAT` itself (**CPU-stepped DAC**), so every sample has a known
expected value. From 100 kSPS up the hardware triangle is used, with SLPDAT chosen per
rate so that one slope spans about 300 samples.

**S0 - Preconditions** (nothing converts)
- Build id, DEVID/REVID, Timer1 calibration (1 250 000 ± 0.01 % per 100 ms).
- `clock_chain_init()`. The clock monitors measure PLL1 out (320), the VCO divider
  (400), CLKGEN6 (320) and CLKGEN7 (400) against Timer1. Tolerance 0.5 %. [8, C.12.1]
- Core 5: `ACALEN = 0`, `CALREQ = 0`, no other channel with a trigger source.
  RA8: `ANSELA.8 = 1`, `TRISA.8 = 1`. [C.10.1, C.10.5]
- Interrupt priorities printed (C.12.8).
- FAIL in the clock tree → every later stage SKIP.

**S1 - SCCP1 alone** (no ADC)
- CLKGEN13: `CCP1TMR` across a Timer1 window → 160 MHz ± 0.5 %. [1, 8]
- Period: CCP1 interrupt counted over 100 ms at 1, 10 and 100 kHz → 100, 1000 and
  10 000 ± 1. In timer and in OC mode. [1]

**S2 - SCCP1 → ADC** (low rate, the CPU counts both ends)
- Single mode, `TRG1SRC = 0x20`, CH0 interrupt on, DMA off. At 1, 10 and 100 kHz:
  SCCP1 events = ADC results, exactly. [2]
- Timer mode against OC mode, `AUXOUT = 2`. Twice as many results as events means
  two events per period. [C.10.3]
- CPU-stepped DAC: `RES[k]` must follow `code[k−1]` within the static accuracy, and a
  frozen result register fails. [3]
- Static transfer DAC → RA8 → ADC at 16 levels, and the same at a short and a long
  SAMC. That characterises the touch-pad load on RA8 (C.11) before any step fault at
  a high rate is judged. The same over UREF (`ANn7`) as a comparison.

**S3 - ADC → DMA → buffer** (low rate, 10 kHz, CPU-stepped DAC)
- DMA0 on "ADC5 Done CH0" (0x48), triggered stream over three buffer cycles. [4]
- The buffer holds exactly the code sequence the CPU stepped. Nothing is missing,
  doubled or out of order. [5, 6]
- After DONE the next sample is at `buf[0]` with no software involved. Guard words
  intact. `DMA0CNT` falls one per result. [7]
- `isr_entries = half + done`, no overrun. [13]

**S4 - Counting at rate** (hardware counts, the CPU only reads the result)
- Ladder: 100 kSPS, then 160 MHz / N for N = 160, 40, 20, 16, 10, 8, 6, 5, 4
  (1, 4, 8, 10, 16, 20, 26.7, 32, 40 MSPS). [C.12.9]
- Per rate, over a 100 ms Timer1 window: SCCP1 events (DMA1), DMA0 transfers and
  overrun. Pass if events = transfers exactly and overrun = 0. [9, 12]
- Where it fails, the IRQSEL = 1 / `CH0DATA` variant runs as well. [C.10.2]
- SAMC = 0 against the period at every rate (2 TAD = 25 ns at 40 MSPS). [10, C.12.6]
- The highest passing rate is carried into S5 to S9.

**S5 - The grid in the data** (the triangle)
- Per rate: a one-shot window of 2048 samples, SCCP1 stopped from the DMA ISR.
- Firmware: turning points by line fit, samples per slope against the prediction
  from `dac2_slope_len()`, and the step check where the step is at least 10 LSB.
  [11, C.12.4]
- A window that fails goes out as `@DUMP`; a passing one as its evaluation numbers
  (section 2).
- The triangle model itself: period, both ends, the first step after a turn. That is
  open question 4, checked at a rate where the slope spans many samples. [C.12.3]
- Cross-check at one rate: the DAC on the FRC instead of the PLL1 VCO. The turning
  points then drift, and the rate still has to come out right against Timer1. [C.11]

**S6 - The stream with the CPU processing**
- Per rate that passed S4 - 40 MSPS included, the overrun brake keeps a storm from
  locking the board - 1 s of triggered stream. The main loop processes every half, and
  the processing is the online check of the triangle: min, max, mean and the turning
  points per half. [14, C.10.9]
- Pass: overrun = late = missed = 0, events (DMA1) = transfers, guard words intact,
  `isr_entries = half + done`.
- Processing load in per cent of the half period, from Timer1: maximum and mean.
  [C.10.9]
- `DMA0STAT` and the counters at the end. [12, C.10.10]

**S7 - Start, stop, restart, rate change**
- The first sample after start is at `buf[0]`. 10 ms after stop, a snapshot of the
  buffer is unchanged. A second start behaves like the first. [16]
- Changing `CCP1PR` between two streams: the new rate is measured and the triangle
  period in samples follows it. [17]

**S8 - The old open questions, now with valid instruments** (last before the
attempt, because they touch the clock tree)
- `CLK6DIV` at several ratios, measured with the clock monitor. That answers C.3
  directly. [C.3]
- CLKGEN6 `ON = 0`: what the clock monitor shows, and whether a one-shot burst
  completes, with the triangle showing moving or frozen values. [C.3]
- Back-to-back, 1 and 100 bursts, the triangle's period in samples: converter faster,
  or samples repeated? [C.2]
- After S8, `clock_chain_init()` again.

**S9 - The attempt: the chain as the example runs it**
- `chain run <ksps> <seconds>` sets up the chain at the rate given, starts it and
  leaves it running. The main loop processes every half. Once per second one status
  line: halves, overrun, late, missed, events against transfers, processing load.
  At the end a verdict.
- In `chain all` it runs automatically for 15 s at the highest rate that passed S4
  to S6, and for 15 s at 8 MSPS (the customer's rate) if that is a different one.
  If no rate passed, it runs at 8 MSPS anyway, so that the log shows how the chain
  breaks in the real mode of operation.
- The summary ends with `USE THIS RATE: <n> kSPS` or with the plain statement of
  which link breaks and where.
- **The recipe for the application.** When S9 passes, the firmware dumps every
  register of the chain as it stands on the silicon - clock tree, SCCP1, ADC core 5,
  DMA0, interrupt priorities - so that the application is built from what ran, not
  from what the code meant to write.
- **The processing budget** in free CPU cycles per sample at the chosen rate (about
  20 at 10 MSPS, 200 at 1 MSPS), besides the load in per cent. The real processing
  can be measured against it before it is written.
- Not covered, decided 25.09.2026 to be handled later: the application's real input
  (pin, core, source impedance and the SAMC it needs) and its real processing. The
  test uses DAC2 over RA8 on core 5 throughout.

**Time budget** (at most 1 minute, decided 25.09.2026): S0-S3 about 3 s, S4 11 rates
× 100 ms about 2 s, S5 under 1 s, S6 11 × 1 s, S7 and S8 about 5 s, S9 2 × 15 s,
UART output 1-6 s. About 55 s. A longer endurance run is `chain run <ksps> <s>`,
typed by hand when it is wanted.

**Log size:** about 170 lines (~12 KB, 1 s at 115200 baud) when everything passes -
banner 10, S0 15, S1 8, S2 15, S3 6, S4 15, S5 25, S6 12, S7 5, S8 10, S9 34 (one
status line per second plus the verdicts), summary 15. Each failing window adds a
`@DUMP` of 32 lines (2048 samples, 64 per line in 3-digit hex, ~6 KB, 0.5 s). Worst
case, every S5 window failing: about 520 lines.

## 6. Order of work

**One delivery** (decided 25.09.2026): the colleague gets the finished test and runs
it once. The steps below are internal commits on master; the colleague is asked only
after step 11. One commit per step. Each step: both builds (`tools\build.bat`, `tools\build.bat sim`)
`-Wall -Wextra` clean, and the MPLAB X board configuration through
`tools\_test_mplabx.bat`. The simulator acceptance run only when asked.

1. Read chapter 12 (clock monitor, VCO divider) and chapter 17/SCCP interrupt.
   Clock tree + clock monitor + S0.
2. SCCP1 counting + S1.
3. Core 5 single mode, CH0 interrupt, CPU-stepped DAC + S2.
4. Triggered stream in `capture.c` + S3.
5. DMA1 as a counter + S4.
6. DAC model fixes, triangle evaluator, dumps + S5.
7. Stream with processing + S6, S7.
8. S8.
9. `chain run`, S9, summary, boot record, `chain from`.
10. `tools/eval_chain.py`, tested on a synthetic log that the simulator produces.
11. Documentation: README (the command, how the colleague runs it), ANALYSIS section F
    replaced, CLAUDE.md corrected (CLKGEN6, CLKGEN13 ÷2, the new module), and a
    HARDWARE-LOG entry "prepared, not yet run".

## 7. For the colleague

```
git pull
MPLAB X: configuration EV74H48A_Curiosity_Platform_MPS512, build, program
Terminal with logging to a file, 115200 8N1 (as before)
Wait for "[boot] READY", type:   chain all
Wait for "@END" (at most 1 minute), send the log file.
If it stops without @END: reset the board, send the log including the new boot banner.
```

## 8. Risks and what to do about them

| Risk | Consequence | Mitigation |
|---|---|---|
| The VCO divider or the CLKGEN13 divider does not divide as assumed | DAC or SCCP1 out of spec, wrong rates | S0/S1 measure them. If CLKGEN13 ÷2 fails, fall back to 320 MHz (out of spec, marked in the log) and continue. |
| Clock monitor layout different from expected | S0/S8 without direct measurement | The CM measurement is an extra; the rest does not depend on it. |
| An ISR vector name (AD5CH0, CCP1, DMA1) differs from the pack's list | Link without handler, default trap | Check vector slots in the built ELF, as was done for `_CLKFInterrupt`. |
| RA8 touch-pad load | Amplitude errors read as grid errors | S2 characterises it at low rate; UREF as the comparison route. |
| `SWTRG` is written by the old path in single mode | An extra conversion at the start | The triggered stream never calls `start_burst()`. |
| A hang at a high rate | Rest of the log lost | Order safe to risky, boot record, `chain from`. |

## 9. Where the implementation departs from the plan (25.09.2026)

- **No DMA1 trigger counter.** A second DMA channel on the SCCP1 event would add one
  transfer per sample and double the DMA load the test is trying to measure - at
  20 MSPS it alone would reach the 33 M transfers/s the support figure names. The
  expected number of triggers is computed from Timer1 instead: both clocks come from
  the FRC, so their ratio is exact, and the count is right to +-3 Timer1 ticks (a few
  triggers at 40 MSPS). A single lost or repeated sample is caught by S5 instead.
- **The grid criterion is "slip", not single slopes or single steps.** Host tests of
  the evaluator (synthetic windows with DNL +-5, noise, a modelled DAC filter) showed
  that step checks at 14 LSB per sample give false alarms on clean data, and that a
  lost sample moves the slope it sits in by only half a sample. A lost or repeated
  sample shifts every later turning point by one; four turning points around it
  carry the whole shift. Only turning points between two full slopes count. Steps
  are judged from 40 LSB per sample on. Result on 2100 clean windows: no false alarm;
  a single fault found in 96-100 % of windows.
- **Slopes of about 128 samples**, not 300: more turning points per window and
  steeper slopes. SLPDAT is not limited to 50 - with a range fitted to it the triangle
  can go much faster; 87 at 40 MSPS.
- **Low-rate stages at 100 kHz** where the plan had 10 kHz (S3), 8 DAC levels instead
  of 16 (S2): the one-minute budget.
- **DAC cross-check on the PLL2 VCO divider (500 MHz)**, not on the FRC: the FRC's
  8 MHz is far below the DAC's 400 MHz minimum. It is not independent of the ADC's
  clock (same FRC), so it checks the model's scaling rather than common errors;
  Timer1 remains the independent check of the absolute rate.
- **Found while building, fixed in the firmware:** the DAC's data registers never had
  an update trigger (`UPDTRG`, p1409); `CCP1RB` was written in OC mode only; SCCP1's
  timer period raises `CCT1`, not `CCP1`.
- **Status lines of `chain run` and S9 are printed after the stream**, not during it:
  printing blocks for milliseconds, which at 40 MSPS is hundreds of halves.
