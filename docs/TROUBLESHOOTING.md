# Troubleshooting guide

This code has **never run on hardware**. It compiles cleanly and every register value
was read out of DS70005591D and the device pack ATDF — but "compiles" and "works" are
different things, and the first person to run it will find whatever is wrong.

This guide is written for that. It is ordered by how likely each thing is to be the
problem, and it tells you **where we are least certain of our own code**, so you do not
waste time on the parts that are solid.

---

## Part 1 — Where this code is most likely wrong

Read this before debugging anything. These are our own doubts, most suspect first.

### 1.1 Is the sample actually in the lower 16 bits? (highest risk)

`AD1CH0DATA` is a **32-bit** register. The DMA is set to 16-bit transfers
(`SIZE = 1`), so it copies half of it. We assume the 12-bit result sits in the lower
half because `FRAC = 0` selects right-aligned integer format — but **the datasheet
never states this outright**, and it is the one assumption that would silently produce
garbage rather than an error.

**How to check:** halt after a few blocks and compare. Read `AD1CH0DATA` directly in
the watch window, then look at the last value written into the buffer. They should show
the same number.

**If they differ:** the sample is elsewhere in the 32-bit word. Then either
- set `DMA0CHbits.SIZE = 2` (32-bit) and make the buffers `uint32_t`, at the cost of
  4 bytes per sample, or
- point `DMA0SRC` at the odd 16-bit half: `(uint32_t)&AD1CH0DATA + 2`.

Try `FRAC = 1` as a cross-check: that switches to fractional (left-aligned) format. If
the numbers change in a way that makes sense, the alignment theory is confirmed.

### 1.2 The PLL setup

This used to be our biggest worry. It is no longer, because we found Microchip's own
MCC-generated example for this exact part and aligned the code with it:

**https://github.com/microchip-pic-avr-examples/dspic33ak-curiosity-adc-40msps**

That example runs at 40 MSPS on a Curiosity board and covers both dsPIC33AK128MC106
and dsPIC33AK512MPS512. It has no DMA — which is why this project exists — but its
clock setup has been on hardware, and ours now uses the same divider values and, more
importantly, the same switching sequence.

**The switching sequence is the part that bites.** DS70005591D page 778:

> a) Enable PLL Input and Feedback Divider update by setting the **PLLSWEN** bit […]
> c) Enable the PLL Output Divider update by setting the **FOUTSWEN** bit […]
> d) Select the clock source by setting the **NOSC[3:0]** bits […]
> e) Enable clock switching by setting the **OSWEN** bit […]

An earlier version of this code set only NOSC and OSWEN. That does not fail loudly —
it leaves the previous divider values in place, so the part comes up at the wrong
frequency and every rate derived from it is wrong. If you see a plausible-looking
sample rate that is off by some factor, this class of mistake is why.

Current values, cross-checked against Table 40-23 and page 777:

| | PLL1 (ADC) | PLL2 (system) |
|---|---|---|
| `PLLxDIV` | `0x0100C829` | `0x01007D29` |
| N1 (`PLLPRE`) | 1 → F_PFD 8 MHz ✔ ≥ 5 MHz | 1 → 8 MHz ✔ |
| M (`PLLFBDIV`) | 200 ✔ in 16…320 | 125 ✔ |
| F_VCO | 1600 MHz ✔ in 500…1600 | 1000 MHz ✔ |
| `POSTDIV1` / `POSTDIV2` | 5 / 1 ✔ POSTDIV1 ≥ POSTDIV2 | 5 / 1 ✔ |
| Output | **320 MHz** → CLKGEN6 → ADC | **200 MHz** → CLKGEN1 → CPU |

Using two PLLs means neither clock needs a fractional divider — `CLK1DIV` and
`CLK6DIV` are both 0. An earlier version divided one 320 MHz PLL by 1.6 using the
9-bit `FRACDIV` field, which worked arithmetically but rested on our reading of how
that field scales. That uncertainty is gone.

The code also parks the system clock on the FRC before touching the PLLs, because
changing PLL settings underneath a running CPU clock can overclock the core. This
matters on a debugger restart, where the part is not freshly reset.

### 1.3 Interrupt priority is left at default

The code enables the DMA0 interrupt but never sets its priority. Whatever the reset
default is, it applies. At 39 000 interrupts per second that is usually fine because
nothing competes — but if you add UART or CAN later, this is where jitter and
`late_service` counts will come from.

### 1.4 Things we consider solid

So you do not hunt here first. Each was read from a primary source and cross-checked:

- **`DMA0SEL = 0x2F`** = "ADC1 Done CH0" — straight from the ATDF value group
  `DMA_SEL__CHSEL`.
