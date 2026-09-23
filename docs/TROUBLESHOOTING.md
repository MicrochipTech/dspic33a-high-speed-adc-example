# Troubleshooting guide

This code has **never run on hardware**. It compiles cleanly and every register value
was read out of DS70005591D and the device pack ATDF — but "compiles" and "works" are
different things, and the first person to run it will find whatever is wrong.

This guide is written for that. It is ordered by how likely each thing is to be the
problem, and it tells you **where we are least certain of our own code**, so you do not
waste time on the parts that are solid.

**Revision 2026-09-22:** a review against the datasheet and the silicon errata found
four real mistakes in the previous version (ADC trigger mode, DMA address limits, status
flag clearing, buffer switching). They are fixed; the README has the list. The code was
then cut to the EV74H48A board: it runs a self-test on the ADC's internal reference before it
touches the external pin, every wait loop is bounded, and **LED0 tells you where it
stopped** — so most of this guide starts from what the LED shows.

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
  integration burst in a loop after reading `AD3CH0DATA`, which clears `CH0RDY`. The
  ISR does the same (`adc_start_burst()`). If bursts stop after the first one,
  `blocks_done` stays at 2 — see §2.2.
- **DMA and ADC stay in step.** Both count 2048. If `blocks_done` runs but the buffer
  content drifts (the first sample of a half is not where you expect), check
  `AD3CH0CNTbits.CNTSTAT` against `DMA0CNT` while halted.

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

**`OSCCTRL.PLLxEN` is deliberately not written — do not add it back.** Section 12.4.6
(page 776) reads as if it were required: "the PLLs can be enabled by PLLxEN bits". But
neither the normative procedure (12.4.6.3, Example 12-4, page 781) nor the MCC-generated
`clock.c` ever sets it; both enable the PLL through `PLLxCON.ON`, whose clock-request
path does the job. A first attempt on hardware set `PLLxEN` as well and waited for
`PLLxRDY` *before* the first divider write, following Example 16-3 (page 1328). That was
wrong on three counts: the snippet belongs to the ADC chapter's gain-calibration example,
it is duplicated verbatim in Example 17-4 (page 1392) with a comment that contradicts its
own code, and its arithmetic does not add up (`FBDIV = 80`, `POSTDIV1 = 4` is 160 MHz,
not the 320 MHz it claims). Worse, waiting for `PLLxRDY` before the dividers waits for a
lock on the POR configuration (`M = 200`, `POSTDIV1 = 2`, `POSTDIV2 = 2` → 400 MHz) —
precisely the intermediate state note 2 of 12.4.6.1 (page 779) warns about.

If `PLLSWEN` never clears (blink code 1, the first PLL wait), look elsewhere: `PLL1DIV`
read back against what the code wrote, and `OSCCTRL` in the register dump (`PLL1RDY` is
bit 14, `PLL1EN` bit 6 — it should read as enabled without the code touching it).

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

- **`DMA0SEL = 0x3B`** = "ADC3 Done CH0" (0x2F/0x35/0x3B/0x41/0x48 for ADC1…5, chosen by `ADC_INSTANCE`) — straight from the ATDF value group
  `DMA_SEL__CHSEL`.
- **`SIZE = 1` = 16-bit** — §13.4.2 page 824 and the register description on page 812.
- **`AD3CH0RES` low half = the sample** — `RES[11:0]` sits in bits 11:0, `RESF` in
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
- **Board facts** — LED0 on RC8, driven high to light; AD3AN5 on RA0 = mikroBUS A pin
  AN; UART2 to the MCP2221A COM port on RH1/RD1 with the PPS codes Microchip's own
  example uses on this board (DIM info sheet DS70005563A Table 1, user guide DS70005562D).
- **Self-test input** — ADxAN6 is the internal 15/16·VDD reference on every core and
  package (Table 16-2), the datasheet samples it the same way (Example 16-3).
- **The command parser** — `cmd_parser.c` is unchanged from its repository, where it
  has a PC test harness and has run on two other microcontrollers.
