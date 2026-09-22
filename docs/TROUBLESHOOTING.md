# Troubleshooting guide

This code has **never run on hardware**. It compiles cleanly and every register value
was read out of DS70005591D and the device pack ATDF — but "compiles" and "works" are
different things, and the first person to run it will find whatever is wrong.

This guide is written for that. It is ordered by how likely each thing is to be the
problem, and it tells you **where we are least certain of our own code**, so you do not
waste time on the parts that are solid.

**Revision 2026-09-22:** a review against the datasheet and the silicon errata found
four real mistakes in the previous version (ADC trigger mode, DMA address limits, status
flag clearing, buffer switching). They are fixed; the README has the list. This guide was
rewritten to match.

---

## Part 1 — Where this code is most likely wrong

Read this before debugging anything. These are our own doubts, most suspect first.

### 1.1 The burst restart (highest remaining risk)

The ADC cannot free-run indefinitely: back-to-back triggering exists only in the
multisample modes, and Integration mode stops after `CNT` conversions (max 65535).
This code sets `CNT = 2048` = one full DMA buffer and restarts the burst with a software
trigger from the DMA `DONE` interrupt. That keeps ADC and DMA in lock-step, but it is
**our own construction — no Microchip example combines an ADC burst, a continuous DMA
transfer and a restart from the ISR** (the README section "What comes from where" lists
what was taken from which example). It rests on two things we could not test:

- **The restart is accepted.** Datasheet Example 16-6 (p1331) retriggers a finished
  integration burst in a loop after reading `AD1CH0DATA`, which clears `CH0RDY`. The
  ISR does the same (`adc1_start_burst()`). If bursts stop after the first one,
  `blocks_done` stays at 2 — see §2.2.
- **DMA and ADC stay in step.** Both count 2048. If `blocks_done` runs but the buffer
  content drifts (the first sample of a half is not where you expect), check
  `AD1CH0CNTbits.CNTSTAT` against `DMA0CNT` while halted.

The restart costs the interrupt latency once per 51.2 µs, so the measured rate will sit
slightly below 40 MSPS. That is expected and not an overrun.

### 1.2 The PLL setup

This used to be our biggest worry. It is no longer, because we found Microchip's own
MCC-generated example for this exact part and aligned the code with it:

**https://github.com/microchip-pic-avr-examples/dspic33ak-curiosity-adc-40msps**

That example runs at 40 MSPS on a Curiosity board and covers both dsPIC33AK128MC106
and dsPIC33AK512MPS512. It has no DMA — which is why this project exists — but its
clock setup has been on hardware, and ours uses the same register values (checked bit
for bit) and the same switching sequence.

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
`CLK6DIV` are both 0. The code also parks the system clock on the FRC before touching
the PLLs, because changing PLL settings underneath a running CPU clock can overclock
the core. This matters on a debugger restart, where the part is not freshly reset.

One thing to keep in mind for anything beyond a functional check: 320 MHz is the
specified maximum ADC input clock, and it is derived from the FRC, whose tolerance puts
the actual value on either side of that limit. The MCC example does the same, so it is
fine for a bench test; a product would use the primary oscillator.

### 1.3 Interrupt priority is left at default

The code enables the DMA0 interrupt but never sets its priority; the reset default of
`IPC9.DMA0IP` is 4. At 39 000 interrupts per second that is usually fine because
nothing competes — but if you add UART or CAN later, this is where jitter,
`late_service` and `proc_missed` counts will come from.

### 1.4 Things we consider solid

So you do not hunt here first. Each was read from a primary source and cross-checked:

- **`DMA0SEL = 0x2F`** = "ADC1 Done CH0" — straight from the ATDF value group
  `DMA_SEL__CHSEL`.
- **`SIZE = 1` = 16-bit** — §13.4.2 page 824 and the register description on page 812.
- **`AD1CH0RES` low half = the sample** — `RES[11:0]` sits in bits 11:0, `RESF` in
  bits 31:20 (register summary p1229). With `FRAC = 0` the result is right aligned
  (p1265).
- **`MODE = 2`, `TRG1SRC = 1`, `TRG2SRC = 2`, then a software trigger** — §16.4.5
  page 1322, Example 16-6 page 1331, and the MCC 40 MSPS example.
- **`DMALOW`/`DMAHIGH` required** — page 809 f., §13.4.5 page 826, and every code
  example in the DMA chapter sets them.
- **`DMAxSTAT` flags clear by writing 0** — legend "C = Clearable" page 815, Example
  13-4 page 835.
