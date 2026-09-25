# dsPIC33AK512MPS512: continuous ADC acquisition through the DMA at a selectable rate

Results report, 25.09.2026. Evidence from master `fbfd883` (run 19, `chain all`) and
nano-board `8861adc` (GUI live test); the code described is master `c3bc644` / nano-board
`0e7244f`, which adds the corrected test verdict and the faster processing loop (not yet
run on the board).

## 1. What this was about

On the dsPIC33AK512MPS512 (board EV74H48A, dsPIC33 Curiosity Platform with the GP DIM) the
example is meant to prove a single sentence:

> **At a sample rate you choose, the ADC streams samples through the DMA into RAM
> continuously, and the CPU processes them on the free half of a ping-pong buffer.**

The sentence has four parts: *chosen rate*, *continuously*, *through the DMA* and *CPU
processes*. "Continuously" means an unbroken, equidistant sampling grid: every sample lies
exactly one sample period after the previous one, across buffer and block boundaries too.

## 2. The result in brief

**The sentence is proven on the board up to 8 MSPS.**

| Evidence | Measured |
|---|---|
| Continuous operation at 8 MSPS, the CPU processing every buffer half | 15 s, 120 000 509 samples, overrun 0, late 0, missed 0 |
| Further rates without any loss (continuous, with the CPU) | 100 kSPS, 1 MSPS, 4 MSPS |
| Data integrity (a DAC triangle as a known signal through the whole chain) | no lost or repeated sample up to 10 MSPS |
| Measurement against model (slope length of the triangle) | 1.000 at every rate, also with the DAC on a second PLL |
| One transfer per conversion | 6144 conversions → 6144 transfers, every buffer index correct |
| Start, stop, restart, rate change | all pass (rate change 1 → 2 MSPS: factor 2.001) |
| Live operation with a PC GUI (halt → transfer a window → restart → evaluate) | 8 MSPS, clean triangle, grid check PASS, 1.3 cycles/s |

The decisive cause of all earlier problems was **one wrongly set bit** that had been in the
code since the first day: the DMA transfer mode (section 5.4).

## 3. The architecture of the chain

```
SCCP1 (timer)  ──►  ADC core 5, Single Conversion  ──►  DMA0, Repeated One-Shot  ──►  ping-pong buffer  ──►  CPU
sample clock        one conversion per trigger          one transfer per conversion    HALF/DONE interrupt     processes
                                                                                                               the free half
```

All three links up to the buffer are hardware. None of them needs the CPU to keep going.
The CPU only learns "half A is complete" or "half B is complete" and works on the half that
is not being written.

| Link | Configuration | Source |
|---|---|---|
| Sample clock | SCCP1 in timer mode, special event trigger (`AUXOUT = 2`), period `CCP1PR` | ATDF, `CCP_CCP1CON2__AUXOUT` |
| ADC trigger | `TRG1SRC = 0x20` ("SCCP1 OC/IC Event"), `MODE = 0` (Single Conversion) | ATDF `AD_CH_CON1__TRG1SRC` |
| DMA | channel 0, `TRMODE = 1` (Repeated One-Shot), `RELOADD/RELOADC`, `HALFEN/DONEEN`, address window = the buffer | DS70005591D 13.4.8.3, p832 |
| Rate | 160 MHz / N, e.g. N = 20 → 8 MSPS; arbitrarily fine below that | – |
| Test signal | DAC2 in triangle mode on RA8 = DACOUT2 = AD5AN3 (core 5), no external wiring | Table 1, pinout |

**Clock tree.** All clocks of the chain come from the same VCO. The ADC, the trigger and the
DAC therefore stand in a fixed ratio to each other, which makes it possible to predict the
value of every single sample.

