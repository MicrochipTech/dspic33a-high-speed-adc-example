# dsPIC33AK512MPS512 — ADC at 40 MSPS into RAM via DMA

A small, complete MPLAB X project showing **how to configure the device** so the
ADC runs at its maximum rate and the DMA moves the samples into RAM — and **how to
measure whether it really keeps up**.

Bare metal, no MCC. Every register write in the source cites the datasheet table or
page it comes from, so nothing has to be taken on trust.

## Read this first

**This code has never run on hardware.** It compiles and links cleanly with the real
compiler, and every bit set in it was read out of the datasheet and the device pack —
but nobody has executed it on a board or measured a signal with it. Treat it as a
clean scaffold with measurement points, not as a reference implementation.

Parts of this example were **AI-assisted**. All register names, bitfields and value
ranges were taken from datasheet **DS70005591D** and from the ATDF files of the
**dsPIC33AK-MP_DFP** device pack, and each one is cited at the point of use — so every
setting can be checked against the primary source.

## Getting started

Open `adc_dma_40msps.X` in MPLAB X, press **Build**, then **Debug** or **Program**.
That is all — the project is configured for dsPIC33AK512MPS512 and pulls the source
from the folder above it.

Verified on 2026-09-21 with:

| Tool | Version |
|---|---|
| MPLAB X IDE | v6.35 |
| XC-DSC compiler | v3.31.00 (also builds with v3.21 when the pack supplies the device) |
| Device pack | dsPIC33AK-MP_DFP 1.4.260 |
| Target | dsPIC33AK512MPS512 |
| Board | EV17P63A (dsPIC33AK512MPS506 Curiosity Nano) — 64 pins, fewer analog inputs than the 128-pin part, enough for a functional check |

There is exactly **one** source file, `adc_dma_40msps.c`, in the repository root. The
MPLAB X project references it; nothing is duplicated.

**This needs real hardware — the simulator will not run it.** MPLAB X's simulator
model for dsPIC33A covers PPS, ports, pull-ups, TMR1/TMR2, UART1-3, the watchdog and
context switching. It does *not* model the clock generators, the PLLs, the ADC or the
DMA, which are the four things this example is about. In the simulator the code
therefore stops at the first wait loop: `PLL1CONbits.OSWEN` gets written but nothing
ever clears it, because there is no PLL to switch. `AD1CONbits.ADRDY` would behave the
same way.

Skipping those loops under conditional compilation would not help: the ADC would not
convert, the DMA would not transfer and the ISR would never fire, so the run would
show that the code starts — not that the configuration works. The number that matters,
`dma_overrun` staying at 0 at full rate, only exists on silicon.

One part *is* worth simulating: `process_buffer()`. Write test values into `buf_a`,
call it on its own, and you can check your arithmetic and its cycle count without a
board.

The `tools/` folder builds the same file from the command line without the IDE. **You
can ignore it** — we use it to check that the code compiles against different
compiler and pack versions.

## First run on hardware

Since this has never run on silicon, here is what to expect and where it is most
likely to trip you up.

### Step 1 — check that anything runs at all, no signal needed

Program the board, let it run, then halt it and look at four variables:

| Variable | Should be |
|---|---|
| `blocks_done` | increasing — at 40 MSPS a block completes every 25.6 µs, so this climbs fast |
| `last_sample` | changing |
| `dma_overrun` | **0** |
| `late_service` | **0** |

An unconnected pin gives you noise around some level. As a sign of life that is
perfectly sufficient — it proves clock, ADC, DMA and the ISR are working together.

### Step 2 — feed a signal in

**Which pin.** The code samples `AD1AN0` (`PINSEL = 0`). Look up which physical pin
that is on your package and check it is free on your board — if something is already
connected there, you will be looking at that instead of your signal. The input map is
Table 16-2 of DS70005591D, from page 1224.

**What level.** 0 to 3.3 V, single ended against AVSS, unipolar (`DIFF = 0`). Anything
with a negative excursion gets clipped at the bottom.

**What frequency.** This matters more than people expect. One buffer is 1024 samples,
which at 40 MSPS is **25.6 µs**. A 1 kHz sine fills 2.5 % of one period — in the buffer
that is a straight line. For a recognisable waveform use something in the **100 kHz to
a few MHz** range, then several periods fit in the window.