- **`SIZE = 1` = 16-bit** — §13.4.2 page 824 states 8, 16 and 32-bit transactions
  explicitly.
- **`TRG2SRC = 0x02` = immediate re-trigger** — Table 16-4 page 1227.
- **320 MHz is the ADC maximum, TAD = 4/F_IN** — Table 40-24 page 2016 and AD50.
- **CLKGEN6 feeds the ADC, CLKGEN1 the system** — Table 16-1 page 1223, §12.4.9
  page 795.
- **`FICD_NOBTSWP` values** — read from both pack versions, see the README.

---

## Part 2 — Symptoms, in the order you will meet them

### 2.0 It does not even compile

Before anything else: **a configuration-bit error is almost always a version
mismatch, not a wrong value.**

```
error: unknown value for configuration setting 'FICD_NOBTSWP': 'BTSWP_ENABLED'
```

The symbolic names changed between pack versions (`ON`/`OFF` in 1.3.x,
`BTSWP_ENABLED`/`BTSWP_DISABLED` in 1.4.x). If MCC generated the file against one pack
and your build uses another, you get this. The fix is either to align the versions or
to write the bit numerically, which is what this project does — see the README.

Other build failures worth knowing:

| Message | Cause |
|---|---|
| `does not seem to support the selected device` | `-mdfp` points at the pack root instead of its `xc16` subdirectory — only relevant for command-line builds |
| `incompatible with 30Fxxxx output` | the linker script was not passed; MPLAB X does this for you |
| toolchain version warning on opening the project | harmless — *Project Properties → XC-DSC*, select the version you have |

### 2.1 It never reaches `main()`, or halts immediately

Almost certainly a wait loop in `clock_init()`. Halt the debugger and look at **which
line** you are on — each one tells you something different:

| Stuck at | Meaning | Where to look |
|---|---|---|
| `while (PLL1CONbits.PLLSWEN)` | the input/feedback divider update was not accepted | `PLL1DIV`: M outside 16…320, or F_PFD below 5 MHz |
| `while (PLL1CONbits.FOUTSWEN)` | the output divider update was not accepted | `POSTDIV1` must be ≥ `POSTDIV2`, and page 778 says the output dividers must not change while the PLL is running |
| `while (PLL1CONbits.OSWEN)` | the PLL will not switch to its source | `NOSC` in `PLL1CON` — is the FRC running? |
| `while (!OSCCTRLbits.PLL1RDY)` | PLL1 does not lock | F_VCO outside 500…1600 MHz. Read back `PLL1DIV` — if it is not the value you wrote, a `…SWEN` step was skipped |
| the same in `PLL2…` | as above, for the system clock | same checks on `PLL2DIV` |
| `while (CLK1CONbits.OSWEN)` | clock generator 1 will not switch | `NOSC = 0x6` (PLL2 out) — is PLL2 locked? |
| `while (CLK6CONbits.OSWEN)` | clock generator 6 will not switch | `NOSC = 0x5` (PLL1 out) — is PLL1 locked? |
| `while (!AD1CONbits.ADRDY)` | the ADC core never comes up | is CLKGEN6 running? Read `CLK6CONbits.CLKRDY` |

**Read back `PLL1DIV` and `PLL2DIV` while halted.** If they do not contain what the
code wrote, the divider update never took effect — that is the failure mode §1.2
describes, and it is silent.

**In the simulator all of these hang** — it models no PLL and no ADC. That is expected,
see the README. Use a hardware debugger.

**A useful trick:** put a breakpoint on the first line of `clock_init()` and step
through. After each switch, read the `CLKxCON` and `OSCCTRL` registers in the watch
window; `CLKRDY` and `PLL1RDY` tell you exactly how far you got.

### 2.2 It runs, but `blocks_done` stays at 0

Work through this in order:

1. **Is the ADC converting?** Read `AD1CH0DATA` repeatedly while halted. Does it
   change? If not, the ADC is not running: check `AD1CONbits.ON`, `ADRDY`, and that
   `TRG2SRC = 0x02` really is set.
2. **Is the DMA enabled?** `DMACONbits.ON` and `DMA0CHbits.CHEN` must both be 1.
3. **Right trigger?** Read back `DMA0SEL` — it must be `0x2F`. A wrong value here means
   the channel waits for an event that never comes.
4. **Is the interrupt enabled?** `IEC2bits.DMA0IE` must be 1. Note it is `IEC2`, not
   `IEC1` — DMA0 lives in the second interrupt register set.