| Consumer | Clock generator | Source | Frequency | Specification (Table 40-24) |
|---|---|---|---|---|
| ADC | CLKGEN6 | PLL1 out | 320 MHz | 32–320 MHz |
| SCCP1 | CLKGEN13 | PLL1 out / 2 | 160 MHz | ≤ 200 MHz |
| DAC | CLKGEN7 | PLL1 VCO divider (1600 MHz / 4) | 400 MHz | 400–500 MHz |
| CPU, Timer1 | CLKGEN1 | PLL2 | 200 MHz | – |

All frequencies were measured on the board with the on-chip clock monitor (S0, S1).

## 4. Starting point

Before this phase there had been 16 board runs with a contradictory picture:

- A **single** ADC burst ran at exactly the rate that was set (to 0.5 %).
- A **continuous stream** delivered about 40 MSPS, whatever the setting.
- Thousands of DMA overruns per measurement point, an interrupt storm, and the CPU missed
  80 to 99 % of the buffer halves.
- The CLKGEN6 divider seemed to have no effect. The ADC seemed to keep converting with its
  clock switched off.
- A note from support said: "ADC triggers for DMA on this device have an issue. A few
  transfers are possible per one trigger."

No variant had ever streamed without errors. The triggered path (SCCP → ADC) had not
converted at all in earlier runs.

## 5. How it was done

### 5.1 Analysis sorted by question (`docs/ANALYSIS.md`)

Every earlier measurement was sorted by the instrument that produced it and by what that
instrument can say at all. Several earlier results turned out to be measurement artefacts
and were withdrawn. One example: rates measured under overload were off by a factor of ten.

The datasheet also showed that the old back-to-back approach cannot be continuous in
principle. Every burst ends after `CNT` conversions and has to be restarted by software,
which leaves a gap in the grid every time. The target architecture that remained is the
triggered chain of section 3.

### 5.2 Faults found before the first test run

Building the test turned up several configuration faults that affected every earlier run:

| Fault | Effect | Correction |
|---|---|---|
| DAC clock 320 MHz, datasheet minimum 400 MHz | DAC out of specification | PLL1 VCO divider, 400 MHz |
| SCCP1 clock 320 MHz, maximum 200 MHz | trigger out of specification | CLKGEN13 ÷ 2 = 160 MHz |
| DAC data registers without an update trigger (`UPDTRG` = 00 requires a manual `UPDREQ`, p1409) | uncertain whether the settings were taken at all | `UPDTRG = 11` |
| The period calculation of the DAC triangle returned one slope instead of two | factor 2 in the model | corrected |
| `CCP1RB` set in OC mode only | possible compare point outside the period | set in both modes |
| SCCP1's timer period raises `CCT1`, not `CCP1` | the counter would have counted nothing | both interrupts counted separately |

### 5.3 The chain test: link by link (`chain all`)

One console command, one minute, one board run. Ten stages, from safe to risky:

| Stage | Question |
|---|---|
| S0 | Are the timer, the clocks (measured with the clock monitor), the ADC core and the pin right? |
| S1 | Does SCCP1 produce the intended period? |
| S2 | At a low rate, does every SCCP1 trigger arrive as exactly one ADC result? The CPU counts both sides. |
| S3 | At 100 kHz, does every ADC result land as exactly one DMA transfer at the right buffer index? The CPU steps the DAC for this. |
| S4 | How many transfers arrive per trigger, from 100 kSPS to 40 MSPS? |
| S5 | Does the DAC triangle arrive without a lost or repeated sample? |
| S6 | Does a continuous stream with the CPU processing every half run without loss? |
| S7 | Do start, stop, restart and rate change work? |
| S8 | The old open questions, now with valid instruments |
| S9 | The use case: 15 s of continuous operation at the best rate and at 8 MSPS |

**The grid check.** A lost sample shifts every later turning point of the triangle by exactly
one sample period, a repeated one by minus one. The turning points are located to a fraction
of a sample by line fits on both slopes, and their spacing over two periods is compared
("slip").

The evaluation was tested on a PC before the first board run. The firmware code was
extracted and fed with synthetic windows containing noise, DNL of ±5 LSB and a modelled DAC
filter. Result: no false alarm in 2100 clean windows, and a single lost or repeated sample
was found in 96 to 100 % of the windows.