**Source impedance — the most likely reason for odd values.** `SAMC = 0` means a sample
time of 0.5 TAD = 6.25 ns, and in that time your source has to charge the hold
capacitor. A 50 Ω function generator manages; a high-impedance divider or a long cable
does not, and you get values that are too small or smeared. If the picture looks wrong,
**`ADC1_SAMC` at the top of the source is the first knob to turn** — raise it and see
whether the amplitude comes up.

### Step 3 — look at the buffer

The buffers refill 39 000 times per second, so you have to stop the capture to see
anything: set a breakpoint in the DMA0 ISR, then view `buf_a` / `buf_b` in the watch
window or as a memory view.

Which one to look at: `active_buf` points at the buffer currently **being filled**, so
the *other* one holds the complete block.

### If it does not work

| Symptom | Where to look first |
|---|---|
| stuck before `main()`, or nothing counts up | clock configuration — a wait loop in `clock_init()` never exits |
| `blocks_done` stays 0 | ADC not converting (`ADRDY`?) or wrong DMA trigger (`DMA0SEL`) |
| `dma_overrun` counting up | the shared DMA bus is not keeping up — see below, this is the interesting result |
| values far too small or flat | source impedance, raise `ADC1_SAMC` |
| values look like a straight line | signal frequency too low for a 25.6 µs window |
| `late_service` counting up | the ISR is not keeping up, reduce the processing or enlarge `SAMPLES_PER_BUF` |

Note that `dma_overrun` counting up is not a bug in this code — it is the measurement
this example exists for.

## How it works

### 1. Clock tree

![Clock tree](docs/01_clock_tree.png)

The part that differs from many other devices: **the fast peripherals do not hang off
the system clock.** There are two dedicated PLLs and fourteen clock generators, so
320 MHz at the ADC alongside a 200 MHz CPU is no contradiction.

Which generator feeds what is not stated in one place:

- **CLKGEN1 is the system clock** — §12.4.9, page 795: *"Clock Generator 1 is the
  clock source for the system clock (sys_clk) and peripheral clock."*
- **CLKGEN6 is the ADC clock** — Table 16-1, page 1223, column "Clock Source",
  together with "Max Input Clock 32 MHz to 320 MHz".

TAD derives from the ADC input clock: **TAD = 4 / F_IN** (parameter AD50, Table 40-39,
page 2034). At 320 MHz that is 12.5 ns, which is also the minimum — more than 320 MHz
is not specified (Table 40-24, page 2016). Hence the 40 MSPS (AD51, throughput
including 1.5 TAD conversion time).

A useful cross-check: the datasheet measures its own current consumption at exactly
this operating point — *"Input frequency 320 MHz, ADC clock 80 MHz, TAD 12.5 ns"*
(DC120/DC121, page 2008). So this is the intended setting, not brinkmanship.

`CLKxDIV` has a 9-bit fractional divider `FRACDIV` next to the integer `INTDIV`, which
is why the factor 1.6 for 200 MHz is expressible at all: `INTDIV = 1`,
`FRACDIV = 0.6 × 512 = 307`.

### 2. ADC — one core, one channel, free running

![ADC path](docs/02_adc_path.png)

| Field | Value | Why |
|---|---|---|
| `PINSEL` | 0 | analog input AD1AN0 (Table 16-2, from page 1224) |
| `NINSEL` | 0 | negative input on AVSS, i.e. single ended |
| `DIFF` | 0 | single ended → unsigned result |
| `FRAC` | 0 | integer, right aligned |
| `SAMC` | 0 | sample time 0.5 TAD = minimum (page 1266) |
| `MODE` | 0 | single conversion, no averaging |
| `ACCNUM` | 0 | no oversampling |
| `TRG2SRC` | 0x02 | **immediate re-trigger** (Table 16-4, page 1227) |

**On the trigger choice**, since that was the original question: for maximum
continuous rate you need neither PWM nor SCCP nor PTG. The ADC triggers itself. Two
variants appear in Table 16-4:

- `0b000010` **immediate re-trigger** — gapless, no period arithmetic. This is what
  the code uses.
- `0b000011` **conversion repeat timer** — fixed rate, period via `RPTCNT[5:0]` in
  `AD1CON`, counting 1 to 64 ADC clock cycles between triggers (page 1258).

One caveat for the second variant and the alternative rate of 25 MSPS: at an 80 MHz
ADC clock the achievable rates sit on 80/n MHz, i.e. 40 / 26.67 / 20 / 16 MSPS.
**Exactly 25 MSPS is not among them.** If it has to be 25, go via the input clock:
200 MHz in → TAD 20 ns → 50 MHz ADC clock → /2 = 25 MSPS.

PWM or SCCP are only needed if sampling must be tied to a switching event, PTG only
for staggered sequences across several cores.

### 3. DMA with ping-pong buffers

![DMA path](docs/03_dma_path.png)

| Field | Value | Why |
|---|---|---|
| `DMA0SEL` | 0x2F | trigger source "ADC1 Done CH0" (ATDF value group `DMA_SEL__CHSEL`) |
| `DMA0SRC` | `&AD1CH0DATA` | the channel's result register |
| `SIZE` | 1 | **16-bit transfers** |
| `SAMODE` | 0 | source address stays put |
| `DAMODE` | 1 | destination increments |
| `TRMODE` | 3 | repeated continuous |
| `RELOADD`, `RELOADC` | 1 | reload destination and count per block |
| `DONEEN` | 1 | interrupt on block completion |

**On 2 bytes per sample:** the DMA handles 8, 16 and 32-bit transactions, selected
through `SIZE[1:0]` (§13.4.2, page 824). A 12-bit result therefore costs 2 bytes, not
4. The result registers themselves are 32 bits wide; with `FRAC = 0` the value is
right aligned, so the lower half carries the data. **That is the one assumption in
this code the datasheet does not state outright** — please verify it on hardware.

Two buffers of 1024 samples, 2 KiB each. At 40 MSPS one buffer is 25.6 µs of signal
and the block interrupt arrives at about 39 kHz. The ISR only swaps the destination
address and counts errors — deliberately short, because at this rate a long ISR
becomes the cause of the next overrun.

### 4. What the CPU does, and what to measure

![CPU and counters](docs/04_cpu_and_counters.png)

## The point of the whole thing

One sentence in the datasheet matters more for this project than any ADC register,
§13.4.4 on page 825:

> *"While DMA channels can function independently to service different peripherals at
> the same time, they are still limited by the presence of a single DMA data bus and a
> single data channel to data space."*

**So the eight DMA channels are not eight parallel data paths.** They share one bus,
and when they contend an arbitration decides (fixed or round robin via
`DMACON.PRIORITY`). For scale: three channels at 40 MSPS and 2 bytes are 240 MB/s
across that one bus, five channels 400 MB/s. **How much it actually carries is not in
the datasheet** — there is no figure in transfers per second, and the DMA does not
appear in the peripheral clock table either.

That is why this code has counters instead of claims.

## What to measure

Read these in the debugger after a run:

| Variable | Meaning | Expectation |
|---|---|---|
| `blocks_done` | completed DMA blocks | × 1024 / elapsed time = **actual sample rate** |
| `dma_overrun` | `DMA0STAT.OVERRUN` seen | **must stay 0**, otherwise samples were lost |
| `late_service` | ISR was too slow | **must stay 0** |
| `dma_bus_err` | bus read/write error | 0 |
| `dma_addr_err` | address error | 0 |
| `last_sample` | last value read | changing = data really moving |

A sequence we would suggest:

1. **One channel, 40 MSPS.** Does `dma_overrun` stay at 0 over a longer run? That
   proves the basic configuration.
2. **Two channels.** Set up ADC2 the same way, second DMA channel
   (`DMA1SEL = 0x35`, "ADC2 Done CH0"). This is where the shared bus first shows its
   limit.
3. **Three channels.** If overruns appear here, the limit is found — with a number the
   datasheet cannot give you.
4. **Only then add the processing.** `process_buffer()` is deliberately written as a
   placeholder loop over every sample, so the cost of touching each value is visible.