- **320 MHz is the ADC maximum, TAD = 4/F_IN** — Table 40-24 page 2016 and AD50.
- **CLKGEN6 feeds the ADC, CLKGEN1 the system** — Table 16-1 page 1223, §12.4.9
  page 795.
- **Interrupt plumbing** — DMA0 is IRQ 77, `IEC2`/`IFS2` bit 13, `INTCON1.GIE` is
  set at reset; the linked ELF has `_DMA0Interrupt` at IVT entry 85.
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
| `__DATA_BASE / __DATA_LENGTH not provided by the device header` | very old pack; replace the two macros in `dma0_init()` with `0x4000` and `0x10000` from your linker script |
| `does not seem to support the selected device` | `-mdfp` points at the pack root instead of its `xc16` subdirectory — only relevant for command-line builds |
| `incompatible with 30Fxxxx output` | the linker script was not passed; MPLAB X does this for you |
| toolchain version warning on opening the project | harmless — *Project Properties → XC-DSC*, select the version you have |

### 2.1 It never reaches `main()`, or halts immediately

Almost certainly a wait loop in `clock_init()` or `adc1_init()`. Halt the debugger and
look at **which line** you are on — each one tells you something different:

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

### 2.2 It runs, but `blocks_done` stays at 0 — or stops at 2

Work through this in order:

1. **Is the ADC converting?** Halt and read `AD1CH0CNTbits.CNTSTAT`: it counts the
   conversions of the current burst. 0 means the burst never started — check
   `AD1CONbits.ON`, `ADRDY`, `TRG1SRC = 1`, `MODE = 2`, and that
   `AD1SWTRGbits.CH0TRG` was written (it is in `main()` and in the ISR).
2. **Did the DMA channel get disabled?** Read `DMA0CHbits.CHEN`. If it is 0 although
   the code set it, the DMA hit an address outside `DMALOW`…`DMAHIGH` and shut the
   channel off (p829 step 5). `dma_addr_err` will be non-zero. Check the two window
   registers contain `0x4000` and `0x13FFF`.
3. **Is the DMA enabled at all?** `DMACONbits.ON` and `DMA0CHbits.CHEN` must both be 1.
4. **Right trigger?** Read back `DMA0SEL` — it must be `0x2F`. A wrong value here means
   the channel waits for an event that never comes.
5. **Is the interrupt enabled?** `IEC2bits.DMA0IE` must be 1. Note it is `IEC2`, not
   `IEC1` — DMA0 lives in the second interrupt register set.
6. **Is data arriving but the ISR not firing?** Look at `DMA0CNT`: if it counts down,
   transfers are happening and the problem is only the interrupt. Check
   `DMA0CHbits.DONEEN`, `HALFEN` and `IFS2bits.DMA0IF`.
7. **`blocks_done` stops at exactly 2:** the first burst ran, the restart from the ISR
   did not take. See §1.1 — read `AD1STATbits.CH0RDY` and `AD1CH0CNTbits.CNTSTAT`
   while halted.

### 2.3 `dma_overrun` is counting up

**This is not a bug — it is the measurement.** It means the DMA channel was triggered
again before it had finished the previous transfer (p816), i.e. the DMA bus could not
keep up with the ADC — exactly the question this example exists to answer (see the
README section on the shared DMA bus).

What to do with the result:

1. Note at how many channels and what rate it starts. That number is the answer.
2. Reduce the load and confirm the mechanism: set `ADC1_SAMC` higher (slower sampling)
   and check the overruns disappear.
3. Then decide: average inside the ADC (`ACCNUM`, see README) or use fewer channels.

**Before reporting it as the bus limit, rule out the trivial causes:** with one channel
and nothing else running, an overrun means the bus lost against something — check that
no other DMA channel is enabled and that no other interrupt is hogging the CPU.

### 2.4 The values look wrong

| What you see | Most likely cause | What to do |
|---|---|---|
| all zeros | pin not connected, or wrong `PINSEL` for your package | check Table 16-2 (from page 1224) for which pin AD1AN0 is |
| all 0xFFF | input above AVDD, or pin tied high | check the level: 0 … 3.3 V |
| amplitude far too small | **source impedance too high for a 6.25 ns sample time** | raise `ADC1_SAMC` step by step and watch the amplitude come up |
| a straight line | signal frequency too low for a 25.6 µs window | use 100 kHz … a few MHz |
| plausible but noisy | expected — ENOB is 10.5 bits typical, and the example has no anti-alias filter | |
| values above 4095 or growing | the DMA source is the accumulator | `DMA0SRC` must be `&AD1CH0RES`, not `AD1CH0DATA` |
| every second value looks wrong | alignment, or the buffer is not 4-byte aligned | the source uses `__attribute__((aligned(4)))`; check it survived |