### 5.4 The decisive finding: the DMA mode (run 18)

The first run of the chain test narrowed the problem down to one spot:

- **S2:** SCCP1 → ADC works, for the first time on silicon. 100/100, 1000/1000 and
  10 000/10 000 results per trigger.
- **S3:** **15 conversions, but 6144 DMA transfers.** The buffer held runs of about 400
  identical values.
- **S4:** About 41 M transfers/s at *every* rate between 100 kSPS and 40 MSPS.

The cause was in `dma.c`: `TRMODE = 3`, **Repeated Continuous**. The datasheet says
(13.4.8.4/13.4.8.5, p833 f.): *"a single trigger starts a sequence of back-to-back
transfers"* and *"multiple transfers can occur with each trigger"*. Every single conversion
therefore made the DMA copy the result register at full speed until the buffer was full.

The right mode is **Repeated One-Shot**, `TRMODE = 1` (13.4.8.3, p832): one transfer per
trigger, with count and addresses reloaded automatically at the end of the block.

This one setting explains the whole history:
- "40 MSPS regardless of the setting" was the DMA's copying speed, not the sample rate.
- The overrun storms came from new triggers meeting a DMA that was still in the middle of its
  block.
- The CLKGEN6 divider that seemed to have no effect was also an artefact of this mode.
- Very likely the support statement "a few transfers per trigger" goes back to the same
  effect.

### 5.5 The proof (run 19)

After the one-bit correction the chain ran:

| Rate | Count (S4) | Grid in the triangle (S5) | Continuous with the CPU (S6/S9) |
|---|---|---|---|
| 100 kSPS | exact | clean, slip 0.03 | clean |
| 1 MSPS | exact | clean, slip 0.04 | clean |
| 4 MSPS | exact | clean, slip 0.09 | clean, CPU load 46 % |
| **8 MSPS** | **overrun 0** | **clean, slip 0.03** | **15 s clean, CPU load 92 %** |
| 10 MSPS | 732 overruns in 0.5 M | clean | – |
| 16 / 20 MSPS | ~2000–2700 overruns | samples lost | – |
| 26.7 / 32 / 40 MSPS | transfers far below triggers | the ADC converts at only 18–20 MSPS | – |

The ratio of slope length to model was **1.000** at every rate up to 10 MSPS.

The test's summary still said "NO RATE". That was the measuring instrument: the expected
counts came from a 10 ms clock measurement with 8 ppm resolution, which read +4.9 ppm. At
8 MSPS the test therefore expected 589 samples too many out of 120 M. This has since been
corrected (commit `c3bc644`).

### 5.6 The GUI in live operation

On the `nano-board` branch a browser GUI drives the chain:

1. Set the rate and send `stream on`.
2. Then in a loop:
   1. Halt the stream (the trigger first).
   2. Transfer one contiguous window of 1024 samples to the PC, binary with a CRC.
   3. Restart the stream.
   4. Show the window and evaluate it with the same grid check as the chain test.
   The counters (overrun, late, missed) are shown per cycle.

Tested live on the board: 8 MSPS, a clean triangle, verdict PASS, 4.8 M transfers per cycle
without an overrun, 1.3 cycles per second.

## 6. Open questions answered

| Question | Answer |
|---|---|
| Is the rate selectable in continuous operation? | **Yes.** Triggered through `CCP1PR`, and back-to-back follows the PLL too (8 MSPS: one burst and the 100th of a stream have the same slope length). |
| Can a variant stream without loss while the CPU processes? | **Yes, up to 8 MSPS** (15 s, 120 M samples). |
| Why does the ADC ignore CLKGEN6? | **It does not.** The divider works (measured 320/160/80 MHz). `CLK6CON.ON = 0` does not stop the generator, though, which is why "the ADC converted without a clock". |
| Why does the DAC triangle not match the model? | The DAC ran below its minimum clock and without an update trigger, and the model had a factor of 2. After the correction it matches to 0.1 %. |