5. **Is data arriving but the ISR not firing?** Look at `DMA0CNT`: if it counts down,
   transfers are happening and the problem is only the interrupt. Check
   `DMA0CHbits.DONEEN` and `IFS2bits.DMA0IF`.

### 2.3 `dma_overrun` is counting up

**This is not a bug — it is the measurement.** It means the DMA could not keep up with
the ADC, which is exactly the question this example exists to answer (see the README
section on the shared DMA bus).

What to do with the result:

1. Note at how many channels and what rate it starts. That number is the answer.
2. Reduce the load and confirm the mechanism: set `ADC1_SAMC` higher (slower sampling)
   and check the overruns disappear.
3. Then decide: average inside the ADC (`ACCNUM`, see README) or use fewer channels.

**Before reporting it as the bus limit, rule out the trivial cause:** if
`late_service` is also counting, your ISR is too slow and *that* is causing the
overrun, not the bus.

### 2.4 The values look wrong

| What you see | Most likely cause | What to do |
|---|---|---|
| all zeros | pin not connected, or wrong `PINSEL` for your package | check Table 16-2 (from page 1224) for which pin AD1AN0 is |
| all 0xFFF | input above AVDD, or pin tied high | check the level: 0 … 3.3 V |
| amplitude far too small | **source impedance too high for a 6.25 ns sample time** | raise `ADC1_SAMC` step by step and watch the amplitude come up |
| a straight line | signal frequency too low for a 25.6 µs window | use 100 kHz … a few MHz |
| plausible but noisy | expected — ENOB is 10.5 bits typical, and the example has no anti-alias filter | |
| values in a strange numeric range | **see §1.1** — the sample may not be in the lower 16 bits | compare `AD1CH0DATA` against the buffer content |
| every second value looks wrong | alignment or the buffers are not 4-byte aligned | the source uses `__attribute__((aligned(4)))`; check it survived |

### 2.5 `late_service` is counting up

The ISR did not finish before the next block was ready. At 39 kHz there is about 25 µs
per block, which is a lot of CPU cycles — so this usually means something else is
consuming them:

- `process_buffer()` is called from `main()`, not the ISR, but it competes for the CPU.
  Shorten it or let it process every second block.
- Enlarge `SAMPLES_PER_BUF` (fewer, larger blocks — 2048 halves the interrupt rate).
- Check whether another interrupt is interfering.

---

## Part 3 — Working methodically

If nothing above fits, strip the problem down. Each step is provable on its own:

1. **Does the CPU run at all?** Comment out everything except a GPIO toggle in the main
   loop. See it on a scope or an LED. This separates "device and toolchain work" from
   "our configuration works".
2. **Does the clock setup survive?** Keep `clock_init()`, then toggle a pin in a
   counted loop. The period tells you the real CPU frequency (see §1.3).
3. **Does the ADC convert without DMA?** Comment out `dma0_init()` and poll
   `AD1CH0DATA` in the main loop. Now you have ADC values with no DMA in the way.
4. **Does the DMA transfer without interrupts?** Leave `DONEEN = 0` and watch
   `DMA0CNT` count down and the buffer fill.
5. **Then switch the interrupt on.** If it breaks at this step, the problem is the
   ISR, not the ADC or the DMA.

This order matters because each step leaves exactly one new thing that can be wrong.

---

## Part 4 — When to come back to us

Please do, and bring this with you — it turns guesswork into a diagnosis:

- **Which step above got you stuck**, and at which source line
- **Register dump while halted:** `AD1CON`, `AD1CH0CON1`, `AD1CH0DATA`, `DMACON`,
  `DMA0CH`, `DMA0SEL`, `DMA0STAT`, `DMA0CNT`, `PLL1CON`, `PLL1DIV`, `PLL2CON`,
  `PLL2DIV`, `CLK1CON`, `CLK1DIV`, `CLK6CON`, `CLK6DIV`, `OSCCTRL`, `IEC2`, `IFS2`
- **The counters:** `blocks_done`, `dma_overrun`, `late_service`, `dma_bus_err`,
  `dma_addr_err`, `last_sample`
- **The first 32 values** from the buffer that was complete
- **Your versions:** MPLAB X, XC-DSC, dsPIC33AK-MP_DFP — and which board
- **Your signal:** frequency, amplitude, source impedance, which pin

The register dump is the important part. With it, most of these questions can be
answered without the board in front of us.

## One more thing we have not checked

**The errata.** We have not read the silicon errata for this device, and ADC, DMA,
clocking and high-resolution PWM are exactly the kind of modules that get errata
entries. If something behaves in a way that contradicts the datasheet, that is the next
place to look — and please tell us, because we would want to know too.