- **`FICD_NOBTSWP` and `FWDT_RCLKSEL` values** — read from both pack versions, see the README.

---

## Part 2 — Symptoms, in the order you will meet them

### 2.0 It does not even compile

Before anything else: **a configuration-bit error is almost always a version
mismatch, not a wrong value.**

```
error: unknown value for configuration setting 'FICD_NOBTSWP': 'BTSWP_ENABLED'
```

The symbolic names changed between pack versions (`ON`/`OFF` in 1.3.x,
`BTSWP_ENABLED`/`BTSWP_DISABLED` in 1.4.x; likewise `FWDT_RCLKSEL` is `BFRC256` in
1.3.x and `BFRC244` in 1.4.x). If MCC generated the file against one pack
and your build uses another, you get this. The fix is either to align the versions or
to write the bit numerically, which is what this project does — see the README.

Other build failures worth knowing:

| Message | Cause |
|---|---|
| `__DATA_BASE / __DATA_LENGTH not provided by the device header` | very old pack; replace the two macros in `dma.c` with `0x4000` and `0x10000` from your linker script |
| `does not seem to support the selected device` | `-mdfp` points at the pack root instead of its `xc16` subdirectory — only relevant for command-line builds |
| `incompatible with 30Fxxxx output` | the linker script was not passed; MPLAB X does this for you |
| toolchain version warning on opening the project | harmless — *Project Properties → XC-DSC*, select the version you have |

### 2.0a "Stuck at the first PLL wait, nothing on the COM port" — check the tool

Before anything else, look at the MPLAB X Dashboard (or *Project Properties →
Conn.*): the tool must be the board's **PKOB4** (`pkob4hybrid`). Anything that runs on
the PC rather than on silicon stops at `WAIT_WHILE(PLL1CONbits.PLLSWEN, 1u)`, because
there is no PLL to perform the switch, and leaves the terminal empty — exactly the
picture of a dead board, on a board that was never touched. The project ships with the
PKOB4 as its only tool, so this should not happen; if the Dashboard says anything else,
someone added it. The decisive check on hardware: LED0 either blinks slowly (good) or
blinks a code (the reason is in the terminal log). A dark LED0 after programming means
the code is not on the board.

### 2.0b "It lands in a break session" — a trap or an unhandled interrupt

MPLAB X halts on a line where nobody set a breakpoint, the Dashboard says *Halted*
instead of *Running*. That is not a mystery and not a debugger problem: the start-up
code links a weak `__DefaultInterrupt` into every one of the 364 vector slots, and it
is two instructions — `break` followed by `reset`. With a debugger attached the `break`
halts the core; without one the part reboots silently. Only two slots are ours (DMA0 =
IRQ 77, U2RX = IRQ 102), so **any** other event on the device ends up there.

**This build takes that vector over and reports the cause.** Instead of `break` you get
a `[TRAP]` block on the console and blink code **9**:

```
[TRAP] unhandled vector or CPU trap
[TRAP] INTTREG.VECNUM: 1
[TRAP] INTTREG.ILR: 1
[TRAP] reached boot stage: 4
[TRAP] last step completed: clock_init() done
[TRAP] vector 1 = CPU/FPU: read INTCON1/3/4 below
[TRAP] INTCON1: 0x00008008
...
[TRAP] INTCON1.ADDRERR: 1
```

How to read it:

| Line | What it tells you |
|---|---|
| `INTTREG.VECNUM` | **which** vector fired. `1` = CPU/FPU, i.e. a genuine CPU trap — the cause is then in the `INTCON*` bits below. `0` = the collapsed COMMON vector. Anything else is a peripheral raising an interrupt this example does not handle — the numbers worth knowing are in the table below |
| `reached boot stage` / `last step completed` | **where** in start-up it happened, even if the console did not exist yet. Stage 4 means `clock_init()` finished, so the clocks are not the suspect |
| `INTCON1.ADDRERR` | an illegal address was used — a bad pointer, or a DMA/linker address outside RAM |
| `INTCON1.STKERR` | stack overflow or underflow |
| `INTCON1.BADOPERR` | illegal opcode — usually a jump into data or through a null function pointer |
| `INTCON3.DMABET` / `CPUBET` | bus error trap from the DMA or the CPU |
| `INTCON4.DIV0ERR` | division by zero |
| `INTCON5.WDTE` / `DMTE` | watchdog or deadman timer expired |