## 7. Limits and assessment

**The CPU's budget.** The CPU runs at 200 MHz, and the DMA delivers the data without any CPU
effort. That leaves per sample:

| Rate | 1 MSPS | 4 MSPS | 8 MSPS | 20 MSPS | 40 MSPS |
|---|---|---|---|---|---|
| CPU cycles per sample | 200 | 50 | 25 | 10 | 5 |

In run 19 even the simple sum used as a placeholder took about 23 cycles per sample. The loop
has since been rewritten to read two samples per 32-bit access, with several passes unrolled.
Its effect on the board is still to be measured.

**The rule of thumb that follows:**

| Rate | Acquisition | Processing |
|---|---|---|
| up to ~4 MSPS | gapless | continuous, including heavier processing |
| ~8 MSPS | gapless (proven) | continuous only for light processing (sum, min/max, threshold); otherwise collect, then evaluate |
| ~10 MSPS | nearly gapless, occasional DMA overruns | collect, then evaluate |
| from ~16 MSPS | loses samples | not useful with this chain |
| 26–40 MSPS | the triggered ADC manages only ~18–20 MSPS | a different architecture is needed |

Rates above 10 MSPS would need other ways of acquisition, each one window at a time only:
- back-to-back bursts without a trigger;
- several ADC cores sampling the same signal with a time offset.

Which rate is realistic for a given application depends on how much has to be computed per
sample.

## 8. Tools in the repository

| Command / file | Purpose |
|---|---|
| `chain all` | the complete proof in under a minute, machine-readable log |
| `chain <n>`, `chain from <n>` | single stages, or continue from a stage |
| `chain run <ksps> <s>` | continuous operation at a chosen rate, one status line per second |
| `stream on <ksps>` / `stream` / `stream off` | the chain as a standing stream, the main loop processing every half |
| `stream grab` (nano-board) | halt, transfer a window in binary, restart; used by the GUI |
| `tools/eval_chain.py <log>` | re-judges every verdict from the log, evaluates dumped windows, `--png` for plots |
| `tools/adc_gui.bat` (nano-board) | browser GUI; `--fake` to run without a board |
| `docs/ANALYSIS.md` | the state of the project, sorted by question |
| `docs/HARDWARE-LOG.md` | dated record of every board run, including the predictions that turned out wrong |
| `docs/CHAIN-TEST-PLAN.md` | plan of the chain test and the reasons behind it |

## 9. Open

- The correction of the test verdict and the faster processing loop (`c3bc644` / `0e7244f`)
  are pushed but have not yet run on the board.
- DMA overruns from 10 MSPS, probably from CPU and DMA competing for the bus.
- The triggered ADC's ceiling of about 18–20 MSPS has not been investigated further.
- In the GUI screenshot the triangles end flat at the bottom, at about 650 counts instead of
  about 300. The cause is unexplained; the grid check passes regardless.
- The GUI reported "missed 2" per cycle. This very likely came from the old, slow processing
  loop in the build that was flashed at the time.
- Two faults in the measuring instruments, not in the chain: SCCP1's OC mode produces no
  events, and the clock monitor codes for the PLL outputs do not select what the ATDF says.
- The real input and the real processing of the later application are deliberately not part
  of this example yet.

## 10. Commits

| Commit | Content |
|---|---|
| `d39c8ca` | plan of the chain test |
| `cdd1ffc` | chain test, corrections to clocks, DAC and SCCP |
| `50aac65` | `stream on`: the chain as a standing stream |
| `28e88fa` | command parser raised to 32 commands |
| `fbfd883` | **DMA in Repeated One-Shot**, runs 17 and 18 |
| `c3bc644` | run 19; test verdict corrected, processing sped up |
| `18524bc`, `15fd31d`, `8861adc`, `0e7244f` | nano-board: merge, GUI cycle `stream grab`, the corrections carried over |