### 2.5 `late_service` or `proc_missed` is counting up

`late_service`: the ISR found `HALF` and `DONE` set at the same time, so it arrived
more than one half (25.6 µs) late and the first half had already been overwritten.
`proc_missed`: the ISR was fine, but `main()` did not get to a completed half before the
next one was done. Both usually mean something else is consuming the CPU:

- `process_buffer()` competes with everything else in `main()`. Shorten it or let it
  process every second half.
- Enlarge `SAMPLES_PER_HALF` (fewer, larger blocks — 2048 halves the interrupt rate;
  keep `CNT` ≤ 65535).
- Check whether another interrupt is interfering (§1.3).

---

## Part 3 — Working methodically

If nothing above fits, strip the problem down. Each step is provable on its own:

1. **Does the CPU run at all?** Comment out everything except a GPIO toggle in the main
   loop. See it on a scope or an LED. This separates "device and toolchain work" from
   "our configuration works".
2. **Does the clock setup survive?** Keep `clock_init()`, then toggle a pin in a
   counted loop. The period tells you the real CPU frequency (see §1.2).
3. **Does the ADC convert without DMA?** Comment out `dma0_init()`, trigger one burst
   with `adc1_start_burst()` and watch `AD1CH0CNTbits.CNTSTAT` climb to 2048 and
   `AD1STATbits.CH0RDY` go to 1. Now you have ADC values with no DMA in the way.
4. **Does the DMA transfer without interrupts?** Leave `DONEEN = HALFEN = 0` and watch
   `DMA0CNT` count down and the buffer fill.
5. **Then switch the interrupts on.** If it breaks at this step, the problem is the
   ISR, not the ADC or the DMA.

This order matters because each step leaves exactly one new thing that can be wrong.

---

## Part 4 — When to come back to us

Please do, and bring this with you — it turns guesswork into a diagnosis:

- **Which step above got you stuck**, and at which source line
- **Register dump while halted:** `AD1CON`, `AD1STAT`, `AD1CH0CON1`, `AD1CH0CNT`,
  `AD1CH0RES`, `AD1CH0DATA`, `DMACON`, `DMALOW`, `DMAHIGH`, `DMA0CH`, `DMA0SEL`,
  `DMA0STAT`, `DMA0CNT`, `DMA0DST`, `PLL1CON`, `PLL1DIV`, `PLL2CON`, `PLL2DIV`,
  `CLK1CON`, `CLK1DIV`, `CLK6CON`, `CLK6DIV`, `OSCCTRL`, `IEC2`, `IFS2`
- **The counters:** `blocks_done`, `dma_overrun`, `late_service`, `proc_missed`,
  `dma_bus_err`, `dma_addr_err`, `last_sample`, `ready_half`
- **The first 32 values** from the half that was complete
- **Your versions:** MPLAB X, XC-DSC, dsPIC33AK-MP_DFP — and which board and silicon
  revision (errata below)
- **Your signal:** frequency, amplitude, source impedance, which pin

The register dump is the important part. With it, most of these questions can be
answered without the board in front of us.

## The errata

Silicon errata DS80001162E (July 2026) was read for this revision. Of its 28 items,
these touch what this code does:

- **Item 2, DMA:** `BRERR` is only set when `RETEN = 1`, and `RETEN` also raises a
  trap. This code leaves `RETEN = 0`, so `dma_bus_err` only ever counts write errors.
- **Item 22, CPU (rev A1 only):** an address error trap can occur with indirect
  register-offset addressing. Workaround is the compiler option
  `-merrata=base_offset` (*Project Properties → XC-DSC → xc-dsc-gcc → Additional
  options*). Not needed on rev A2 — check the marking on your board.
- **Item 26, CPU:** a cache invalidation followed by an IVT fetch can stall the CPU.
  Only triggered by `BOOTSWP`, manual cache invalidation or run-time self-programming,
  none of which this code does.
- **Item 28, debugger:** hardware breakpoints at branch targets can be missed. If a
  breakpoint in the ISR does not hit, enable software breakpoints.

Nothing in the errata concerns the ADC, the clock generators or the PLLs. If something
behaves in a way that contradicts the datasheet anyway, please tell us — we would want
to know too.
