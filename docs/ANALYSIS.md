# Where this example stands, what was measured, and how much of it holds

Status of 24.09.2026, master `e2bd11a`. Written to be read without the chat history
behind it, and without reading `docs/HARDWARE-LOG.md` end to end - that file is a
diary, this one is sorted by question.

## What the example is meant to demonstrate

One sentence:

> **At a sample rate you choose, the ADC streams samples through the DMA into RAM
> continuously, and the CPU processes them on the free half of the buffer.**

Everything below is measured against that sentence. It matters that it is one
sentence with four parts - *chosen rate*, *continuously*, *through the DMA*, *CPU
processes* - because the parts were proven separately and they are not equally far
along.

## How things were measured, and what each instrument can say

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

## What was checked, and what came out

### 1. Does the chain carry samples at all, complete and in order?

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

### 2. Is the sample rate selectable?

Four mechanisms were tried. The table is the short version; the detail is below it.

| Mechanism | Result |
|---|---|
| Sample time `SAMC` | No effect on the rate (run 4) |
| ADC repeat timer, `TRG2SRC = 3`, `RPTCNT` | Register holds the value, rate unchanged (runs 5, 6, 7) |
| CLKGEN6 divider `CLK6DIV` | Register holds the value, `DIVSWEN` and `CLKRDY` confirm, rate unchanged (runs 8, 9, 10) |
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

### 3. Is CLKGEN6 the ADC's clock?

**Checked with:** `test clkoff` - the ADC core is taken down, CLKGEN6 is switched off
entirely, the core is brought back and asked to convert.

**Result: the ADC keeps converting with the generator switched off.** 260 halves in
run 10, 390 in run 14. Datasheet Table 16-1 names CLKGEN6 as the ADC clock source.

**Uncertainty:** what the samples *are* in that state was not checked - only that
halves keep arriving. Combined with the previous section, "halves keep arriving" is
no longer strong evidence that conversions keep happening.

This is a question for the product line rather than for this example.

### 4. Does the CPU get to do its work?

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

### 5. Does the console work?

**Checked with:** typing at it from an idle boot, after the firmware was changed to
run nothing on its own.

**Result: yes.** `rx = 0` through runs 1 to 7 was the receive interrupt starving
behind the DMA interrupt, not wiring, not the terminal, not the PPS configuration.
Idle at boot, everything answers.

### 6. Do the clock switches arrive in the hardware?

**Checked with:** `test clock` - every divide ratio written and read back, with
`DIVSWEN` and `CLKRDY` awaited and checked.

**Result: yes, every one.** And it is worth being precise about what that proves:
**the register holds the value.** It says nothing about the clock that comes out,
which is exactly the trap run 8 fell into. The line is printed under the test's own
PASS so that nobody reads it as more than it is.

## The measurement errors we made, and how they were found

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

## What holds, what is likely, what is open

**Holds, on silicon:**

- ADC → DMA → ping-pong buffer carries every sample, in the order it was converted.
- The console, the clock tree, the self-test, the trap handler, the guard words.
- A single burst runs at the rate the PLL is set to, to better than 0.5 % from 4 to
  40 MSPS.
- Clock-register writes arrive and are confirmed.

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
- Why the ADC keeps converting with CLKGEN6 switched off.
- Whether any variant can stream without loss while the CPU processes. **No variant
  has ever done so in any run.**
- The whole triggered family - SCCP as first trigger, as second trigger, and PWM -
  is untested, because the three errors above meant it was never really tried.

## What the next run settles

`test all` on master `e2bd11a` or later. It is built to leave nothing standing,
because a board run costs a person.

| Step | Question it answers |
|---|---|
| `self` | is the chain wired up |
| `clock` | do divider writes arrive (and the reminder that this proves only that) |
| `clkoff` | does the ADC run without its clock generator |
| `bursts` | **does the rate depend on how many bursts run** - 1, 10 and 100 at the same setting, the bridge between "one burst follows the setting" and "a thousand do not" |
| `sweep` | the rate ladder with the counters per row |
| `matrix` | every variant: converts, rate follows, transfers per trigger |
| `matrix` acceptance | **does any variant stream for 2000 halves at 4 and at 8 MSPS with overrun, late and missed all zero** |
| `matrix` data | the triangle as an isolated burst *and* as the 100th of a stream - period in samples against rate decides whether streamed samples are repeats |

The summary ends either with the name of the variant the example can be built on, or
with the plain statement that none of them streams cleanly - in which case the example
does not yet demonstrate what it says it does, and that belongs on the console rather
than in a footnote.