**The vector numbers to expect on this device** (from the pack's ATDF interrupt list —
the authoritative source; `VECNUM` is the IRQ number, no offset to apply):

| VECNUM | Name | Why it could fire here |
|---|---|---|
| 201 | `AD3CH0Interrupt` | **the first suspect.** `adc_init()` sets `IRQSEL = 0`, so the ADC raises a channel-done event per conversion — that event is the DMA trigger and is meant to stay in the peripheral. If it also reaches the CPU, it arrives 40 million times a second on a vector with no handler. `IEC6` bit 9 is the enable; this code never sets it, so it should be masked — if VECNUM is 201 anyway, that assumption is wrong and is the bug |
| 202 … 212 | `AD3CMP0` … `AD3CH5` | other ADC3 sources, same family |
| 9 | `CLKFInterrupt` | the fail-safe clock monitor saw the system clock stop and moved the CPU to the backup FRC. This build has a handler for it (`[CLKF]` lines, blink code **10**), so 9 should not appear as a trap; 10 (`CLKEInterrupt`, clock error) still would |
| 77 | `DMA0Interrupt` | ours — should never appear here |
| 102 | `U2RXInterrupt` | ours — should never appear here |
| 1 | `CPUFPUInterrupt` | a real CPU trap; read the `INTCON*` bits |
| 0 | `COMMONInterrupt` | collapsed vector |

Any other number: look it up in
`packs/Microchip/dsPIC33AK-MP_DFP/<ver>/atdf/dsPIC33AK512MPS512.atdf`, search for
`interrupt index="<number>"`.

`boot_stage`, `trap_seen`, `trap_vec` and `trap_stage` live in **persistent RAM**, so
they survive the reset that a trap causes when no debugger is attached. If the previous
run ended in a trap, the next start-up says so up front:

```
[boot] WARNING the previous run ended in a trap
[boot] trap count: 1
[boot] last trap vector: 1
[boot] boot stage when it hit: 4
```

That is what turns "the board just keeps restarting" into a located fault. With the
debugger you can read the same four variables in the *Variables* window at any time.

**Can a trapped core still talk on the UART?** Yes, and the handler is built for it.
The break itself does not disable the transmitter: the core is halted by the debugger
*after* the handler runs, and without a debugger nothing halts at all. What could
swallow the message is the console's own state, so three things are deliberate:

- The handler calls `console_force_up()`, not `console_sync_baud()`. Pins, the PPS
  mapping (`_U2RXR`, `_RP114R`) and the whole UART are written again from scratch, and
  the baud divider is picked from the clock the CPU is actually on. A trap inside
  `clock_init()` therefore still prints — at FRC speed, which is the correct rate at
  that point.
- Every transmit wait is bounded (`TX_WAIT_LIMIT` in `cli.c`). This matters more than
  it looks: `console_puts()` used to spin on `U2STATbits.TXBF` forever, so a UART that
  was *not* transmitting would have hung inside the very report that explains the
  fault — the failure would have hidden its own diagnosis. A garbled line beats none.
- **LED0 is switched on before the first character is attempted.** Lighting it needs
  neither a working UART nor a sane clock. So a steady-lit LED0 with an empty terminal
  is itself a message: trapped, and the console did not survive it. After the report
  the LED blinks code 9.

If the terminal stays empty and LED0 is lit, read `trap_vec`, `trap_stage` and
`boot_stage` with the debugger — they hold the same information the console would have
printed, and they survive a reset.

### 2.1 The LED blinks a code — it stopped at a checkpoint

