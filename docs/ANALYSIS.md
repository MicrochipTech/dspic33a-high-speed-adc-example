# Where this example stands, what was measured, and how much of it holds

Status of 25.09.2026: sections A to E as of master `e2bd11a` (run 16); section F
describes the chain test that implements sections C.8 to C.12 (`chain all`, not yet run
on the board). Written to be read without the chat history
behind it, and without reading `docs/HARDWARE-LOG.md` end to end - that file is a
diary, this one is sorted by question.

## Contents

- [A. What the example is meant to demonstrate](#a-what-the-example-is-meant-to-demonstrate)
- [B. How things were measured, and what each instrument can say](#b-how-things-were-measured-and-what-each-instrument-can-say)
- [C. What was checked, and what came out](#c-what-was-checked-and-what-came-out)
  - [C.1 Does the chain carry samples at all, complete and in order?](#c1-does-the-chain-carry-samples-at-all-complete-and-in-order)
  - [C.2 Is the sample rate selectable?](#c2-is-the-sample-rate-selectable)
  - [C.3 Is CLKGEN6 the ADC's clock?](#c3-is-clkgen6-the-adcs-clock)
  - [C.4 Does the CPU get to do its work?](#c4-does-the-cpu-get-to-do-its-work)
  - [C.5 Does the console work?](#c5-does-the-console-work)
  - [C.6 Do the clock switches arrive in the hardware?](#c6-do-the-clock-switches-arrive-in-the-hardware)
  - [C.7 Can back-to-back sampling be continuous at all?](#c7-can-back-to-back-sampling-be-continuous-at-all)
  - [C.8 The mode the example needs: hardware trigger → single conversion → DMA](#c8-the-mode-the-example-needs-hardware-trigger--single-conversion--dma)
  - [C.9 Checking the chain link by link](#c9-checking-the-chain-link-by-link)
  - [C.10 Further points on the chain](#c10-further-points-on-the-chain)
  - [C.11 The DAC as the test signal, and its clock](#c11-the-dac-as-the-test-signal-and-its-clock)
  - [C.12 Numbers and facts for the chain (researched 25.09.2026)](#c12-numbers-and-facts-for-the-chain-researched-25092026)
- [D. The measurement errors we made, and how they were found](#d-the-measurement-errors-we-made-and-how-they-were-found)
- [E. What holds, what is likely, what is open](#e-what-holds-what-is-likely-what-is-open)
- [F. What the next run settles](#f-what-the-next-run-settles)

## A. What the example is meant to demonstrate

One sentence:

> **At a sample rate you choose, the ADC streams samples through the DMA into RAM
> continuously, and the CPU processes them on the free half of the buffer.**

**"Continuously" means an unbroken, equidistant sampling grid** (decided 24.09.2026):
every sample is exactly one sample period after the previous one, for as long as the
stream runs, with no gap and no offset at any buffer or burst boundary. A stream with
periodic gaps is not continuous in this sense, however small or constant the gaps are,
because a broken grid distorts the signal (spectral artefacts, phase jumps). Any path
that needs software - or anything other than the sample clock itself - to keep the
conversions going fails this requirement by construction.

Everything below is measured against that sentence. It matters that it is one
sentence with four parts - *chosen rate*, *continuously*, *through the DMA*, *CPU
processes* - because the parts were proven separately and they are not equally far
along.

## B. How things were measured, and what each instrument can say

Five instruments were used. Their limits are the reason several early results had to
be withdrawn, so they are worth stating before the results.

**Timer1 as a stopwatch.** A free-running 32-bit counter at 12.5 MHz, fed from the
CPU clock branch (PLL2), while the ADC runs off PLL1. The two branches cannot flatter
each other. Its own calibration is checked at every sweep: 1 250 001 ticks per 100 ms
against 1 250 000 expected, in every run. **This one is trustworthy.**

**The counters in the DMA handler** - `dma_overrun`, `late_service`, `proc_missed`,
and since run 16 also `isr_entries`, `half_events`, `done_events`, `burst_starts`.
They count events the handler *sees*, which is not always the same as events that
*happen*; see the overrun section below for the one case where that difference
matters a lot.

**The self-test on the internal reference.** Samples the ADC's internal 15/16·VDD
reference through the whole chain and checks the mean. It proves the chain is wired
up. It samples a **constant**, so it cannot distinguish a working converter from a
result register that never changes. This limit was not stated in the first ten runs
and should have been.

**The DAC triangle.** A known, changing signal from the on-chip DAC2, routed to the
ADC *inside the chip* over the UREF line (`UREFCON.INSEL = 7`, read as `ANn7`, which
every ADC core has). No pin, no wire. It counts nothing - it shows the samples. This
is the strongest instrument in the set, and it is the only one that can speak about
order and completeness.

**The one-shot capture.** `capture_oneshot_n(N)` runs N bursts back to back, the DMA
interrupt itself stopping the stream after the last. Afterwards nothing is writing
the buffer, so the window is contiguous by construction. This exists because reading
the buffer while the stream runs produces torn data at these rates - see the
measurement-error section.

## C. What was checked, and what came out

### C.1 Does the chain carry samples at all, complete and in order?

**Checked with:** the DAC triangle through UREF, captured as one contiguous buffer,
judged on peak-to-peak, the largest step between neighbouring samples, and slope
reversals. Run 14 (`docs/logs/` has run 15 and 16; run 14's numbers are in the
HARDWARE-LOG entry).

**Result: yes, proven.** The raw dump shows the triangle - a clean monotonic fall
from 3728 to 629 across the window, no reversal, largest step 90 counts out of a
swing of 3240 against a limit of 405. Nothing is missing, nothing is out of order,
across the half boundary and across the burst restart.

**Uncertainty: none worth naming.** This is the one result that rests on looking at
the data rather than on counting events, and it has been reproduced.

### C.2 Is the sample rate selectable?

Four mechanisms were tried. The table is the short version; the detail is below it.

| Mechanism | Result |
|---|---|
| Sample time `SAMC` | No effect on the rate (run 4) |
| ADC repeat timer, `TRG2SRC = 3`, `RPTCNT` | Register holds the value, rate unchanged (runs 5, 6, 7) |
| CLKGEN6 divider `CLK6DIV` | Register holds the value, `DIVSWEN` and `CLKRDY` confirm. **Effect on the rate never measured with a valid instrument** - runs 8 and 9 measured it under overrun load, which is void (see section C.3) |
| PLL1 output dividers | **Works for a single burst. In continuous streaming, apparently not** (runs 15, 16) |

**How the PLL result was measured:** one burst of 2048 conversions, timed with
Timer1, at each of fourteen settings from 4 to 40 MSPS. Corrected for a fixed
overhead (below), every one of the fourteen lands within 0.5 % of its nominal rate,
and the overhead itself varies only between 11.0 and 12.2 µs across a factor of ten
in rate. **A rate-dependent error could not be removed by subtracting a constant**,
so this is solid: a single burst really does run at the configured rate.

**And the contradiction:** in continuous streaming at the same settings, the measured
throughput is about 40 MSPS at *every* setting. Each sweep point moves 2000 buffer
halves in 48.7 to 50.4 ms whatever the rate was set to - a three per cent spread
while the setting spans a factor of ten.

**Two readings are still open, and they are not the same thing:**

- The converter really runs flat out in continuous operation and the setting only
  takes effect for an isolated burst.
- The converter runs at the set rate and each conversion lands in the buffer more
  than once. Microchip acknowledges exactly that for this silicon: *"ADC triggers
  for DMA on this device have an issue. A few transfers are possible per one
  trigger."* In that case the buffer would fill faster than samples are converted,
  with repeated values, and 40 MSPS would be the **transfer** rate rather than the
  sample rate.

Nothing measured so far distinguishes them, because nothing measured so far counts
*conversions* - the counters count DMA events, and the time is divided by an assumed
number of samples.

### C.3 Is CLKGEN6 the ADC's clock?

**What the documents say - all of them, and they agree** (checked 24.09.2026):

| Source | Statement |
|---|---|
| DS70005591D 16.4.3, p1319 | *"The ADC module is clocked from the Clock Generator 6. This input clock is divided by four to get the analog core clock (TAD). The conversion takes two ADC analog core clock cycles."* Input 32 to 320 MHz, i.e. 4 to 40 MSPS. |
| Table 16-1, p1223 | Clock Source: CLKGEN6, Max Input Clock 32 MHz to 320 MHz. |
| Table 12-2, p723 | Clock Generator 6 → All Sources → ADC. PLL1 is one of CLKGEN6's selectable sources; it does not feed the ADC directly. |
| Table 40-39, p2033 | AD50 TAD = 4/F_IN, 12.5 to 125 ns; AD51 40 Msps. Note 4 says 1.5 TAD per conversion where 16.4.3 says 2 - the datasheet contradicts itself there, irrelevant to the source. |
| ATDF, `dsPIC33AK-MP_DFP` 1.3.185 | OSCCTRL parameter `CLK_GEN_6 = "ADC"`, caption 320. No ADC register has a clock-select or clock-divide field. |
| `microchip-pic-avr-examples/dspic33ak-curiosity-adc-40msps`, MPS512 variant | MCC `clock.c`: `CLK6CON = 0x29500` (NOSC = PLL1 out), `CLK6DIV = 0`, PLL1 = 320 MHz. ADC Integration mode, software start, back-to-back - our configuration. 39.72 MSPS measured, **without DMA**. The divider is never exercised. |

Our `clock.c` sets CLKGEN6 bit for bit as that example does, and writes `INTDIV`
(bits 30:16) and `FRACDIV` into the right fields.

**What follows from that:** the chain is PLL1 → CLKGEN6 → ÷4 → TAD → 2 TAD per
conversion. The PLL1 result of section C.2 - a single burst follows the PLL to 0.5 % -
is therefore not evidence *against* CLKGEN6 but exactly what this chain predicts: the
PLL changes CLKGEN6's input.

**The two observations that seemed to contradict it both rest on withdrawn
instruments:**

- **"The CLKGEN6 divider has no effect" (runs 8, 9).** Measured in streaming under
  overrun load: 39.7 to 40.8 MSPS at every ratio. The same instrument made the PLL look
  ineffective in run 10 (41 to 44 MSPS at every setting), and the one-shot measurement
  of run 14 then showed that the PLL works. **The divider has never been measured with
  the one-shot instrument.**
- **"The ADC keeps converting with CLKGEN6 off" (`test clkoff`, 260 halves in run 10,
  390 in run 14).** What was counted is halves arriving, not conversions. If each
  trigger produces several DMA transfers (section C.2, second reading), the buffer fills
  without new conversions. Neither the values nor the burst duration were checked in
  that state. No datasheet text describes a mechanism that keeps CLKGEN6 running with
  `ON = 0`; a clock-request path like the one `PLLxCON.ON` has would be one, but that is
  a hypothesis, not a finding.

**Result: CLKGEN6 is the ADC's clock as far as every document goes, and nothing
measured validly contradicts it.** The question "why does the ADC ignore CLKGEN6" is
replaced by "does it?" - to be answered by two scenarios:

- `CLK6DIV` at several ratios with PLL1 fixed, timed as a one-shot burst like run 14.
- `clkoff` with a one-shot burst: does it complete at all, how long does it take, and
  does the DAC triangle through UREF show moving or frozen values.

### C.4 Does the CPU get to do its work?

**Checked with:** `proc_missed`, the count of completed halves the main loop never
saw, measured while the main loop calls `capture_service()`.

**Result: no, at any setting.** 1614 to 1977 of 2000 halves missed. The reason is
established: every lost sample raises the DMA channel interrupt, that event has no
enable bit of its own (DS70005591D 13.6.1 - `DMA0CH` has HALFEN, DONEEN and MATCHEN
only), and at these rates the handler is entered about a million times a second. The
interrupt is priority 4, the UART receive interrupt priority 1, so the console dies
with the main loop - which is how runs 7 and 9 ended, silently.

**This is the part of the example's sentence that is furthest from working.**
"The CPU processes them on the free half" is currently false at every rate the sweep
walks.

### C.5 Does the console work?

**Checked with:** typing at it from an idle boot, after the firmware was changed to
run nothing on its own.

**Result: yes.** `rx = 0` through runs 1 to 7 was the receive interrupt starving
behind the DMA interrupt, not wiring, not the terminal, not the PPS configuration.
Idle at boot, everything answers.

### C.6 Do the clock switches arrive in the hardware?

**Checked with:** `test clock` - every divide ratio written and read back, with
`DIVSWEN` and `CLKRDY` awaited and checked.

**Result: yes, every one.** And it is worth being precise about what that proves:
**the register holds the value.** It says nothing about the clock that comes out,
which is exactly the trap run 8 fell into. The line is printed under the test's own
PASS so that nobody reads it as more than it is.

### C.7 Can back-to-back sampling be continuous at all?

**Decided in discussion, 24.09.2026, from the datasheet - no board run needed: no.**

#### What a burst is

In this project a **burst** is a closed run of `CNT` conversions that the ADC executes
one after the other and then ends by itself. The channel runs in Integration mode
(`MODE = 2`); DS70005591D 16.4.4 (p1321): *"the first conversion is initiated by a
trigger selected by TRG1SRC and all subsequent conversions are executed by a trigger
selected by TRG2SRC"*.

1. **Start.** Software writes `ADnSWTRG` (`TRG1SRC = 1`, `adc_start_burst()`). That
   starts the first conversion.
2. **Keeping going.** `TRG2SRC = 2` (back-to-back) starts every further conversion
   *"immediately after the previous conversion is finished"* (16.4.5, p1322). No timer
   and no clock sets the spacing - only the duration of one conversion (2 TAD from
   CLKGEN6) does.
3. **Per conversion.** `IRQSEL = 0` makes every single conversion raise the channel
   event "ADCn Done CH0" (16.4.7, p1323). That event is the DMA trigger; the DMA copies
   the result from `ADnCH0RES` into the buffer. (`ADnCH0DATA` is the burst's
   accumulator in this mode, not the single result.)
4. **End.** After `CNT` conversions the ADC stops by itself. `CNT` is tied to the
   buffer: 2 × half length, 2048 by default. One burst fills the whole buffer once.

```
SWTRG ─► C1 C2 C3 … C2048 ─► stop
          │  │  │      │
          one DMA transfer per conversion ─► buffer [half A | half B]
                                               HALF IRQ ┘      └ DONE IRQ
```

**Why there are bursts at all.** Free-running conversion exists only in the multisample
modes, and there it is bounded by `CNT` (at most 65535). Single Conversion mode knows no
`TRG2SRC` (p1322), and the back-to-back value is reserved for `TRG1SRC` (Table 16-3,
p1226). A continuous stream built on back-to-back therefore has to restart the burst
again and again.

**Single burst and stream** are two different things, and the two measurements of
section C.2 differ exactly here:

- **Single burst** - the one-shot measurement of runs 14 and 15,
  `capture_oneshot_n()`: exactly one burst, Timer1 measures its duration, afterwards
  nothing writes the buffer. Here the rate follows the PLL setting to 0.5 %.
- **Stream** - the DMA DONE interrupt starts the next burst immediately, thousands of
  times. Between two bursts there is every time a short gap for the jump into the ISR
  and the restart. This is where the flat ~40 MSPS at every setting were seen.

`test bursts` (1, 10 and 100 bursts at the same setting) was built to probe exactly
this difference.

#### The restart in the DMA interrupt

The end of a burst raises an interrupt, and that interrupt starts the next burst - but
it is **the DMA's DONE interrupt, not the ADC's**:

- The ADC has an end-of-burst interrupt of its own, but it is not used. `IRQSEL = 0`
  makes the channel event fire on every conversion, because the DMA needs it at that
  pace; its CPU interrupt is switched off (`AD3CH0IE = 0`), otherwise it would fire 40
  million times a second.
- The DMA counts transfers. After the first half it sets HALF, after the second DONE;
  both raise the DMA0 interrupt.
- In `dma0_event()` (`capture.c`), at DONE: first a pending input switch is applied
  (the only moment the channel is idle), then `start_burst()` → `adc_start_burst()` →
  software trigger, and the ADC begins the next `CNT` conversions.

**The continuous stream therefore hangs on software. Without the ISR there is no next
burst.** Three consequences:

1. **The stream has gaps.** Between the last conversion of one burst and the first of
   the next lie interrupt latency, handler and trigger write. On a clean single burst
   that is the 4 to 5 µs residual measured in section C.2 - at 40 MSPS about 200
   samples per burst that are never converted.
2. **The restart competes with the overrun storm.** Every lost transfer raises the same
   DMA0 interrupt, which has no separate enable bit. With the handler entered about a
   million times a second, when the restart happens depends on when DONE is found
   among all the overrun entries.
3. **So the stream is not continuous** in the example's sense: it is a chain of blocks
   with gaps whose length is set by the CPU load.

#### Why the other multisample modes do not help

Back-to-back (`TRG2SRC = 2`) exists only inside a multisample mode, and every one of
them ends (DS70005591D 16.4.4, p1321-1322):

- **Integration** (`MODE = 2`): after `CNT` conversions. `CNT` is 16 bits and has no
  "endless" value.
- **Window** (`MODE = 1`): `TRG1SRC` is a gate signal whose level enables the
  `TRG2SRC` triggers; *"The number of conversions is limited by the CNT[15:0] bits"*,
  and the channel is done when the gate drops *or* `CNT` is reached.
- **Oversampling** (`MODE = 3`): after `ACCNUM` (4 to 32) accumulated results, which
  are sums, not single samples (`ACCBRST` only keeps other channels from interrupting
  it).

Single Conversion mode does not use `TRG2SRC` at all (16.4.5, p1322).

#### What that means

So a back-to-back stream is a chain of bursts, and in this example each next burst is
started by software from the DMA DONE interrupt (`dma0_event()` → `start_burst()` →
`ADnSWTRG`). Between the last conversion of one burst and the first of the next lie the
interrupt latency, the handler and the trigger write - about 4 to 5 µs on a clean single
burst (section C.2), and whatever the overrun storm adds under load, since every lost
transfer enters the same handler. **Inside a burst the samples are equidistant; across
burst boundaries they cannot be.** A larger `CNT` (up to 65535, independent of the
buffer) would make the gaps rarer, not absent.

Moving the restart into hardware does not rescue it either - for example a second DMA
channel, triggered at the end of the first one's block, writing `ADnSWTRG`. The gap
would be small and constant, but it would still not be one sample period, so the grid
would carry a fixed offset at every burst boundary. The requirement is an unbroken
grid (top of this document), not a small gap.

The DMA side is not the problem: channel 0 already runs in Repeated Continuous mode
(`TRMODE = 3`, `RELOADD = RELOADC = 1`, `dma.c`), reloads its destination and count at
the end of each block and waits for the next trigger without any software. It is the
ADC that stops delivering triggers when the burst ends.

This rules back-to-back out for the example's sentence, whatever the rate question in
section C.2 turns out to be. It stays useful as an instrument - a single burst is the
cleanest rate measurement we have - but not as the way the example samples.

Microchip's own `dspic33ak-curiosity-adc-40msps` (MPS512 variant) does not contradict
this: it runs one burst of 800 conversions, without DMA and without restart. It shows
the converter's speed, not a continuous stream.

**What remains as the way to the goal is the triggered family:** Single Conversion
mode, `TRG1SRC` on a periodic hardware trigger (SCCP or PWM), one conversion per
trigger, one DMA transfer per conversion. The grid then comes from a hardware timer by
construction and the interrupt is left with the processing only. Microchip's MC106
variant of the same repository samples this way (SCCP1 as a 5 MHz trigger). This family
has never been tested correctly here (see the SCCP errors below).

### C.8 The mode the example needs: hardware trigger → single conversion → DMA

**Decided in discussion, 24.09.2026. This is the target architecture; it has not yet
run correctly on silicon.**

```
SCCP/PWM ──► ADC (1 conversion per trigger) ──► DMA (Repeated Continuous) ──► buffer
 hardware          hardware                          hardware
                                          HALF/DONE IRQ ──► CPU: processing only
```

Three links, each a hardware module reacting to an event of the previous one. None of
them needs the CPU to keep going.

**1. SCCP1 (or PWM) - the sample clock.** SCCP1 runs as a timer counting 0 to `CCP1PR`
and rolling over. Once per period its compare match on `CCP1RB` is put out as the
special event trigger (`CCP1CON2.AUXOUT = 2`, `sccp.c`).

- The period is `PR + 1` ticks of the SCCP clock - exact, and unchanging while nobody
  writes the register.
- Clock: CLKGEN13 (`CLKSEL = 1`) fed from PLL1, the same source as the ADC's CLKGEN6.
  One common source puts every trigger at a fixed phase to the ADC clock; two
  independent clocks would let the ADC's synchroniser take the trigger one clock early
  or late, which is jitter in the grid. A Microchip support case recommends exactly this
  setup.
- Rate = f_CLKGEN13 / (`PR` + 1). **Corrected 25.09.2026:** SCCP1 may be clocked at
  200 MHz at most (Table 40-24, section C.12.1), so CLKGEN13 = PLL1 out / 2 = 160 MHz,
  as in Microchip's MC106 example: 4 ticks = 40 MSPS, 5 = 32, 8 = 20, 16 = 10,
  20 = 8, 40 = 4, 160 = 1 MSPS. (The first version of this section said 320 MHz -
  which is what `clock.c` does today, out of specification.)
- A PWM generator can serve instead - it has ADC trigger outputs of its own - but it
  runs off CLKGEN5 (Table 12-2, p723).

**2. The ADC in Single Conversion mode - one conversion per trigger.**
`adc_set_mode_single()`: `MODE = 0`, `TRG1SRC` = the SCCP1 trigger (32, `0x20` "SCCP1
OC/IC Event" per the ATDF). `TRG2SRC` and `CNT` are not used in this mode (16.4.5,
p1322).

- Each trigger starts sampling (`SAMC`), then the conversion (2 TAD = 25 ns at
  320 MHz). The result lands in `ADnCH0RES`, and the channel event (`IRQSEL = 0`)
  signals it.
- **Nothing ends.** Unlike a burst there is no count; the ADC waits for the next
  trigger indefinitely.
- Condition: sample time plus conversion must be shorter than the trigger period,
  otherwise the next trigger meets a busy channel and is lost or delayed.
- The sampling instant now depends on the trigger alone. The conversion time no longer
  sets the rate; it only has to fit inside the period.

**3. The DMA in Repeated Continuous mode - one transfer per conversion.** The ADC's
channel event triggers DMA channel 0, which copies one value from `ADnCH0RES` to the
next buffer address (`dma.c`). HALF after the first half, DONE at the end of the block;
then the DMA reloads destination and count by itself and writes from the start again.
The CPU only learns "half A is complete" or "half B is complete" and processes the half
that is not being written - the ping-pong part of the example's sentence.

**In time:**

```
SCCP:   ↑       ↑       ↑       ↑       ↑       ↑        period T, exact
ADC:    [S|C]   [S|C]   [S|C]   [S|C]   [S|C]   [S|C]    S = sample, C = convert
DMA:        →b0     →b1     →b2  …  →b1023 (HALF)  →b1024 …  →b2047 (DONE) →b0 …
CPU:                             processes half A while B fills
```

The sampling instants are T apart across every half boundary. A fast or slow CPU does
not change that; a CPU too slow would miss halves, but the grid itself would stay
intact.

**Where the chain can still break** - what the test scenarios have to check:

1. **The trigger does not arrive.** This is what the three errors of runs 5 to 7 were
   (code 34 instead of 32, `AUXOUT` 1 instead of 2, the wrong clock).
2. **The DMA loses a transfer (overrun):** a hole in the grid. The DMA is said to
   manage about 33 M transfers/s (support statement), which caps the rate.
3. **More than one transfer per trigger** - the issue Microchip acknowledges for this
   silicon: samples appear twice and the grid is stretched.

**Acceptance criterion, derived from the requirement at the top:** a scenario passes
only if it shows `overrun = 0` **and** exactly one transfer per trigger - for example
triggers counted against transfers, or the DAC triangle's period in samples against
the set rate - over a stream long enough to matter, with the CPU processing throughout.

### C.9 Checking the chain link by link

**Decided in discussion, 24.09.2026:** before the chain of section C.8 is judged as a
whole, every component is checked in its own function. The instruments are proposals
from the discussion; nothing in this section has run yet.

**The method:** start at a low rate (1 to 10 kHz), where the CPU can take an interrupt
for every single event and count all three ends of the chain - SCCP1 events (`CCP1IF`),
ADC results (the channel interrupt), DMA transfers (`DMA0CNT` / `DMA0DST` read back).
Only once every link holds there, move up the rate, where the CPU can no longer count
each event and the hardware has to do it (Timer1 against the DMA's own count, the DAC
triangle).

**The seven questions of the chain:**

| # | Question | How to check | Already shown? |
|---|---|---|---|
| 1 | Does SCCP1 produce the intended period? | SCCP1 alone, no ADC. Its clock: read `CCP1TMR` twice across a known number of Timer1 ticks → CLKGEN13 frequency. Its period: count `CCP1IF` against Timer1 at a low rate and compare with `(PR + 1) / f_CLKGEN13`. | No. |
| 2 | Can these events trigger the ADC? | ADC in Single mode, `TRG1SRC = 32`, channel interrupt to the CPU at low rate: one ADC result per SCCP1 event, counted both sides. | No - runs 5 to 7 had three errors in exactly this path. |
| 3 | Does the ADC really convert on that trigger? | A known changing signal: the DAC triangle through UREF, read from `ADnCH0RES` in the same interrupt. The values must follow the triangle - a frozen result register would repeat itself. | No (only in back-to-back mode). |
| 4 | Can the ADC trigger the DMA? | DMA `CHSEL` = "ADCn Done CH0" in Single mode: `DMA0CNT` falls by one per ADC result. | Yes in back-to-back mode (run 14); the event is the same with `IRQSEL = 0`, but not yet seen in Single mode. |
| 5 | Does the DMA fetch the value from the ADC? | At low rate compare the value the ISR reads from `ADnCH0RES` with the value the DMA put into the buffer at the same index. | Yes in back-to-back mode (run 14, the triangle arrived). |
| 6 | Does the DMA store it in the buffer? | Buffer contents against the ISR's reading as in 5; guard words behind the buffer untouched. | Yes in back-to-back mode (run 14). |
| 7 | Does the DMA restart at the beginning of the buffer by itself? | After DONE, with no software touching the channel: the next transfer lands at `buf[0]`, `DMA0DST` back at the start, guard words intact. | Implicitly yes: in back-to-back streaming the DONE handler restarts only the ADC (`start_burst()` → `adc_start_burst()`), never the DMA, and run 14's triangle ran across that boundary. In Single mode not yet seen. |

**Further points of the chain, raised in the same discussion:**

| # | Question | How to check |
|---|---|---|
| 8 | Do SCCP1 and ADC really run from one clock? | Frequencies of CLKGEN6 and CLKGEN13 each measured (as in 1), both `NOSC` = PLL1 read back. Two sources would put jitter into the grid (section C.8). |
| 9 | Exactly **one** conversion per trigger, and **one** transfer per conversion? | Counts of 1, 2 and 4 must be equal - at low rate by the CPU, at high rate by `DMA0CNT` against SCCP1 periods over a Timer1 window. This is Microchip's known "few transfers per trigger" issue and the acceptance criterion of section C.8. |
| 10 | Does sample time plus conversion fit into the period? | At each rate: `SAMC` + 2 TAD against `PR + 1`. **The ADC does not report a lost trigger** (checked in DS70005591D, see below), so a trigger that meets a busy channel can only show as a count difference in 9 or a double step in 11. |
| 11 | Is the grid equidistant in the data? | The DAC triangle as a linear ramp: the step between neighbouring samples must be constant within noise. A lost sample shows as a double step, a repeated one as a zero step, a gap as an offset. This is the only instrument that sees the grid itself rather than counting events. |
| 12 | Does the DMA keep up at the rate (overrun)? | `dma_overrun = 0` over the stream, with and without the CPU processing (the CPU's own bus accesses compete with the DMA). |
| 13 | Do HALF and DONE arrive, and only they? | `isr_entries = half_events + done_events`, two per block, no storm. The interrupt carries notification only. |
| 14 | Does the CPU keep up with the processing? | `proc_missed = 0` and `late_service = 0` while the main loop processes each half. |
| 15 | Does it hold over time? | A long stream (seconds to minutes): number of transfers = rate × time by Timer1, all counters still zero at the end. |
| 16 | Start, stop, restart | The first sample after start is at `buf[0]`; after stop nothing writes any more; a second start behaves like the first. |
| 17 | Rate change | Changing `CCP1PR` between two streams sets the new rate (1 at each point). Whether it may change during a stream without breaking the grid is a separate question - the example does not need it. |

**What the ADC reports about lost triggers and results - nothing** (DS70005591D, checked
24.09.2026):

- The status registers carry ready bits only. `ADnSTAT.CHxRDY` and `ADnRSTAT.CHxRRDY`:
  *"set by hardware when the corresponding channel x conversion result is written into
  ADnCHxRES register. The bit is cleared by hardware when ADnCHxRES register is read"*
  (p1262). There is no overrun, overwrite or trigger-collision bit in the ADC register
  set.
- `ADnDATAOVR` is not an overrun register despite its name: it is the value that
  replaces every result in Test mode (p1261, 16.4.8).
- The trigger section (16.4.5, p1322) and the priority scheme (16.4.1, p1319) say
  nothing about a trigger that arrives while the channel is still sampling or
  converting - neither that it is queued nor that it is dropped. Only for
  back-to-back and oversampling does it say timing "can be delayed" by other channels.
  With one channel in use there is no competing channel, but the behaviour on a busy
  channel is undocumented.

**Consequence for the test:** a lost trigger and a lost or doubled result can only be
seen from outside the ADC - by counting (9: SCCP1 periods against DMA transfers over a
Timer1 window) and in the data (11: the ramp step). Both instruments are therefore
required, not optional. And point 10 has to be kept with margin: the period must be
comfortably longer than `SAMC` + 2 TAD, because a violation would not announce itself.

### C.10 Further points on the chain

**Raised in discussion, 24.09.2026.** Points 1 to 3 could break the grid without any
counter noticing; the rest concern configuration, instruments and the CPU.

**Could break the grid silently:**

1. **The ADC's periodic offset calibration.** DS70005591D 16.4.13, p1326: *"The
   offset calibration procedure takes 14 TAD cycles"*, it runs on request (`CALREQ`)
   or *"automatic, periodically"* when `ACALEN` (`ADnCON[28]`) is set, every 1 s to
   1 h (`CALRATE`). *"The offset calibration has the lowest priority, and it is
   delayed when a conversion is in progress. The ADC must idle a few ADC clock cycles
   to start the calibration"* (`CALCNT`). In back-to-back there is no idle time; in
   the triggered mode there is idle time between every two triggers - so a calibration
   can start in a gap and still be running when the next trigger arrives. What happens
   to that trigger is not documented. **The configuration must hold `ACALEN = 0` and
   never set `CALREQ` during a stream, and the register dump must show it.** Our code
   does not touch `ACALEN`, so it is at its reset value; that is to be read back, not
   assumed.
2. **Where "a few transfers per trigger" could come from.** Hypothesis, not a finding:
   if the DMA trigger is level-sensitive and hangs on the result-ready signal -
   `CHxRRDY`, which is cleared only when `ADnCH0RES` is read (p1262) - the DMA can
   fire again before its own read has cleared it. That would be exactly the issue
   Microchip acknowledges. A variant worth testing: `IRQSEL = 1`, which in Single
   Conversion mode also raises the event *"after each conversion"* (16.4.7, p1323),
   with the DMA reading `ADnCH0DATA` instead of `ADnCH0RES`. The exact wording and any
   workaround in the errata DS80001162E are to be looked up and quoted here.
3. **Exactly one event per SCCP1 period.** In output-compare mode SCCP1 has two
   compare points (`CCP1RA = 0`, `CCP1RB = PR/2`, `sccp.c`). Whether `AUXOUT = 2`
   fires once or twice per period has not been checked. Twice would double the rate,
   and not necessarily with equal spacing. Check at low rate: `CCP1IF` count and ADC
   results against Timer1 (section C.9, points 1 and 2).

**Configuration and order:**

4. **Start and stop order.** Start: DMA armed, then ADC on and ready, then SCCP1 last.
   Stop: SCCP1 first, then the rest. Otherwise the first trigger meets an ADC or DMA
   that is not ready, and the first sample is undefined - or the last trigger arrives
   into a half-dismantled chain.
5. **Only one active channel on the core.** The fixed priority scheme (16.4.1, p1319)
   delays higher-numbered channels. A forgotten second channel - the self-test's, for
   instance - would shift the grid. The register dump must show one channel enabled.
6. **The rate resolution.** With SCCP1 on CLKGEN13 at 160 MHz (its limit is 200 MHz,
   section C.12.1) the rates are 160 MHz / N: 40, 32, 26.7, 22.9, 20, ... MSPS, finer
   further down. Finer steps only by
   moving PLL1, and PLL1 moves TAD and the trigger clock together. Which rates the
   example must offer is still to be decided.

**Instruments:**

7. **Counting triggers in hardware.** Above a few hundred kHz the CPU cannot count
   triggers. A second DMA channel, or a second SCCP in counter mode, on the same SCCP1
   event would count them in hardware, so that "triggers = transfers" (section C.9,
   point 9) can be checked directly at 20 MSPS, not only through the ramp.
8. **Telling analog settling from lost samples.** If `SAMC` is too short for the source
   impedance of UREF/DAC at a high rate, the ramp is distorted although the grid is
   intact. The ramp test must separate step faults (double step = lost, zero step =
   repeated) from amplitude faults (a smooth deviation), or it will report grid
   errors that are really analog ones.

**The CPU:**

9. **The processing budget.** "The CPU processes" needs a number. At 10 MSPS with 1024
   samples per half there are 102 µs per half - about 20 CPU cycles per sample at
   200 MHz. The example has to say what its processing does per half and measure how
   much of the budget it uses, rather than only reporting `missed = 0`.
10. **Bus contention between CPU and DMA.** While the CPU reads one half the DMA writes
    the other; both compete for RAM. Whether the buffer and the stack should sit in
    different RAM banks is part of whether section C.9, point 12 (no overrun under CPU
    load) holds.

### C.11 The DAC as the test signal, and its clock

**Discussed 25.09.2026.** The ADC is tested with the chip's own DAC2 in Triangle Wave
mode. The triangle is preferred over the ramp (sawtooth): the ramp has a jump over the
full swing once per period, with a settling transient behind it; the triangle only has
turning points where the slope changes sign, and an evaluator that predicts the signal
knows where they are.

**Route.** DACOUT2 is RA8 = AD5AN3, a core-5 input. The code today does not use the
pin but the internal path: `UREFCON.INSEL = 7` puts DAC2 on the UREF line, which every
core reads as `ANn7` (`dac.c`). The reason given there is that RA8 runs through the
board's touch-pad network, which loads the output, and that the pin route needs core 5.
**Decided 25.09.2026: the test goes over RA8.** DACOUT2 → RA8 → AD5AN3, sampled on
ADC core 5. UREF stays available as a variant. What follows from the decision:

- The ADC work moves to **core 5**. `board.h` defaults to core 3 (AD3AN5, mikroBUS A)
  but already carries `DAC_ADC_CORE 5` and `DAC_ADC_PINSEL 3` for this route. The DMA
  trigger (`CHSEL`) becomes "ADC5 Done CH0"; the trigger code for SCCP1 in `TRG1SRC`
  does not depend on the core.
- The signal is on a pin, so a scope can watch exactly what the ADC samples - an
  independent look at the DAC that the internal route cannot give.
- The board's touch-pad network on RA8 loads the DAC output. That is exactly the case
  of section C.10, point 8: the amplitude and the settling within `SAMC` may suffer
  while the grid is intact. The load has to be characterised first - at a low rate,
  where `SAMC` is long, the triangle must come out undistorted - before a step fault at
  a high rate is read as a grid fault.

**Clock.** (Section C.12.1 adds: the DAC needs 400 to 500 MHz, so today's 320 MHz is
out of specification; the common source becomes the PLL1 VCO, with the DAC on its VCO
divider output at 400 MHz. The arguments below hold for that as well - they need one
VCO, not one output.) The DAC already runs from the same source as the ADC: `clock.c` sets
`CLK7CON = 0x29500` (CLKGEN7, NOSC = PLL1 out, no divider), exactly as CLKGEN6 for the
ADC. The trigger (SCCP1 on CLKGEN13) is meant to hang on PLL1 as well (section C.8).

Advantages of one common source for ADC, SCCP1 and DAC:

1. **Every sample becomes predictable.** DAC steps and sampling instants stand in a
   fixed rational ratio, so the value each index *must* hold can be computed and
   compared sample by sample. That is the strongest grid test there is: a lost or
   repeated sample shows as a deviation at a definite index, not as a statistical
   effect.
2. **No beat.** With two independent clocks the phase between DAC step and sampling
   instant drifts slowly, the step size wobbles by ±1 DAC step, and a grid fault can
   only be separated from that statistically.
3. **Reproducible.** Every capture with the same settings looks the same, including
   where a fault sits.
4. **Deterministic, not sporadic.** The DAC switches its steps on clock edges. If the
   sampling instant falls on such an edge, the ADC catches the settling. With a common
   clock that happens always or never; with independent clocks it happens now and then
   - and would look like a grid fault.

Disadvantages:

1. **Errors common to both sides are invisible.** If PLL1 is off, DAC and ADC are off
   together and the triangle's period in samples still looks right. The absolute rate
   has to be measured independently - Timer1 on PLL2 does that already.
2. **The repeat test is weakened.** The idea so far was: the triangle's period in
   seconds is fixed, so its period in samples reveals the true sample rate. That still
   holds while the rate is set by `CCP1PR` with PLL1 fixed. If the rate is set by PLL1,
   the DAC scales along and the period in samples does not move.
3. **Clock feedthrough becomes a pattern.** DAC switching disturbances hit every sample
   at the same phase - a fixed offset or pattern instead of noise. Harmless for the
   grid question, but to be known when judging amplitude (section C.10, point 8).
4. **Fewer DAC codes covered.** With an integer ratio the samples always hit the same
   DAC steps. Bad for ADC linearity, which is not what this example tests.

**Proposal:** keep the common source for the grid test, because it allows a prediction
for every sample; the absolute rate is checked by Timer1 anyway. Add one scenario with
the DAC on an independent source (PLL2 or FRC) as a cross-check against errors common
to both sides.

**Prerequisite:** the DAC model has to be exact before the DAC can serve as a
reference. It is not today: `dac2_period_ns()` is about eight times off what a capture
shows, and `DACLOW` is not reproduced - the upper end matches `DACDAT`, the lower end
does not. Getting the triangle model right (period, both ends, step per DAC clock) is a
test step of its own, before any grid verdict rests on the triangle.

### C.12 Numbers and facts for the chain (researched 25.09.2026)

**C.12.1 Clock limits - two of our generators run out of specification.**
DS70005591D Table 40-24, "Peripheral Input Clock Timing Specifications" (p2016):

| Module | Min | Max | Ours today |
|---|---|---|---|
| ADC | 32 MHz | 320 MHz | CLKGEN6 = PLL1 out = 320 MHz - at the limit, in spec |
| DAC | **400 MHz** | 500 MHz | CLKGEN7 = PLL1 out = **320 MHz - below the minimum** |
| MCCP (the CCP modules, SCCP1 included) | - | **200 MHz** | CLKGEN13 = PLL1 out = **320 MHz - above the maximum** |
| CLKGEN input | - | 800 MHz | |

Consequences:

- **SCCP1 has to run at 200 MHz or less.** Microchip's MC106 example does exactly
  that: its SCCP1 trigger clock is PLL1 out divided by 2 = 160 MHz ("5 MHz trigger
  rate (160 MHz clock, PR=31)"). With CLKGEN13 = PLL1 out / 2 = 160 MHz the grid stays
  coherent with the ADC (ratio 1:2) and the rates are 160 MHz / N: 40 (N = 4), 32 (5),
  26.7 (6), 20 (8), 16 (10), 10 (16), 8 (20), 4 (40), 1 MSPS (160). Needs
  `CLK13DIV.INTDIV = 1` (ratio 2) - a CLKGEN divider, whose effect has never been
  validly measured (section C.3); the SCCP1 period check (section C.9, point 1) measures it.
- **The DAC needs 400 to 500 MHz.** The same PLL1 can deliver that from its second
  output, the VCO divider (`VCO1DIV`, today "unused", `clock.c`), as a CLKGEN7 source:
  VCO 1600 MHz / 4 = 400 MHz. Coherence does not need the same output, only the same
  VCO - ADC 320, SCCP1 160, DAC 400 MHz stand in fixed rational ratios. The exact
  `VCO1DIV` encoding is still to be read from chapter 12.
- **This may explain open question 4** (triangle period about eight times off, `DACLOW`
  not reproduced): the DAC has been clocked below its specified minimum throughout.
  Two further causes are documented (C.12.3). Every DAC capture so far was taken out of
  specification.

**C.12.2 The errata say nothing about the ADC.** DS80001162E (current revision; silicon
A2 = DEVREV 02h, A1 = 01h), 28 items in modules CPU, DMA, PWM, ITC, QEI, CCP, PTG, DAC,
GPIO, SPI, SILICON, debugger - **no ADC item, no CLKGEN, PLL or UREF item**, and "There
are no known data sheet clarifications". Relevant at the edges only:

- #2 DMA: the bus read error flag is only set with `RETEN = 1` - a `dma_bus_err`
  counter without `RETEN` can never count read errors.
- #17, #18 CCP: shutdown gate, and input capture one-shot - neither applies to SCCP1 as
  a timer with a special event trigger.
- #20 PWM: in LLC mode with `TRIGy = EOC` and `CAPTREN`, no ADC triggers - only if PWM
  becomes the trigger source.
- #21 DAC, **A1 only**: output buffers non-linear near the rails, workaround route
  over UREF. Which silicon the board carries (A1 or A2) is to be read from `DEVREV` at
  boot and printed.

**"A few transfers per trigger" is not in the errata.** The sentence in `CLAUDE.md`
comes from a support channel. It stays a hypothesis to be tested (section C.9, point 9),
not a documented property of the silicon.

**C.12.3 The DAC triangle - how fast it can move, and why our model was wrong.**
Chapter 18:

- `SLPDAT` is in 12.4 format (p1414): 12 integer bits, 4 fraction bits; the DAC value
  moves by `SLPDAT`/16 codes per DAC step.
- Equation 18-4 (p1420): `SLPDAT = (DACDAT - DACLOW) * 16 / (T_SLOPE / T_DAC)` with
  `T_DAC = 2 / F_DAC`. Solved: **one slope** lasts `(DACDAT - DACLOW) * 32 /
  (SLPDAT * F_DAC)`. The full triangle is two slopes - `dac2_period_ns()` computes one
  slope and calls it the period, a factor of 2.
- *"The very first clock cycle of the slope process selects a scaled SLPxDAT value"*
  (p1421) - the shape near each turning point deviates from the straight line.
- Example 18-3, footnote (p1422): *"The maximum value of DACDAT must be set at 0xF32 -
  SLPxDAT, and the minimum value of DACLOW must be set at 0xCD + SLPxDAT."* Our tests
  use `DACLOW = 0x100`, `DACDAT = 0xF00`, `SLPDAT` up to 64 or more: 0x100 is below
  0xCD + SLPDAT once `SLPDAT` > 51 - a documented reason why the lower end was not
  reproduced.
- Electrical (Table 40-42/43, p2035-2036): 12 bits, **DNL ±5 LSB**, INL -15..25 LSB,
  settling 600 ns (5-95 %), output range 0.165 to 3.135 V, `CLOAD` max 30 pF, drive
  ±15 mA. The 30 pF matters for RA8 with the touch-pad network behind it.

So a correct triangle model needs: the right clock (400 to 500 MHz), both slopes,
limits inside `0xCD + SLPDAT` and `0xF32 - SLPDAT`, and the turning points excluded or
modelled.

**C.12.4 How sensitive the triangle test is.** At the steepest monotonic setting,
`SLPDAT = 16` (one code per DAC step) at `F_DAC` = 500 MHz: one code per 4 ns =
250 codes/µs. Per ADC sample (400 MHz DAC gives 0.8 of these values):

| Rate | 1 MSPS | 4 | 10 | 20 | 40 |
|---|---|---|---|---|---|
| Step per sample, 500 MHz DAC | 250 LSB | 62.5 | 25 | 12.5 | 6.25 |

Against that: ADC ENOB 10.5 bits (AD34), i.e. about 0.8 LSB rms noise, and DAC DNL of
±5 LSB, which makes individual steps uneven by several LSB. A single lost sample
doubles one step; at 20 MSPS that is 12.5 → 25 LSB, clearly visible; at 40 MSPS
6.25 → 12.5 LSB against ±5 LSB DNL - marginal if judged step by step.

**The robust criterion is the turning points, not the steps.** With DAC and ADC on one
VCO, the number of samples per triangle slope is fixed and known exactly. A lost sample
makes one slope one sample short, a repeated one makes it one sample long, a gap shifts
all later turning points. The turning point is located by fitting a line to each slope
(excluding the first samples after the turn, see C.12.3) and intersecting them - to a
fraction of a sample, independent of DNL, at every rate. The step check stays as the
second, local instrument where the step is large enough (up to about 20 MSPS).

**C.12.5 The analog side of sampling.** `T_SAMPLING = 9 x R_TOTAL x C_HOLD` for 12 bits
(p1320), `R_TOTAL = R_S + R_IC`, `C_HOLD` = 1 pF, `R_IC` = 120 Ω, `C_PIN` = 4 pF
(Table 40-39, p2034). `SAMC` runs from 0.5 TAD (6.25 ns at 80 MHz) in 2 TAD steps. Even
with a few hundred ohms of source resistance the required time is a few ns, so the
shortest `SAMC` is enough electrically; the touch-pad network on RA8 is the unknown and
has to be characterised at low rate (section C.11).

**C.12.6 The 1.5 or 2 TAD question now matters.** At 40 MSPS the trigger period is 25 ns
= 2 TAD. Section 16.4.3 says a conversion takes 2 TAD, Table 40-39 note 4 says the
throughput *includes* 1.5 TAD of conversion. With `SAMC` = 0.5 TAD, sampling plus
conversion is 2.0 TAD (fits exactly) or 2.5 TAD (does not fit - 32 MSPS would be the
ceiling). Microchip's 40 MSPS figure comes from back-to-back; their triggered example
runs each channel at 5 MSPS. **Whether a single channel can be triggered at 40 MSPS is
undocumented** - the ladder has to include 32 and 40 MSPS to find out.

**C.12.7 Register facts for core 5** (ATDF, pack 1.3.185):

| Item | Value |
|---|---|
| `TRG1SRC` SCCP1 | `0x20` "SCCP1 OC/IC Event" - one value group `AD_CH_CON1__TRG1SRC` shared by all five cores, so it holds for core 5 |
| DMA `CHSEL` ADC5 | `0x48` "ADC5 Done CH0" (core 3 was `0x3B`) |
| DMA `CHSEL` SCCP1 | `0x18` "SCCP1" - a second DMA channel can count SCCP1 events in hardware (section C.10, point 7) |
| `AD5CH0RES` / `AD5CH0DATA` | `0xDA4` / `0xDA0` |
| `AD5CON` / `AD5STAT` / `AD5RSTAT` / `AD5SWTRG` | `0xD80` / `0xD88` / `0xD8C` / `0xD94` |
| `AD5CON` reset | `0x480000`: `ACALEN`, `CALREQ`, `CALRATE`, `CALCNT` all 0 - no periodic calibration unless someone sets it |
| RA8 | `ANSELA` bit 8 and `TRISA` bit 8, both reset to 1 (analog input) |
| `CCP1CON2.AUXOUT` | 0 disabled, 1 timer rollover, **2 special event trigger**, 3 OC signal |
| IRQ numbers | CCP1 52, DMA0 77, U2RX 102, AD5CH0 241 |

**C.12.8 Interrupt priorities - proposal.** Today DMA0 runs at 4 (IPC9 default), U2RX at
1, console commands execute inside the U2RX interrupt, and the processing in the main
loop at 0. In the target mode the DMA0 interrupt only carries HALF and DONE, so there is
no storm to starve anything. Proposal: keep DMA0 highest (it is short and its latency
decides whether a half is caught), U2RX below it; during an acceptance stream the
console stays silent, because a command at priority 1 preempts the processing at 0.
The CCP1 and AD5CH0 interrupts are enabled only in the low-rate counting scenarios -
at a high rate they would fire at the sample rate.

**C.12.9 The rate ladder - proposal.** Low rates for the per-event counting of section C.9:
1 kHz, 10 kHz, 100 kHz. Then the SCCP1 rates at 160 MHz / N: 1, 4, 8, 10, 16, 20, 26.7,
32, 40 MSPS. Expected by the documents: pass up to 20 MSPS (another customer's stable
figure), open between 26.7 and 32 MSPS (the DMA ceiling of about 33 M transfers/s
from support), open at 40 MSPS (C.12.6). The test reports where it passes; the example
is then built on the highest rate that passes with margin.

**C.12.10 Pass/fail criteria - proposal, per stream:** triggers = conversions =
transfers, exactly (counted by the CPU at low rate, by a second DMA channel at high
rate); `overrun`, `late`, `missed` = 0; every triangle slope has the predicted number of
samples (C.12.4); where the step is at least 10 LSB, every step within the predicted
value ± (DNL + 3σ noise); guard words intact; `ACALEN` = 0 and one channel enabled in
the register dump.

## D. The measurement errors we made, and how they were found

This section exists because most of the wrong answers in this project came from the
measurement rather than from the device. Anyone reading the older entries in the
HARDWARE-LOG should know which numbers there have since been withdrawn.

**Rate measured under load - wrong by a factor of ten.** Runs 4 to 11 measured the
delivered rate while the CPU was drowning in the overrun interrupt. Every rate figure
from those runs is void. Found by measuring the same setting on a single clean burst
and getting 3990 kSPS where the loaded figure said 40652.

**The one-shot window included its own setup.** The clock was started before
`capture_settle()`, so taking the DMA channel down and building it up again - a fixed
11.3 µs - sat inside the measured window. At 4 MSPS that is 2 % of the burst, at
40 MSPS 18 %, which looked exactly like a rate-dependent error. Found by converting
each row to a window duration and noticing the offset was the same everywhere.

**`dma_overrun` is a lower bound, not a count.** `OVERRUN` is one bit in `DMA0STAT`
and the handler increments once per *entry in which it is found set*. Several losses
between two entries count as one. Every "about 4 % of the samples are lost" in the
older entries means "the bit was seen set in as many entries as 4 % of the sample
count". Found while working out a prediction; confirmed from the data side, where the
overrun counts *fall* as the rate rises - absurd as a loss fraction, coherent as a
count of handler entries.

**A comparison that proved nothing, printed as if it did.** The sweep printed
`blocks (must be 2x bursts)` - but `blocks_done` is free-running by design
(`seen_blocks` uses it) while every counter beside it is per measurement point. A
total was being compared with a sample, which looked like a factor of three and then
of forty. Separately, `half_events + done_events == blocks_done` is true by
construction and can never disagree, so it could not have decided anything either.

**The DAC test passed on a window that stood still.** The reversal detector's
hysteresis was a fixed 96 counts and the window moved 72, so it never armed and
reported zero reversals - which read as "in order". The window had also moved about
a tenth of what the DAC settings predicted, and nothing checked that. Found by
computing what the triangle should have done and comparing.

**Timer1 was not running unless the sweep ran.** It was started from
`timebase_check()`, which only the sweep calls, so a run that only did `test dac`
measured its window as zero ticks.

**The DAC test ran on the wrong ADC core.** DACOUT2 is an input of core 5; the test
ran on core 3 and measured an open pin. Found by reading the pinout table.

**The half length was not in the output.** Every duration and rate derived from a
sweep row is (halves × samples per half) / rate, and the evaluation silently assumed
1024 - which `buf` changes at run time. An evaluation against the wrong length would
have been wrong without looking wrong.

**Three errors in the SCCP trigger path, all ours.** The trigger code was changed
from 32 to 34 on the strength of datasheet Table 16-4; the device pack's ATDF names
`0x20` = 32 as "SCCP1 OC/IC Event" and `0x22` = 34 as **SCCP3**, so the ADC was told
to listen to a module nothing configured. The auxiliary output carried the timer
rollover (`AUXOUT = 1`) instead of the special event trigger (`AUXOUT = 2`). And the
module was clocked from the peripheral clock (PLL2) while the ADC runs off PLL1,
which a support case warns against explicitly. Each one alone was enough for "no
conversion at all", and that is what runs 5 to 7 reported - so **the SCCP path has
never actually been tested.**

**The test suite did not test the example's claim.** The variant matrix asked whether
each variant converts, whether its rate follows, and whether the data are intact - and
never whether a stream *lasts* with the CPU keeping up, which is the sentence the
example exists for. Fixed in `2c8137f`.

## E. What holds, what is likely, what is open

**Holds, on silicon:**

- ADC → DMA → ping-pong buffer carries every sample, in the order it was converted.
- The console, the clock tree, the self-test, the trap handler, the guard words.
- A single burst runs at the rate the PLL is set to, to better than 0.5 % from 4 to
  40 MSPS.
- Clock-register writes arrive and are confirmed.

**Corrected in the firmware on 25.09.2026, effect not yet seen on silicon:**

- The DAC was clocked at 320 MHz, below its 400 MHz minimum, and SCCP1 at 320 MHz, above
  its 200 MHz maximum (C.12.1). Now 400 MHz (PLL1 VCO divider) and 160 MHz (CLKGEN13 / 2).
- The DAC's data registers were written without an update trigger: `DACxCON.UPDTRG`
  resets to 00, "the user must set DACxCON.UPDREQ bit manually" (p1409), and nobody did.
  Now `UPDTRG = 11`. Together with the clock and the factor 2 in `dac2_period_ns()` these
  are three candidate causes of open question 4.
- `sccp1_start()` wrote `CCP1RB` in output-compare mode only; a timer-mode start after an
  OC run with a longer period kept a compare point beyond the new period.

**Likely but not proven:**

- The DMA's ceiling is about 33 M transfers/s. This is a Microchip support statement,
  with 20 MSPS confirmed stable by another customer on the same device, not our own
  measurement.
- The overruns seen at 4 MSPS are not a bandwidth limit - the arithmetic does not
  allow it - but what they are instead is not established.

**Open:**

- Whether the rate is selectable **in continuous streaming**, which is the example's
  central claim.
- Whether the converter speeds up under streaming or each conversion lands in the
  buffer more than once.
- Whether the CLKGEN6 divider sets the rate, and whether the ADC really converts with
  CLKGEN6 switched off - both only ever observed with withdrawn instruments; every
  document says CLKGEN6 is the ADC's clock (section C.3).
- Whether any variant can stream without loss while the CPU processes. **No variant
  has ever done so in any run.**
- The whole triggered family - SCCP as first trigger, as second trigger, and PWM -
  is untested, because the three errors above meant it was never really tried. Since
  back-to-back is ruled out as a continuous path (section C.7), **this family is the only
  candidate left for the example's sentence.**

## F. What the next run settles

`chain all` on master after 25.09.2026 - the chain test of `docs/CHAIN-TEST-PLAN.md`,
which implements sections C.8 to C.12 as one command of under a minute. The old
`test all` (back-to-back, above) stays in the firmware but is no longer the run to make.

| Stage | Question it answers | Sections |
|---|---|---|
| S0 | Timer1 calibration; every clock of the chain measured by the clock monitor (PLL1 out, VCO divider, CLKGEN6, CLKGEN7, PLL2 VCO divider); core 5 without calibration and without other channels; RA8 analog | C.9 8, C.10 1 and 5, C.12.1 |
| S1 | SCCP1's clock against Timer1 (160 MHz?), its period by interrupts at 1, 10, 100 kHz, timer and OC mode | C.9 1 |
| S2 | SCCP1 -> ADC: events = results at 1, 10, 100 kHz, counted by the CPU on both sides; one or two events per period; the DAC's static transfer over RA8, SAMC 0 against 31, UREF as comparison; the CPU-stepped DAC read back sample by sample | C.9 2, 3; C.10 3, 8; C.11 |
| S3 | ADC -> DMA -> buffer at 100 kHz, three blocks: every buffer index holds exactly the code the CPU stepped for it, across the block restart by the DMA alone, guard words intact | C.9 4-7, 13 |
| S4 | every rate 100 kSPS .. 40 MSPS: transfers against triggers (from Timer1, same FRC), overrun, and the IRQSEL = 1 / CH0DATA variant where the default fails | C.9 9, 10, 12; C.10 2; C.12.6 |
| S5 | every rate: the triangle in one contiguous window, turning points by line fits; a lost or repeated sample shifts all later turning points by one ("slip"); slope length against the model; DAC on the second VCO as a cross-check | C.9 11; C.11; C.12.3, C.12.4 |
| S6 | every rate that passed S4: one second of stream with the CPU processing every half; overrun, late, missed, transfers against triggers, processing load and free cycles per sample | C.9 12-15; C.10 9, 10 |
| S7 | first sample at buf[0], nothing written after stop, restart, rate change | C.9 16, 17 |
| S8 | CLKGEN6 divider and CLKGEN6 off, measured at the generator; back-to-back: 1 burst against the 100th of a stream | C.2, C.3 |
| S9 | the chain as the example runs it, 15 s at the best rate and at 8 MSPS; registers dumped while it runs | A |

The summary ends with `USE THIS RATE: <n> kSPS` or with the plain statement that no rate
streams cleanly with the CPU processing.