## If the bandwidth is not enough

The ADC can average internally, before a DMA transfer even happens — `ACCNUM[1:0]` in
`AD1CH0CON1` (page 1266):

| `ACCNUM` | Samples | Result width |
|---|---|---|
| 0b00 | 4 | 13 bit |
| 0b01 | 16 | 14 bit |
| 0b10 | 64 | 15 bit |
| 0b11 | 256 | 16 bit |

At 16× averaging, 240 MB/s becomes 15 MB/s and the result still fits in 2 bytes with
14 bits. Sampling stays at 40 MSPS; only the output rate drops.

**Whether that is an option depends on your measurement method** — for a pure
amplitude measurement it helps, for a phase-based method averaging can destroy the
information. That is worth a phone call.

`MODE[1:0]` also offers three further sampling modes (integration, window/gated,
single conversion), and the last three setting channels have a second accumulator for
second-order filters.

## What this code does not do

- **No PWM and no clock output.** For an external DAC above 120 MHz a controller pin
  is not the way: the output pins are specified with 2.3 ns rise and 1.7 ns fall time
  (Table 40-26, page 2017, at 25 pF). At 120 MHz one period is 8.3 ns — four of those
  nanoseconds would be edges. The datasheet states no maximum PWM output frequency,
  and none for the Reference Clock Output either.
- **No multiple channels.** On purpose: one should be provably working first.
- **No interrupt prioritisation, no error recovery, no calibration.** The ADC can
  recalibrate itself periodically (`ACALEN` and `CALRATE` in `AD1CON`) — worth a look
  for a longer measurement.
- **No statement on analog input bandwidth.** The datasheet does not give one, and the
  input parameters it does give (hold capacitance, pin capacitance, interconnect
  resistance) all carry the note "design guidance only, not tested". The ENOB of
  10.5 bits was characterised with a 1 kHz sine and says nothing about high input
  frequencies.

## One trap worth knowing about

The symbolic values for `FICD_NOBTSWP` were **renamed between pack versions**:

| Pack | Accepted values |
|---|---|
| dsPIC33AK-MP_DFP 1.3.185 | `ON` / `OFF` |
| dsPIC33AK-MP_DFP 1.4.260 | `BTSWP_ENABLED` / `BTSWP_DISABLED` |

Both name the same bit (FICD mask 0x8000, value 0x0 = BOOTSWP enabled). If MCC
generates `config_bits.c` against one pack and the build uses another, the compiler
rejects a value that is perfectly valid elsewhere:

```
error: unknown value for configuration setting 'FICD_NOBTSWP': 'BTSWP_ENABLED'
```

The fix is not to guess the value but to align the versions. This project carries a
`PACK_14_OR_NEWER` switch at the top of the source so it builds with either pack.

## Files

| Path | Contents |
|---|---|
| `adc_dma_40msps.c` | the entire code, commented with datasheet references |
| `adc_dma_40msps.X/` | MPLAB X project — build, program and debug from here |
| `docs/*.png`, `docs/*.mmd` | the block diagrams above, with their Mermaid sources |
| `tools/` | command-line build without the IDE; **ignore this unless you want it** |

### About `tools/`

Only needed to build without MPLAB X:

```
cd tools
python setup.py          # find compiler and pack, configure build.bat and Makefile
build.bat                # build
```

`setup.py` scans for installed XC-DSC compilers and dsPIC33AK-MP packs, in both
places they can live (`%USERPROFILE%\.mchp_packs` and MPLAB X's own `packs` folder),
lets you choose, and writes the paths into `build.bat` and `Makefile`. With
`--verify` it also runs a real test build. `--list` just shows what it found.

Two things that cost us time there, in case you build without the IDE:

1. `-mdfp` must point at the **`xc16` subdirectory** of the pack, not the pack root —
   otherwise the compiler reports "does not seem to support the selected device"
   although the pack does contain it. `c30_device.info` lives one level down.
2. The **linker script must be given explicitly** with `-T`
   (`support/dsPIC33A/gld/p33AK512MPS512.gld` inside the pack). Without it the
   compiler links against a 30F architecture and stops with "incompatible with
   30Fxxxx output".