Nothing in this code waits forever. Every hardware wait is bounded by `WAIT_LIMIT`
iterations; when it runs out, `fail(code)` switches the DMA off, stores the code in
`fail_code`, **prints the code, its meaning and a full register dump on the console**
(the UART is up before the clocks are touched, so this works for codes 1 to 4 too) and
then blinks the code on LED0: *code* short blinks, a long pause, repeat. With a debugger
you land in `fail()` and `fail_code` tells you the same. A terminal log therefore
already contains what the "Where to look" column below asks for.

| Code | Where it gave up | Meaning | Where to look |
|---|---|---|---|
| 1 | `clock_init()`, PLL1 | `PLLSWEN`, `FOUTSWEN` or `OSWEN` did not clear, or `PLL1RDY` never came | `PLL1DIV`: M outside 16…320, F_PFD below 5 MHz, F_VCO outside 500…1600 MHz, `POSTDIV1` < `POSTDIV2`. Read `PLL1DIV` back — if it is not the value the code wrote, a `…SWEN` step was skipped |
| 2 | `clock_init()`, PLL2 | as above, for the system clock | same checks on `PLL2DIV` |
| 3 | `clock_init()`, CLKGEN1 | clock generator 1 will not switch to PLL2 (or back to the FRC at the start) | `CLK1CON`: `NOSC = 0x6`, is `PLL2RDY` set? |
| 4 | `clock_init()`, CLKGEN6 | clock generator 6 will not switch to PLL1 | `CLK6CON`: `NOSC = 0x5`, is `PLL1RDY` set? |
| 5 | `adc_init()` | the ADC core never reported `ADRDY` | is CLKGEN6 running? `CLK6CONbits.CLKRDY` |
| 6 | self-test or main loop | no buffer half completed within the limit | §2.2 |
| 7 | self-test | mean on the internal reference outside 3648 … 4032 | §2.4, `selftest_mean` |
| 8 | self-test or main loop | `DMA0CHbits.CHEN` went to 0 on its own | address fault: `dma_addr_err`, `DMALOW`, `DMAHIGH` |
| 9 | anywhere | CPU trap or an interrupt with no handler | the `[TRAP]` block on the console, §2.0b |
| 10 | `_CLKFInterrupt()` | the fail-safe clock monitor moved the CPU to the backup FRC (IRQ 9) | `[CLKF]` lines: `OSCCTRL` (is `PLL2RDY` still set?), `PLL2CON`, `CLK1CON.COSC`; a supply dip or a PLL2 divider outside its limits are the usual causes |
| 11 | `capture_service()` or the self-test | one of the 16 guard words directly behind the sample buffer changed: something wrote past the end of `buf` | the `[guard]` lines say which word and what it holds; `buf`/`buf_end`/`guard*` in the dump. If the value looks like a sample (12-bit), the DMA ran past the buffer — check `DMA0CNT` against the buffer size and the `SIZE` encoding in `dma.c` |

The blink *speed* depends on which clock the CPU is on when it stops (8 MHz FRC for
codes 1 and 2, 200 MHz afterwards); the code is chosen so that it reads the same either
way. Count the blinks, not the tempo.

### 2.2 Code 6: no blocks arrive, or the stream stops

Code 6 right after programming means the first buffer half never completed. Code 6
after the LED has blinked for a while means the stream ran and then stopped — almost
always the burst restart (§1.1). Halt and work through this in order:

1. **Is the ADC converting?** Halt and read `AD3CH0CNTbits.CNTSTAT`: it counts the
   conversions of the current burst. 0 means the burst never started — check
   `AD3CONbits.ON`, `ADRDY`, `TRG1SRC = 1`, `MODE = 2`, and that
   `AD3SWTRGbits.CH0TRG` was written (it is in `main()` and in the ISR).
2. **Did the DMA channel get disabled?** Read `DMA0CHbits.CHEN`. If it is 0 although
   the code set it, the DMA hit an address outside `DMALOW`…`DMAHIGH` and shut the
   channel off (p829 step 5). `dma_addr_err` will be non-zero. Check the two window
   registers contain `0x4000` and `0x13FFF`.
3. **Is the DMA enabled at all?** `DMACONbits.ON` and `DMA0CHbits.CHEN` must both be 1.
4. **Right trigger?** Read back `DMA0SEL` — it must be `0x3B` for ADC3 (`0x2F` … `0x48` for ADC1 … 5). A wrong value here means
   the channel waits for an event that never comes.
5. **Is the interrupt enabled?** `IEC2bits.DMA0IE` must be 1. Note it is `IEC2`, not
   `IEC1` — DMA0 lives in the second interrupt register set.
6. **Is data arriving but the ISR not firing?** Look at `DMA0CNT`: if it counts down,
   transfers are happening and the problem is only the interrupt. Check
   `DMA0CHbits.DONEEN`, `HALFEN` and `IFS2bits.DMA0IF`.
7. **`blocks_done` stops at exactly 2:** the first burst ran, the restart from the ISR
   did not take. See §1.1 — read `AD3STATbits.CH0RDY` and `AD3CH0CNTbits.CNTSTAT`
   while halted.

If `DMA0CHbits.CHEN` is 0 you get code 8 instead of 6: the DMA disabled itself, which
it only does on an address outside `DMALOW`…`DMAHIGH` (p829 step 5). Check the two
window registers contain `0x4000` and `0x13FFF`.

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

### 2.4 The values look wrong — or the self-test fails (code 7)

The self-test samples AD3AN6, the ADC's internal 15/16·VDD reference (Table 16-2,
p1224), with the sample time the datasheet uses for it (`SAMC = 3`, Example 16-3), and
expects a mean of 3840 ± 5 %. `selftest_mean` holds what it saw:

| `selftest_mean` | Most likely cause |
|---|---|
| 0 or a few counts | the DMA is copying from the wrong register, or the ADC is not converting at all — compare `AD3CH0RES` in the watch window |
| a few hundred, far below 3840 | sample time too short for the internal reference — raise `SELFTEST_SAMC` |
| 4095 | reference saturated — `VDD` and `AVDD` differ, or `DIFF`/`FRAC` are not 0 |
| plausible but outside the window | gain error larger than expected; widen `SELFTEST_MIN`/`MAX` and note the value — this is real device information |
| values above 4095 | the DMA source is the accumulator — `DMA0SRC` must be `&AD3CH0RES` |

For the external input, after the self-test passed:

| What you see | Most likely cause | What to do |
|---|---|---|
| all zeros | pin not connected, or wrong `PINSEL` for your package | check Table 16-2 (from page 1224) for which pin AD3AN5 is; on the EV74H48A it is mikroBUS A pin AN |
| all 0xFFF | input above AVDD, or pin tied high | check the level: 0 … 3.3 V |
| amplitude far too small | **source impedance too high for a 6.25 ns sample time** | raise `ADC1_SAMC` step by step and watch the amplitude come up |
| a straight line | signal frequency too low for a 25.6 µs window | use 100 kHz … a few MHz |
| plausible but noisy | expected — ENOB is 10.5 bits typical, and the example has no anti-alias filter | |
| values above 4095 or growing | the DMA source is the accumulator | `DMA0SRC` must be `&AD3CH0RES`, not `AD3CH0DATA` |
| every second value looks wrong | alignment, or the buffer is not 4-byte aligned | the source uses `__attribute__((aligned(4)))`; check it survived |

### 2.4a No console, or garbage on the terminal

- **Which COM port.** The board has two: the MCP2221A's (this console) and the
  PKOB4's (unused). Both appear when you plug the USB cable in. Try the other one.
  115200 8N1, no flow control.
- **With a debugger attached and no output at all**, read these while halted and
  compare: `RPCON` (IOLOCK), `RPOR28` (RP114R, bits 8-14, must be 21), `RPINR13`
  (U2RXR, bits 16-23, must be 50), `TRISH` (bit 1 clear), `U2CON` (ON, TXEN, RXEN set,
  CLKMOD set), `U2BRG` (35 on the FRC, 868 on PLL2), `U2STAT` (`TXBE` set when idle).
  A `U2BRG` of 0 or `RPOR28` of 0 means the init did not run or did not take.
- **Nothing at all, LED blinks normally.** Press Enter — the prompt is only sent once
  at start-up and after each command. If still nothing: the PPS mapping (`_RP114R`,
  `_U2RXR`) or `TRISH1`. Read `U2STATbits.TXBE`: 1 means the transmitter is idle and
  the bytes went somewhere.
- **Garbage.** Baud rate. The UART clock is the 100 MHz standard-speed peripheral clock
  and `U2BRG = 868` in fractional mode gives 115 207 baud — only if PLL2 really drives
  CLKGEN1 at 200 MHz. A wrong CPU clock shows up here first (§1.2).
- **Commands echo but nothing happens.** The receive interrupt is not running: check
  `IEC3bits.U2RXIE`, `IPC12bits.U2RXIP` (must be 1 … 7) and that `_U2RXInterrupt` is in
  the vector table (IRQ 102).
- **`proc_missed` rises while you type.** Expected during a long reply — see the
  README, "The console".

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

1. **Does the CPU run at all?** Call `fail(3)` as the first line of `main()`: LED0
   must blink three times and pause. If it does not, the board is not programmed, not
   powered, or LED0 is not on RC8 (a different board). This separates "device and
   toolchain work" from "our configuration works".
2. **Does the clock setup survive?** Keep `clock_init()`, then `fail(3)` right after
   it. The blink is now 25 times faster than in step 1 if PLL2 drives the CPU — the
   `fail()` timing assumes 200 MHz once `CLK1CON.COSC` reports PLL2 (see §1.2).
3. **Does the ADC convert without DMA?** Comment out `capture_init()`, trigger one burst
   with `adc_start_burst()` and watch `AD3CH0CNTbits.CNTSTAT` climb to 2048 and
   `AD3STATbits.CH0RDY` go to 1. Now you have ADC values with no DMA in the way.
4. **Does the DMA transfer without interrupts?** Leave `DONEEN = HALFEN = 0` and watch
   `DMA0CNT` count down and the buffer fill.
5. **Then switch the interrupts on.** If it breaks at this step, the problem is the
   ISR, not the ADC or the DMA.

This order matters because each step leaves exactly one new thing that can be wrong.

---

## Part 4 — When to come back to us

Please do, and bring this with you — it turns guesswork into a diagnosis.

**The short version: the terminal log from power-up.** Open the MCP2221A's COM port at
115200 8N1 with logging on (Tera Term: *File → Log*; PuTTY: *Session → Logging*),
then press the board's reset button or re-plug it. The firmware reports every start-up
step, the self-test result, a `[stat]` line with all counters every 5 s, and on a
failure the reason and a complete register dump. Let it run for a minute, then type
`regs` and `stats` once, and send the file. That covers everything in the list below
except the signal details.

If you prefer the debugger, or the console itself is what does not work:

- **Which step above got you stuck**, and at which source line
- **Register dump while halted:** `AD3CON`, `AD3STAT`, `AD3CH0CON1`, `AD3CH0CNT`,
  `AD3CH0RES`, `AD3CH0DATA`, `DMACON`, `DMALOW`, `DMAHIGH`, `DMA0CH`, `DMA0SEL`,
  `DMA0STAT`, `DMA0CNT`, `DMA0DST`, `PLL1CON`, `PLL1DIV`, `PLL2CON`, `PLL2DIV`,
  `CLK1CON`, `CLK1DIV`, `CLK6CON`, `CLK6DIV`, `OSCCTRL`, `IEC2`, `IFS2`
- **The LED pattern** you saw, and `fail_code`
- **The counters:** `blocks_done`, `dma_overrun`, `late_service`, `proc_missed`,
  `dma_bus_err`, `dma_addr_err`, `last_sample`, `ready_half`, `selftest_mean`
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
