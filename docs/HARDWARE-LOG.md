# Hardware log - every run on the board, dated

Board: EV74H48A with dsPIC33AK512MPS512 GP DIM, PKOB4, console on the MCP2221A COM port,
115200 8N1. Until 23.09.2026 nothing in this repository had run on silicon; everything
below is what the board said and what was changed because of it. Add an entry for
every run.

## 2026-09-23, run 1 - state of 22.09. afternoon (before the trap handler)

Boot, clocks, console, ADC ready, DMA armed, "[boot] self-test on the internal
reference" - then a trap, caught by a handler the tester had added himself:
`INTTREG.VECNUM = 2`, `ILR = 14`. No further output.

Findings: the whole clock chain works on silicon (PLL1 320 MHz, PLL2 200 MHz, switch
to PLL2, UART re-clocked). The tail of "[clk] PLL2 locked, 200 MHz" came out as
garbage: `console_puts()` returns when the last character is in the FIFO, and the
clock switched while it was still in the shift register. Vector 2 is `XRAMECCInterrupt`
per the pack's interrupt list, not an address trap.

Changed: `console_flush()` before the CLKGEN1 switch; the trap report prints `PCTRAP`
(the PC at the trap) and explains vectors 2..5.

## 2026-09-23, run 2 - master 11:21 (after `git reset --hard` + `git pull`)

Self-test passed: mean 3815 on the internal 15/16 VDD reference. So ADC, DMA, DMA
interrupt and buffer work. Then `fail 6` (stream stopped). Register dump: `DMA0STAT =
OVERRUN | DONE` with `IFS2 = 0`, `AD3CH0CNT.CNTSTAT = CNT`, `CH0RDY`, `DMA0DST` back at
the buffer start - a DONE happened, its interrupt was wiped, nothing restarted the
burst. Counters: `dma_overrun = 7157` in 253 halves, `late_service = 9`.

Changed: the DMA interrupt clears `DMA0IF` first, then reads the status; `dma0_clear()`
writes the status word once instead of bit-field read-modify-write (a flag set by the
hardware between the read and the write was written back as 0).

The trap of run 1 did not reappear on master.

## 2026-09-23, run 3 - master 11:36 (screenshot)

Self-test passed (3824), then immediately "[boot] uart up on FRC" again: the part
reset as soon as the stream ran at SAMC 0 with the main loop processing. No `[TRAP]`
block, no "previous run ended in a trap" (persistent RAM is random after a POR/BOR,
so main() normalises it). Typed characters echoed, Enter did nothing.

Changed: `RCON` printed and decoded at boot (this device has POR, BOR, WDTO, SWR, EXTR,
CM, BUCKR, VREG2R..4R - no TRAPR); the receive interrupt counts bytes, CR and LF and
shows them in `[stat]` and `status`; the rate sweep runs automatically after the
self-test (`AUTO_SWEEP`), so a board that dies at some rate shows the last rate it
survived; 16 guard words behind the buffer (`fail 11`); the buffer became a dedicated
volatile object in its own section and the DMA window is exactly that buffer.

## 2026-09-23, run 4 - master 12:00 (dedicated buffer, window 0x4070..0x506F, auto sweep)

`RCON = EXTR` (MCLR from the programmer - the expected cause for the first boot after
flashing). Self-test 3817. The sweep, 2000 halves per point:

```
samc 31  ksps 1250   overrun idle/process/sfr 61185/61165/61036  late 0  missed 1888
samc 15  ksps 2500   overrun idle/process/sfr 82215/84825/82215  late 0  missed 1906
samc 7   ksps 5000   overrun idle/process/sfr 82000/85936/82000  late 0  missed 1908
samc 3   ksps 10000  overrun idle/process/sfr 82579/88501/82579  late 0  missed 1940
samc 1   ksps 20000  overrun idle/process/sfr 82665/86295/82665  late 0  missed 1869
samc 0   ksps 40000  overrun idle/process/sfr 83083/84494/83083  late 0  missed 1773
```

Then "measurement running on the external input"; the log ends there (whether `[stat]`
lines or another boot followed is not known).

Reading: the numbers do not describe six rates. Overrun is the same at every nominal
rate (idle and sfr identical to the digit in five rows), and 1900 of 2000 halves are
missed even at a nominal 1.25 MSPS, where a half takes 820 us and the processing 25 us.
The only consistent story: the delivered rate did not change between rows - `SAMC` is
written (the register is `AD3CH0CON1[20:16]`) but does not change the stream in
Integration mode with the back-to-back trigger. The ADC ran at about 40 MSPS in every
row, losing about 4 % of the samples as OVERRUN regardless of CPU activity, with every
overrun raising the DMA interrupt (~1.6 million per second), which starves the main
loop (missed) and the priority-1 console (Enter "does nothing"). No `late`, no
`addr_err`, no guard violation: the DMA stays inside the buffer.

Changed: the sweep measures the rate with Timer1 (32-bit, peripheral clock / 8, time
base checked against `__delay32()` and printed) and reads `SAMC` back from the
register.

Next: the measured `ksps` column decides. If it is the same in every row, the rate has
to be set through the ADC clock (`CLK6DIV`) or a timer trigger instead of `SAMC`; only
then does the sweep become the curve the example exists for.

## 2026-09-23, after run 4 - the trigger changed to the ADC repeat timer (no board run yet)

Decision (before the measured column was even in): a deterministic sample rate needs a
time base that is not the ADC finishing its previous conversion. Two facts from the
datasheet (DS70005591D, read from the PDF): Table 16-4 (p1227) lists `TRG2SRC = 000011`
as "Conversion repeat timer trigger defined by RPTCNT[5:0] (ADnCON[23:18])", and 16.4.5
(p1322) says "This timer is clocked from the ADC analog core clock (TAD), and its period
is set by RPTCNT[5:0]"; the same section says of back-to-back: "The timing is affected
(can be delayed) by priorities of other channels". Example 16-4 (p1330) uses the repeat
timer with `RPTCNT = 60`; Example 16-8 (p1334) uses CCP1 as trigger (`TRG2SRC = 32`).

Also found: Microchip's own 40 MSPS example for this board (dspic33ak-curiosity-adc-
40msps) does not use the DMA at all. It copies `AD3CH0RES` in a hand-timed assembly loop
("200MHz CPU : 40MSPS = 5 instructions per sample") for 800 samples, with the
back-to-back trigger. Five CPU cycles is the budget one sample has at 40 MSPS; that the
DMA lost about 4 % at that rate is consistent with a transaction costing more.

Changed: `TRG2SRC = 3`, `RPTCNT` from `ADC_RPTCNT` in board.h (default 2 = 40 MSPS
nominal at TAD 12.5 ns), `period <2..63>` command, `rpt=` in the status line, the sweep
now steps `RPTCNT` 63, 32, 16, 8, 4, 3, 2 (1.27 to 40 MSPS nominal, "nominal =
80000/rptcnt ksps") and prints the measured rate next to it. Rates below 1.27 MSPS would
need `CLK6DIV` or the CCP trigger; not built.

Added with it, still before any board run: a **rate self-test** after the reference
self-test (`capture_ratetest()`, `fail 12`): 200 halves at RPTCNT 16 and at RPTCNT 4,
delivered rate measured against Timer1 (`timebase.c`, checked once against
`__delay32()`), each within 10 % of nominal and the two four times apart — the check
that would have caught run 4's "rate does not change" without a sweep. And
`BOOT_VERBOSE` (board.h, default 0): the `[clk]`/`[adc]`/`[dma]` register commentary
at boot is off; reset cause, self-test, rate test, sweep and every failure still print.

Because the repeat-timer pairing rests on the datasheet text alone (and Example 16-3 has
already shown the datasheet can be wrong in detail), the firmware now tries the
candidates itself instead of leaving a switch to flip: `ADC_PACING` in board.h, default
AUTO, runs the rate test on the ADC repeat timer (`TRG2SRC = 3`), on SCCP1 as a timer
whose period match triggers the ADC (`TRG2SRC = 32`, the pairing datasheet Example 16-8
p1334 shows with Integration mode; `sccp.c`) and on back-to-back (measured only), prints
one `[ratetest]` verdict per source and a `[pacing] summary` line, and uses the first
paced source that passed - back-to-back if none. `pacing <3|32|2>` and `period <n>`
change it at run time; the sweep steps the list of the active source (SCCP1: 80, 40,
20, 10, 8, 5, 4 ticks = 1.25 to 25 MSPS, so 25 MSPS is on this grid).

## 2026-09-23, branch `nano-board` - the second board, untested

CLAAS will order the dsPIC33AK512MPS506 Curiosity Nano (EV17P63A) from 1 October, not
the EV74H48A the measurements run on. Branch `nano-board` adds it without touching
master: `board.h` carries two profiles (`BOARD`), the MPLAB X configuration `EV17P63A_Curiosity_Nano_MPS506` and
`build.bat nano` select the second one (device MPS506, `nEdbgTool`, `BOARD=2`). Facts
from the Nano user guide DS70005634: LED0 RD0 active low, SW0 RC3, AD1AN0 on RA2 (RP3),
the debugger's CDC channel on RC10 = RP43 (target TX) and RC11 = RP44 (target RX); from
the datasheet: ANSEL resets to analog (p640), ADC1 channel 0 is IRQ 157 (IEC4 bit 29).
Board-specific code that was hard-wired to ADC3 and to the EV74H48A pins (interrupt
masking, the `adif=` field, the trap text, the routing registers in the dump, the
banner) now derives from `ADC_INSTANCE` and `board.h`. Builds clean for both devices and
the simulator; nothing on a Nano yet. Plan: master keeps the EV74H48A measurements,
this branch takes them in by merge, and goes to master once a Nano has run it.

What the whole exercise is for, stated once: continuous sampling, ADC and DMA running
in the background into a ping-pong buffer, the CPU processing the half that is not
being written. The sweep's `process` column is exactly that case, and the highest rate
at which it shows `overrun 0` and `missed 0` is the answer for this device.

## 2026-09-23, run 5 - master built 15:27 (pacing trial: repeat timer, SCCP1, back-to-back)

The colleague's terminal, as pasted (the log is cut after the sweep header; whether a
sweep row, `[stat]` lines or another boot followed is not known):

```
[boot] uart up on FRC, 115200 8N1
[boot] adc_dma_40msps Sep 23 2026 15:27:16
[boot] RCON: 0x00000080
[boot] reset cause: EXTR

adc_dma_40msps - ADC at 40 MSPS into RAM via DMA
board: EV74H48A, dsPIC33AK512MPS512 GP DIM
build: Sep 23 2026 15:27:17
type 'help' for the commands
please log this terminal from power-up and send it back
> [boot] self-test on the internal reference
[selftest] mean on internal 15/16 VDD (expect ~3840): 3819
[pacing] time base check, ticks per 100 ms (expect 1250000): 1250001
[ratetest] pacing: ADC repeat timer (period in TAD = 12.5 ns)
[ratetest]   period: 16
[ratetest]     nominal ksps: 5000
[ratetest]     measured ksps: 36493
[ratetest]     outside the 10 % window
[ratetest]   FAIL
[ratetest] pacing: SCCP1 timer (period in ticks of 10 ns)
[ratetest]   period: 20
[ratetest]     nominal ksps: 5000
[ratetest]     measured ksps: 37898
[ratetest]     outside the 10 % window
[ratetest]   FAIL
[ratetest] pacing: back-to-back (no rate control)
[ratetest]   measured ksps (no period to compare with): 35284
[ratetest]   PASS
[pacing] summary: repeat timer FAIL, SCCP1 timer FAIL, back-to-back runs
[pacing] using: back-to-back (no rate control) - NO PACED SOURCE PASSED, the rate is not under control
[boot] automatic rate sweep before the measurement (AUTO_SWEEP in board.h)
[sweep] halves per point: 2000
[sweep] pacing: back-to-back (no rate control)
[sweep] idle = CPU polls RAM only, process = main-loop processing, sfr = CPU polls an SFR
[sweep] overrun must be 0 for a usable rate; late/missed are from the process run
[sweep] nominal = the rate the period should give, measured = samples per second the DMA
[sweep] delivered (Timer1), reg = the period read back from the hardware
[sweep] timer check, ticks per 100 ms (expect 1250000): 1250001
[sweep]
```

Reading: the time base is right (1 250 001 ticks per 100 ms, twice), so the measured
rates are real. Neither paced source paces. With the repeat timer at RPTCNT 16 (nominal
5 MSPS) the DMA received 36.5 MSPS, with SCCP1 at 20 ticks (nominal 5 MSPS) 37.9 MSPS,
back-to-back 35.3 MSPS: three trigger sources, one rate, and it is the converter's own
rate minus the burst-restart gap. Either the `TRG2SRC`/`RPTCNT` writes do not take
(the register read-back, `reg` in a sweep row and `AD3CH0CON1` in the `regs` dump, would
show that; neither is in this log), or in Integration mode the conversions inside a
burst run back-to-back whatever `TRG2SRC` says and the repeat timer only matters for
single conversions. The datasheet text (16.4.5) does not settle this; the board has.

The log ends with `[sweep] ` and no row: the first (and, for back-to-back, only) sweep
point did not print within whatever time the colleague waited. At 35 MSPS the three
loads of 2000 halves take about 0.2 s; `sweep_point()` is bounded by
`SWEEP_WAIT_LIMIT`, a trap would print `[TRAP]`, a `fail()` would print `[FAIL]`. So
either the paste was taken while the row was still pending, or the CPU is starved the
way run 4 described (every overrun raises the DMA interrupt, about 1.6 million per
second at this rate). Open until the next, complete log.

Changed for the next run, so that a log answers these questions by itself: the banner
carries the git revision (`tools/version.bat` -> `version.h`, run before every build
from the IDE and the command line), a `[build]` block prints board and every
compile-time switch, a `[regs]` snapshot is printed after initialisation and before
the self-test (`AD3CH0CON1` with `TRG2SRC` and `RPTCNT` included), and every `[stat]`
line is followed by a `[half]` line with min/max/mean/pp of the last completed half.

## 2026-09-23, run 6 - master 1ebb140 (+local changes), first complete log, about 30 s

`RCON = EXTR`. Self-test 3817. Time base 1 250 011 / 1 250 001. Rate test as in run 5:
repeat timer at RPTCNT 16 delivered 36 067 ksps, SCCP1 at 20 ticks 37 401, back-to-back
37 533 - no paced source, back-to-back used. One sweep row (back-to-back): measured
38 262 ksps, overrun idle/process/sfr 82736/86980/82299 of 2 048 000 samples, missed
1825 of 2000. Then four `[stat]` lines 5 s apart:

```
[stat] blocks=195425 overrun=7815329  late=0 missed=187326 ... last=17 ... pace=2 per=0 run=1 ad3if=1 rx=0 last=0x00000000 cr=0 lf=0
[half] n=0 min=8 max=32 mean=17 pp=24
[stat] blocks=390651 overrun=15916707 late=0 missed=381793 ...
[stat] blocks=586133 overrun=24028708 late=0 missed=576515 ...
[stat] blocks=781391 overrun=32131436 late=0 missed=771014 ...
```

Register snapshot, decoded with the device header (`_AD1CH0CON1_*_POSITION` etc.):
`AD3CH0CON1 = 0x05000381` = TRG1SRC 1 (software), MODE 2 (Integration), **TRG2SRC 3
(repeat timer)**, SAMC 0, PINSEL 5. `AD3CON = 0xC30A8000` = ON, **RPTCNT 2**, ADRDY,
CALRDY. `DMA0CH = 0x06001C4B` = CHEN, HALFEN, DONEEN, SIZE 1 (16 bit), TRMODE 3,
DAMODE 1, RELOADD/RELOADC. `U2CON = 0x48008030` = ON, RXEN, TXEN, MODE 0.
`U2STAT = 0x001E0000` = RXBE, XON, **RCIDL** (receiver idle, line at the idle level),
TXBF; no FERR/OERR/PERR. `IEC3 = 0x40` (U2RX on), `IPC12` U2RX priority 1, `IPC9` DMA0
priority 4. `RPINR13 = 0x00320000` (U2RXR = 50 = RD1), `TRISD = 0xFFFF`.

Reading:

1. **The trigger registers hold what the firmware wrote** (TRG2SRC 3, RPTCNT 2, and the
   rate test rewrites RPTCNT to 16 before measuring), and the rate still does not
   follow them. In Integration mode on this silicon, the conversions inside a burst
   run back-to-back whatever TRG2SRC says; the SCCP1 period match does not pace them
   either. The datasheet text (16.4.5) is not what the board does. The remaining
   lever for the rate is the ADC clock itself (CLKGEN6 divider, `CLK6DIV`), which
   scales TAD and with it the back-to-back rate: /2 = 20 MSPS, /4 = 10 MSPS, /8 =
   5 MSPS. Not built yet.

2. **At 37.5 MSPS the DMA loses 4 % of the samples**, exactly as in runs 4 and 5
   (7.8 million overruns per 5 s of 195 000 halves x 1024 samples), and every overrun
   raises the DMA interrupt: 1.56 million per second, priority 4. The main loop gets
   4 % of the halves (`missed` 96 %), `late` stays 0. This matches Microchip's own
   example, which copies with a hand-timed 5-cycle loop instead of the DMA at this
   rate.

3. **The console receives nothing:** `rx=0` after 20 s of typing, no UART error
   flags, receiver idle with the line high (`RCIDL`), pin routing and enables as
   intended, JTAG off (RD1 is also TCK). Two explanations remain and the log cannot
   separate them: the terminal's bytes never reach RD1 (wrong COM port or a terminal
   that shows the log but does not send), or the priority-1 receive interrupt
   starves behind the 1.56 million priority-4 interrupts per second. The second one
   disappears with the rate; if `rx` stays 0 at a rate without overruns, it is the
   first.

4. `[half]` says the input is at 8...35 counts of 4096, mean 17: nothing is connected
   to mikroBUS A AN, the pin floats near ground. Expected; the signal source comes later.

Changed after run 6 (no board run yet): the ADC clock divider is the fourth pacing
candidate (`pacing 64`, `clock_adc_set_div()`: CLKGEN6 `INTDIV`, divided clock =
F_IN / (2 · INTDIV), switched with `DIVSWEN`, ratios 1/2/4/6/8/10 = 40/20/10/6.7/5/4
MSPS; 32 MHz is the ADC minimum). The rate test tries it at ratios 8 and 2 after the
repeat timer and SCCP1. The switch repeats the boot sequence with the ADC core off
(stream stopped, ADC `ON = 0`, generator `ON = 0`, divider, generator `ON = 1`, `DIVSWEN`,
`CLKRDY`, ADC `ON = 1`, `ADRDY`), never under a running core or generator. The boot sweep now ends with a decision: the highest rate whose
`process` run had overrun 0 and missed 0 becomes the measurement rate (`[sweep] using
period ...`), or `[sweep] NO CLEAN RATE` if none. The overrun interrupt cannot be
switched off on its own - DS70005591D 13.6.1 says any channel event flag raises the
channel interrupt and `DMA0CH` has enables only for HALF, DONE and MATCH - so the
storm at 40 MSPS is avoided by not running there, not by masking it. If the console
was starved by that storm, `rx` will count at the chosen rate; if it stays 0, the
bytes never reach RD1.

## 2026-09-23, evening - two defects in the SCCP1 candidate, and Microchip's mechanism added (no board run yet)

Reading Microchip's example for this ADC (github.com/microchip-pic-avr-examples/
dspic33ak-curiosity-adc-40msps, README only; the code comes through MCC): its "40 MSPS"
are eight channels of one core at 5 MSPS each, every channel in **Single Sample mode
with the SCCP1 trigger as first trigger** (MCC: "Single Sample", "SCCP1 Trigger
Event"; SCCP1 in PWM mode on CLK12 = 160 MHz, PR 31 = 5 MHz), results read by a
hand-timed assembly loop. CLK6 stays at 320 MHz. Timer pacing works on this silicon,
but through TRG1 in Single Conversion mode, not through TRG2 inside an Integration
burst.

Checking our SCCP1 candidate against the datasheet then found two defects, so runs 5
and 6 never tested SCCP1 at all:

1. `ADC_TRG2_SCCP1` was 32. Tables 16-3 and 16-4 (p1226 f.) list `100010` = 34 as
   "SCCP1 trigger"; 32 = `100000` is "PTG trigger 12". Fixed to 34.
2. `CCP1CON2` was 0, i.e. `AUXOUT = 00` = "No signal output on aux_out" (Table 26-10,
   p1818). The signal the ADC sees as "SCCP1 trigger" is that auxiliary output; in
   timer mode `AUXOUT = 01` puts the period rollover on it. Fixed.

Added: pacing source 65, `ADC_PACE_SINGLE` - Single Conversion mode, `TRG1SRC = 34`,
one conversion per SCCP1 period, DMA per conversion, no burst, no `CNT`, no restart in
the DONE interrupt; `capture_start()` starts the SCCP1, `capture_stop()` stops it.
Period in ticks of 10 ns as for 34; rate test at 20 and 5 ticks (5 and 20 MSPS); sweep
80/40/20/10/8/5/4 ticks.

Also: the clock-divider source (64) now takes the ratio in hundredths and uses the
fractional field (`FRACDIV`, 1/512 steps, Example 12-2's formula). Its sweep has two
passes: the even ratios 10, 8, 6, 4, 2, 1 (4 ... 40 MSPS), then 9, 7, 5, 3, 2.5, 1.6,
1.25 (4.4, 5.7, 8, 13.3, 16, 25, 32 MSPS). Ratios below 2 leave INTDIV at 0 with a
fraction; whether that divides or bypasses is what those rows show. The fastest clean
row of either pass wins. The AUTO order is now 65, 64, 3, 34, 2: Microchip's mechanism
first, the divider second, the two burst triggers for the record, back-to-back as the
reference. Verified in the simulator (boot cycles through all five, ping-pong PASS) and
in all three builds; not on the board.

Also (24.09., before any board run): after the sweep the boot switches the ADC core
and CLKGEN6 off (`capture_shutdown()`, `pwr=0` in `[stat]`) and only serves the
console, with a `[stat]` line every 10 s. That separates the two open questions of
run 6: if `rx` counts now, the console was starved by the overrun interrupts; if it
still stays 0 with nothing converting, the bytes never reach RD1. `start` brings
clock and core back (`clock_adc_on()`, `adc_reinit()`) at the rate the sweep chose.

## 2026-09-24 - two phases at boot, ADC core switch at run time, DAC2 as the signal source (no board run yet)

Wanted by the user: the existing tests run and log their results (phase 1, ADC 3 on the
mikroBUS input), then a second phase repeats every test with DAC2 driving ADC 5, and the
ping-pong halves are checked for the DAC signal. Built:

- `adc.c` selects the core at run time: a table row per core (registers, interrupt
  word and CH0 bit, DMA trigger code), `ADCREG()/ADCBITS()` go through `adc_cur`.
  `capture_select_core()` stops, takes the core down, switches, re-runs `adc_init()`,
  re-arms the DMA on the new trigger/result register. `core` command.
- `dac.c`: DAC2 Triangle Wave mode (Example 18-3), CLKGEN7 from PLL1 (320 MHz), 0x100 ..
  0xF00, SLPDAT 8 = 44.8 us period = 22.3 kHz (Equation 18-4). DACOUT2 = RA8 = AD5AN3 =
  DIM P44 = capacitive touch pad 2 on the EV74H48A - the loop closes on the pin, no wire.
  `dac` command.
- `dactest.c`: copies each completed half (2 KB) right after completion, judges min/max
  (150 LSb), reversals vs. period at the measured rate (10 %), jumps > 4 steps + 64.
  `dactest` command; phase 2 runs 64 halves.
- Phase 2 ends with `[DONE]` naming both phases and the DAC verdict; everything off
  afterwards (ADC, CLKGEN6, DAC2, CLKGEN7).
- `cmd_parser.h`: `CMD_PARSER_MAX_COMMANDS` 16 -> 24 (the one deliberate edit of the
  vendored parser, see CLAUDE.md).

Open until the board says: whether DACCTRL1/DAC2 come up on CLKGEN7 at 320 MHz (the
datasheet's design point is 400 MHz), whether the touch pad's network loads the DAC
buffer, and whether ADC 5 reads the pin the DAC drives (AD5AN3 shares the pad).

Also (24.09.): every test now starts from a defined state, `capture_settle()`: stream
stopped, the burst in flight finished (or aborted by taking the core down and up if it
never ends), stale CH0RDY and event flags cleared, the DMA channel taken down
(`dma0_deinit()`: interrupt masked, channel and module off, flags cleared), `ready_half`
0; the next `capture_start()` runs `dma0_init()` again from scratch. Every test ends
with the DMA off and begins with a freshly initialised one (the user's rule). Reason
(the user's question): a burst always ends at a block boundary, but the
single-conversion source stops wherever its timer is switched off, and the next test
would have started in a half-filled half - harmless for a rate, misleading for the DAC
check. Self-test, rate test, every sweep point and the DAC test call it first.

Also (24.09.): the buffer half length is a run-time value, `capture_half_len()`,
default = the allocation maximum of 1024 samples, so nothing changes unless asked.
`buf <16..1024>` (stream stopped) sets it; the next `capture_start()` sets the ADC burst
length (`adc_set_burst_len`), the DMA block and the guard words - which now sit right
behind the region in use - for that size. All consumers (stats, dump, sweep and rate
maths, DAC test, self-test mean) read the length. No heap: the array stays static at
its maximum. The simulator build takes `build.bat sim <n>` to run the ping-pong check
at another size.

## 2026-09-24, run 7 - master 1817f6d (+local changes), build 09:39:31, both phases

`RCON = EXTR`. The first run with the two boot phases. Self-test 3815 on core 3,
3824 on core 5.

**What the board confirmed for the first time.** The ADC core switch works at run
time: phase 2 reports `core: 5`, `DMA0SEL 0x3B -> 0x48` and `DMA0SRC 0x0B64 ->
0x0DA4`, so the DMA follows the core to the other trigger and the other result
register, and the self-test passes on both cores. DAC2 comes up on CLKGEN7 at
320 MHz: `DACCTRL1 = 0x3F7F8000`, `DAC2CON = 0x8100`, `DAC2DAT = 0x0F000100`
(3840/256), `DAC2SLPDAT = 8`, `CLK7CON` equal to `CLK6CON`. Two of the three
questions the 24.09. entry left open are answered; the third - whether ADC 5 reads
the pin the DAC drives - is not, because the DAC test never printed a verdict (below).

**All four paced sources failed, in two different ways.**

1. Both SCCP1 paths, `ADC_PACE_SINGLE` (65) and `ADC_TRG2_SCCP1` (34), printed
   `no data at period: 20` - not a wrong rate, no conversion at all. After the
   23.09. fixes (code 34 instead of 32, `AUXOUT = 01`) that points at SCCP1 still
   emitting nothing, and the log cannot say more: the register snapshot had no SCCP
   block at all.
2. The clock divider (64) at ratio 8 (`period 800`, nominal 5000 ksps) delivered
   37 350 ksps, and the repeat timer (3) at RPTCNT 16 delivered 36 028 ksps - both
   the unpaced rate. `CLK6DIV` was 0x0000 in the phase-1 snapshot and 0x8000 in the
   phase-2 one (`INTDIV 0, FRACDIV 256` = the ratio 1 written back after the test),
   so writes do reach the register; what the log does not show is the value that
   stood there *while* the divider was measured. `rate_measure()` discarded the
   return of `capture_set_period()`, so a divider switch that failed
   (`DIVSWEN`/`CLKRDY` never coming) produced a normal looking row.

So `[pacing] using: back-to-back - NO PACED SOURCE PASSED, the rate is not under
control`, and the sweep has one row.

**The measurement, now confirmed a fourth time and in two independent runs.**
Back-to-back 38 167 ksps, overrun 83 083 of 2 048 000 samples = **4.06 %**, missed
1969 of 2000, late 0. Phase 1 and phase 2 printed these figures **digit for digit
identical** - with a floating mikroBUS pin in phase 1 and the DAC driving the pin in
phase 2. The loss is therefore set by the clock alone and has nothing to do with the
signal: at ~38 MSPS the DMA does not keep up. That is the number this example exists
to produce.

**The run stops inside the DAC test, silently.** The log ends after
`[dactest]   jump limit LSb: 76`, the last of the header lines; no verdict, no
`[PHASE 2 DONE]`, no `[DONE]`, no END banner, no boot banner, no `[TRAP]` block. The
colleague repeated the run with a second terminal program (TeraTerm, then Hercules)
and it stopped at the same line both times, with `help` having no effect afterwards -
so it is the board, not the copy. Between that line and the next output there are
only `capture_settle()`, `counters_clear()`, `capture_start()` and the collecting
loop, and **every wait on that path is bounded**: `capture_settle()` waits at most
`WAIT_LIMIT` (~30 ms) and then takes the core down and up, `adc_reinit()` bounds
ADRDY, `dma0_deinit()` has no loop, and the loop itself leaves with `[dactest] no
data` (6) or `DMA channel switched itself off` (8). An ordinary wait can therefore
not be the cause.

The one mechanism that fits every observation: the DMA interrupt runs at priority 4
and fires ~1.56 million times a second at 38 MSPS (every 640 ns), U2RX at priority 1
sits below it. If the handler takes longer than the gap between events, the CPU never
reaches the main loop again - no output, no trap, no reset, and the console deaf,
which is also what `rx = 0` in run 6 was. Against it: the sweep, which runs the same
start/stop sequence, completed. Unproven either way.

Changed in reaction (no board run yet):

- `dactest.c` traces the four steps and flushes after each line
  (`settling`, `settled`, `starting the stream`, `stream started, collecting`,
  `first half copied`), so the next log names the call that swallows the CPU.
  Flushed because an unflushed line sits in the transmit FIFO when the CPU stops.
- `sccp.c/.h`: `sccp1_regs_dump()` - `CCP1CON1`, `CCP1CON2`, `CCP1PR` and `CCP1TMR`
  read twice. Both reads 0 means the timer never started and no aux-out pulse can
  reach the ADC. It is in the boot snapshot (`diag.c`) and in the rate test, in the
  `no data` branch as well - which is the only witness when nothing converts.
- `capture.c`: `pacing_readback()` prints, next to each measured row, what the
  hardware holds - divide ratio and ADC clock for 64, RPTCNT and TRG2SRC for 3, the
  SCCP registers for 34/65. `rate_measure()` now reports a refused period (code 13)
  instead of measuring anyway.
- `console.h` includes `<stdbool.h>`; it declares `console_sweep(uint32_t, bool)`
  and was not self-contained, which only showed when `sccp.c` included it.

## 2026-09-24, after run 7 - back-to-back only, nothing runs by itself (no board run yet)

The user's decision after run 7: concentrate on back-to-back, remove everything else from
the code, and let the firmware do nothing until someone types a command.

**Removed.** `sccp.c/.h`; the pacing sources 3 (ADC repeat timer), 34 (SCCP1 as second
trigger) and 65 (one conversion per SCCP1 trigger); `capture_autopace()`, the pacing
selection and the whole `pacing`/`period` mechanism including the ISR path that applied a
period between two bursts; `ADC_PACING`, `ADC_RPTCNT`, `ADC_SCCP_TICKS` and `AUTO_SWEEP`
in `board.h`; the `rptcnt` argument of `adc_init()` and the `adc_set_period`/`adc_trg2`/
`adc_set_mode_single` helpers; the two automatic boot phases with their register
snapshots. `TRG2SRC` is wired to 2 and nothing writes it again.

Why: all four were configured correctly, read back correctly and ignored (runs 4 to 7).
Keeping them meant every log carried four failing blocks before the one measurement that
works.

**The rate is now the ADC clock alone.** `clock_adc_set_div()` returns `CLKDIV_OK` or the
step that failed - `CLKDIV_NOT_WRITTEN` (the write did not reach CLK6DIV),
`CLKDIV_DIVSWEN` (the switch never completed), `CLKDIV_CLKRDY`, `CLKDIV_LOST` (the value
was discarded over the switch), `CLKDIV_ADC` (the core did not come back). The fields are
read back before and after the switch. `capture_set_clkdiv()` does the sequence the user
asked for and it is the boot order run backwards and forwards again: DMA channel down and
burst finished (`capture_settle`), ADC core off (`adc_deinit`), CLKGEN6 off, divider
written and read back, generator on, `DIVSWEN`, `CLKRDY`, fields read back, core on with
`ADRDY` (`adc_reinit`), DMA set up from scratch on the next `capture_start()`. Run 7 could
not distinguish a divider that never switched from one that switched without effect,
because `rate_measure()` discarded the return value of `capture_set_period()`.

Also corrected: the comment in `clock_adc_set_div()` claimed ratio 1 was `INTDIV 0,
FRACDIV 0`. The arithmetic in the same block gives `INTDIV 0, FRACDIV 256`, which is what
the phase-2 snapshot of run 7 showed (`CLK6DIV = 0x8000`).

**Nothing runs at boot.** The firmware brings LED, console, clocks, ADC core and DMA
channel up, sets the slowest ratio (`ADC_CLKDIV` = 1000 = /10 = 32 MHz = 4 MSPS), prints
about a dozen lines and waits. No conversion is triggered, so no DMA event and no
interrupt can come from the ADC side. That is what settles `rx = 0`: with nothing
converting, a character that does not echo cannot be blamed on interrupt starvation.

**`test` runs the parts.** `test` alone lists them; `test all [halves]` runs self, clock,
sweep, dac in that order and prints a four-line verdict at the end. Only `test self`
failing stops `all` - without a working chain every number after it is meaningless.

- `test self`  - the self-test on the internal reference at the slowest clock.
- `test clock` - every ratio of the ladder switched and read back, **nothing measured**.
  This is the test that separates "the switch does not happen" from "the switch happens
  and the rate does not follow".
- `test rate [halves]` - delivered rate at the ratio set now, judged against the nominal
  one within 10 %.
- `test sweep [halves]` - the ladder with overrun/late/missed per point.
- `test dac [halves]` - the DAC2 triangle through the chain.

Plus `clk <100..1000>` to set a single ratio by hand, and `regs` as before.

**The sweep runs from the slowest rate upwards** (the user's decision), because the
slowest point is the one the DMA should manage: the first row is the row most likely to
pass, and a failure there is the chain, not the rate. The ladder is
1000, 900, 800, 700, 600, **500**, 450, 400, 350, 300, 250, 200, 150, 125, 100 - that is
4.00, 4.44, 5.00, 5.71, 6.67, **8.00**, 8.89, 10.0, 11.4, 13.3, 16.0, 20.0, 26.7, 32.0,
40.0 MSPS. 500 = 8 MSPS is in it because that is the customer's floor. The fractional
ratios are there for a second reason: 2.5 sits exactly between 2 and 3, and 4.5 between
4 and 5, so a row that lands halfway between its neighbours proves `FRACDIV` works and a
row that snaps to a neighbour proves only `INTDIV` counts. Ratio 1 is `FRACDIV 256`, so
that question is not academic.

**Still in, unchanged in purpose:** the self-test, the counters, the guard words, the trap
handler and the `RCON` report, `dac.c`/`dactest.c` (the only way to show that the samples
arrive complete and in order - counters cannot), and the DAC test's step trace from
earlier today.

Verified: both builds (`build.bat` and `build.bat sim`) are `-Wall -Wextra` clean. The
simulator acceptance run was not made - it is run on request now. **Nothing of this has
been on the board.**

The expected log is much shorter than run 7's: the boot prints about a dozen lines
instead of two full register snapshots and five rate-test blocks per phase, and the
register dump is a command (`regs`) rather than an automatism.

## 2026-09-24, run 8 - master afc5e00 (+local changes), build 11:31:46 - THE CONSOLE WORKS

`RCON = EXTR`. The first run of the reworked firmware, and the first run in which anyone
typed anything at the board.

**The console receives.** `help` answered with the full command list, `test all` started.
That closes the question the last three runs left open: **the bytes do reach RD1, and
`rx = 0` in runs 6 and 7 was the receive interrupt (priority 1) being starved behind the
DMA interrupt (priority 4, 1.6 million per second at full rate)**. Pin routing, PPS,
terminal and COM port were never the problem. The idle boot was worth it on its own: from
here on experiments cost a command, not a build and a colleague.

**Self-test 3829** on the internal reference at the slowest clock setting. PASS.

**`test clock` passed all fifteen ratios** - every one written, read back identically,
`DIVSWEN` cleared, `CLKRDY` came, the ADC core came back with `ADRDY`. And that turned out
to be a **false positive**: it proves the register holds the value, nothing more.

**The sweep showed the rate does not follow the divider at all.** Twelve rows before the
log was cut, every one of them at the full rate:

```
ratio 1000 (read back 1000)  ksps nominal 4000  measured 39729   overrun 73157/80633/77511  missed 1735
ratio  900 (read back  900)  ksps nominal 4444  measured 39818   overrun 72663/79459/77114  missed 1705
ratio  800 (read back  800)  ksps nominal 5000  measured 39693   overrun 74019/89791/76741  missed 1929
ratio  700 (read back  700)  ksps nominal 5714  measured 40675   overrun 66681/89308/75017  missed 1925
ratio  600 (read back  600)  ksps nominal 6666  measured 39248   overrun 76987/76983/76727  missed 1568
ratio  500 (read back  500)  ksps nominal 8000  measured 39311   overrun 77307/84700/73206  missed 1754
ratio  450 (read back  450)  ksps nominal 8888  measured 40356   overrun 69259/69892/69215  missed 1930
ratio  400 (read back  400)  ksps nominal 10000 measured 40338   overrun 69762/86856/76987  missed 1802
ratio  350 (read back  350)  ksps nominal 11428 measured 40390   overrun 67316/67767/67359  missed 1921
ratio  300 (read back  300)  ksps nominal 13333 measured 40761   overrun 65538/77950/77035  missed 1346
ratio  250 (read back  250)  ksps nominal 16000 measured 40433  overrun 64324/64459/63702  missed 1913
ratio  200 (read back  200)  ksps nominal 20000 measured 41119   overrun 61112/90055/77496  missed 1552
```

Timer check 1250001 of 1250000, so the stopwatch is right. Asked for 4 MSPS, got 39.7.
The scatter between rows is about 5 % and does not correlate with the ratio - it is run to
run noise, not a trend. Overrun stays at 3 to 4.5 % of 2 048 000 samples throughout, and
`missed` at 1300 to 1930 of 2000, exactly as in runs 4 to 7.

**The cause, found in the datasheet afterwards (12.4.2 step 4 and Example 12-2, p771).**
The documented procedure changes the divider **with the clock generator running**:

```
CLK6CONbits.ON = 1;                  // the generator is ON throughout
CLK6DIVbits.INTDIV  = 1;             // 4a: integer factor
CLK6DIVbits.FRACDIV = 128;           // 4b: fraction
CLK6CONbits.DIVSWEN = 1;             // 4c: apply
while (CLK6CONbits.DIVSWEN != 0);
```

Our sequence switched CLKGEN6 **off** first (`ON = 0`), wrote the divider, switched it back
on and then set `DIVSWEN`. The bit cleared, `CLKRDY` came, the register kept the value -
and the divide factor was never taken over. Fixed: the generator stays on, `INTDIV` is
written before `FRACDIV`, then `DIVSWEN`. The ADC core and the DMA channel are still taken
down by the caller; they are what needs protecting, the generator is not.

**Second finding in the same paragraph:** "FRACDIV will not work if INTDIV is configured
to 0" (12.4.2 4b). INTDIV is ratio/2, so **no ratio between 1 and 2 can be realised** -
`FRACDIV` alone does nothing and the clock comes out undivided. Ratios 1.25 and 1.5 are
therefore out of the ladder, 20 MSPS is the fastest divided rate, and the step above it is
the undivided 40 MSPS. `clock_adc_set_div()` now refuses 101..199 with `CLKDIV_INTDIV0`
instead of silently running at full speed. Ratio 1 is written as both fields 0.

**Not known yet:** the log ends inside the sweep, so there is no DAC test result and no
verdict block. The three remaining rows (150, 125, 100) are gone from the ladder anyway.

Nothing of the fix has been on the board.

**Why the run stops dead, and the brake against it.** After the ratio-200 row the log ends
mid-line and the parser stops answering - the same picture as run 7's DAC test. The
explanation that fits everything: at the undivided rate the overrun interrupt fires every
625 ns, which is 125 CPU cycles at 200 MHz, and an interrupt entry with its context save
plus the handler costs about the same. The CPU sits exactly on the edge of never returning
to the main loop, and twice it fell off. No trap, no reset, no banner, because the CPU is
executing valid code - it just never leaves the handler; and the console dies with it
because U2RX is priority 1 and the DMA channel 4. Run 6 shows the same effect one step
weaker: `missed` 96 %, so the main loop still got 4 % of the halves.

The overrun event cannot be masked on its own (13.6.1; `DMA0CH` has HALFEN, DONEEN and
MATCHEN only). So the handler stops itself: past `OVERRUN_LIMIT` (500 000) in one
measurement it masks its own interrupt, takes the channel down and ends the stream
(`capture_overrun_aborted()`). A healthy full-rate point produces about 82 000 overruns per
2000 halves, so the limit never fires in normal use; a runaway reaches it within a third of
a second. The sweep row, the rate test and the DAC test all report it instead of the board
going quiet. `counters_clear()` re-arms it, and every test calls that first.

This does not make a flooding rate usable - it makes it survivable, so the run continues
and the log gets written. The cure for the flood itself is the divider fix above.

## 2026-09-24, run 9 - master 38e7a0c, build 12:23:31 - the brake holds, the divider still does nothing

**The brake works and the board stays alive.** The whole `test all` ran to the end, and
afterwards the console still answered - `help`, `test`, `test clock`, `test dac` all worked.
At the undivided ratio all three loads aborted with STOPPED, which is the brake doing its
job. No freeze, for the first time since run 6.

**But the brake stays tripped - a defect, fixed after this run.** It compared the
cumulative `dma_overrun`, which only `counters_clear()` resets, so once it had fired the
very first overrun of every later test tripped it again: `test self` reported "DMA channel
disabled" although everything had just worked. It has its own counter now, zeroed at every
`capture_start()`, and it also clears `dma_armed` so the next start re-initialises the
channel.

**The divider still changes nothing.** All thirteen rows measured 39 750 to 40 791 ksps -
ratio 1000 (4 MSPS asked for) and ratio 200 (20 MSPS asked for) alike:

```
postdiv/ratio 1000  nominal  4000  measured 39750   overrun 78730/187413/75638  missed 1960
              900   nominal  4444  measured 40418   overrun 71734/78695/73985   missed 35
              800   nominal  5000  measured 39797   overrun 78725/130000/78725  missed 15
              700   nominal  5714  measured 40364   overrun 72426/159177/74830  missed 13
              600   nominal  6666  measured 40781   overrun 69520/115816/79235  missed 11
              500   nominal  8000  measured 40043   overrun 73070/142702/72706  missed 9
              450   nominal  8888  measured 40706   overrun 69718/71807/69937   missed 1944
              400   nominal 10000  measured 40663   overrun 68689/166827/68704  missed 7
              350   nominal 11428  measured 40704   overrun 68271/68770/68318   missed 1924
              300   nominal 13333  measured     0   overrun STOPPED/241619/62881
              250   nominal 16000  measured 40791   overrun 65440/66444/64961   missed 1940
              200   nominal 20000  measured 40712   overrun 66631/STOPPED/62008 missed 47
              100   nominal 40000  measured     0   overrun STOPPED/STOPPED/STOPPED
```

That is now the second switching sequence with the same result. Run 8 switched CLKGEN6 off
around the write, run 9 left it running exactly as Example 12-2 prescribes; in both the
register took the value, `DIVSWEN` cleared, `CLKRDY` came, and the ADC converted at 40 MSPS
throughout. **The conclusion is no longer "we switch it wrongly" but "the CLKGEN6 divider
does not set the ADC conversion rate on this silicon."**

Worth noting in the table: `missed` is bimodal - either about 1930 of 2000 or under 50 -
and the rows where the main loop kept up are the rows where the `process` run shows roughly
twice the overruns. Both follow from the CPU sitting on the tipping point of the interrupt
load: if it tips, the main loop gets nothing; if it does not, the processing itself costs
bus cycles and pushes the overrun count up. It is not a property of the rate, which never
changed.

**Changed in reaction (no board run yet): the rate comes from PLL1 now.**

PLL1 feeds nothing but the ADC path - the CPU runs off PLL2 - so it can be retuned freely.
FVCO is 1600 MHz and the output is FVCO / (POSTDIV1 * POSTDIV2), both fields 1..7 with
POSTDIV1 >= POSTDIV2 (p778). The ladder, slowest first:

```
  7/7   32.65 MHz    4.08 MSPS   slowest that still clears the ADC minimum of 32 MHz
  7/6   38.10 MHz    4.76 MSPS
  6/6   44.44 MHz    5.56 MSPS
  7/5   45.71 MHz    5.71 MSPS
  6/5   53.33 MHz    6.67 MSPS
  7/4   57.14 MHz    7.14 MSPS
  5/5   64.00 MHz    8.00 MSPS   the customer's floor
  6/4   66.67 MHz    8.33 MSPS
  5/4   80.00 MHz   10.00 MSPS
  6/3   88.89 MHz   11.11 MSPS
  5/3  106.67 MHz   13.33 MSPS
  6/2  133.33 MHz   16.67 MSPS
  5/2  160.00 MHz   20.00 MSPS
  5/1  320.00 MHz   40.00 MSPS   the boot setting of clock_init()
```

The decisive argument for this route: **the PLL's output-divider switch is the one
`clock_init()` performs at every boot.** If it did not work the board would not come up at
all, so unlike the CLKGEN6 divider it is known to work on this silicon. The sequence is the
same as before - DMA channel down, ADC core off (p778: the output dividers must not move
while the PLL is operating), PLL1DIV written and read back, FOUTSWEN awaited, PLL1RDY
awaited, CLKGEN6 CLKRDY awaited, core on, DMA from scratch.

`clock_adc_hz()` is derived from the registers now (PLLPRE, PLLFBDIV, POSTDIV1, POSTDIV2,
then the CLKGEN6 ratio) instead of assuming 320 MHz, so a switch that did not take shows up
in the number instead of being papered over. `clock_dac_hz()` uses the same PLL output -
CLKGEN7 hangs off PLL1 too, so retuning for the sample rate moves the DAC's triangle with
it, and `dactest` reads the period back live.

**New: `test clkoff`.** Table 16-1 names CLKGEN6 as the ADC clock source, and its divider
has no effect - so ask the board directly: take the core down, switch CLKGEN6 **off**,
bring the core back and try to convert. If halves still arrive, the ADC is not running off
that generator and the last two runs are explained at a stroke. If nothing arrives, the
generator does feed it and the mystery stays with the divider. Either way it is one command
and a few milliseconds. It runs as part of `test all`, after `test clock`.

`test clock` keeps the CLKGEN6 ratios and now prints, under its PASS, that passing there
only proves the register holds the value.

Boot default is the slowest PLL setting (7/7), for the same reason the ladder starts there.
New commands: `pll <p1> <p2>` for the rate, `clk` kept for the CLKGEN6 ratio with its
warning. Both builds `-Wall -Wextra` clean; nothing of this has been on the board.

## 2026-09-24, run 10 - master 33118cc, build 12:41:01 - the ADC ignores its clock entirely

Boot at PLL 7/7: `adc clock Hz 32653061`, `sample rate ksps 4081`. So the PLL switch itself
does arrive - the registers report the clock the ladder asks for. Self-test 3832, PASS.

**`test clkoff`: the ADC kept converting with CLKGEN6 switched off.** 260 halves after the
generator was taken down, and the core still reported ADRDY. Table 16-1 names CLKGEN6 as the
ADC clock source; the board disagrees.

**And the PLL does not set the rate either.** Fourteen rows, the read-back clock rising
cleanly from 32.65 to 320 MHz - a factor of ten - and the measured rate flat at 41 375 to
44 231 ksps throughout:

```
postdiv 7/7   32.65 MHz   nominal  4081   measured 42516   overrun 56567/58489/57239
postdiv 6/5   53.33 MHz   nominal  6666   measured 42408   overrun 54695/55042/54483
postdiv 5/5   64.00 MHz   nominal  8000   measured 42242   overrun 53602/53599/53595
postdiv 5/2  160.00 MHz   nominal 20000   measured 43334   overrun 42753/43211/43085
postdiv 5/1  320.00 MHz   nominal 40000   measured 44231   overrun 32314/32625/32076
```

Note what the measured column is: **above the datasheet's 40 MSPS** at every setting, with a
weak upward trend that follows the PLL by 4 % while the nominal rate spans a factor of ten.
The overrun count falls as the PLL goes up, from 56 567 to 32 314.

**The reading this forces, and it is bigger than a rate problem.** `blocks_done` counts DMA
half-completions, not conversions. If the ADC's result-ready event stays asserted, the DMA
copies the same result register over and over at its own speed - which would be independent
of every ADC clock setting, would come out above the converter's specified maximum because
it is not the converter's rate, would not stop when CLKGEN6 is switched off, and would show
exactly this weak coupling to the PLL through the register interface.

**The self-test cannot tell the difference.** It samples a DC reference, so a frozen register
gives the same mean of 3832 that real conversions do. It has never proved that anything is
converted - only that a plausible value lands in the buffer.

**The DAC test is therefore the decisive measurement of this project**, and it has never once
run correctly: in run 7 it stopped silently, in runs 9 and 10 the brake stopped it, and in
run 10 it ran on **ADC core 3** while DACOUT2 is an input of core 5 - it was measuring an open
pin. Rebuilt for this, no board run yet:

- **The DAC is measured inside the chip.** `UREFCON.INSEL` puts one of DAC1..DAC8 on the
  device's internal UREF line (ATDF value group `UREFCON_CON__INSEL`: 1 AVDD/2, 2 VDD/2,
  3 VDDcore, 4 bandgap, 5 temperature sensor, 6..13 DAC1..DAC8, 14 AVSS, 15 AVDD), and
  `ADnAN7` is the UREF input of **every** core (Table 16-2). So the test routes DAC2 to UREF
  and samples AN7 on whatever core is in use: no pin, no wire, no core switch, and none of
  the loading the board's touch-pad network puts on RA8. `uref_route_dac2()` in dac.c,
  `UREFCON` added to the register dump. The pin route is documented in board.h for the
  record: DACOUT1 = AD5AN1 = RA1 (shared with PGC2), DACOUT2 = AD5AN3 = RA8, both on core 5.
- **Capture first, analyse afterwards.** Eight halves are copied into RAM and nothing is
  computed while the stream runs; the judgement comes after the stream is down. The old
  version analysed each half as it arrived, which under the overrun storm took long enough
  for half a million overruns to pile up - that is why the brake stopped it after the first
  half in run 10. A memcpy of 2 KB is about a thousand cycles and fits in the 24 us a half
  lasts even with the storm stealing most of the CPU. 16 KB of the 64 KB go to the store;
  the firmware now uses about 21 KB in total.
- **The verdict is about data, not rate.** Peak-to-peak over the whole capture is the
  question: a frozen register gives 0, a real ramp some thousand counts. Eight halves are
  8192 samples, about 200 us at the rates seen, and one slope of the triangle lasts 220 us -
  so a clean monotonic ramp of roughly 1600 counts must appear. Slope reversals show whether
  the samples are in order, and gaps count halves that completed but were not copied, which
  would break the continuity the shape is judged on. A few dozen raw values are printed, so
  the ramp can be seen in the log instead of trusted from the arithmetic.

If that capture shows a ramp, the chain is proven - conversions are real, complete and in
order - and only the rate is open. If it shows a flat line, the DMA is moving a stale
register and every rate measured in runs 4 to 10 is void.

Worth taking to the product line either way: an ADC that ignores both its clock generator's
divider and its PLL's output dividers, converts with the generator switched off, and delivers
above its specified maximum is a question for the factory, and the register evidence for it
is now complete.

## 2026-09-24, run 11 - master 928867d, build 13:03:59 - THE ADC REALLY CONVERTS

`dac on`, then `test dac`. The DAC2 triangle routed to the internal UREF line and sampled as
AN7 on core 3 - no pin, no wire, no core switch.

**The question this project has been circling since run 4 is answered: the conversions are
real.**

```
min 221   max 3864   peak-to-peak 3643
```

The DAC was set to 256..3840. The buffer holds the full DAC range, so the ADC converts a
real signal and the DMA moves real results. The stale-register hypothesis raised after run
10 is dead, and the internal UREF path works.

**What the run could not answer is order and completeness, and the reason is in the numbers:**

```
halves missed between copies (gaps): 8552   for eight copies
slope reversals: 430
largest step between two samples: 3126
overrun during the capture: 342508
```

Between two memcpys about a thousand halves completed. At the full rate the main loop runs
tens of milliseconds behind the DMA, so the half being copied had been overwritten a thousand
times over: the copy is a mixture of old and new data. That is visible in the raw dump, which
alternates between two plateaus of about 3100 and 1470 with the step always a few samples
into the half - the seam between what the DMA had already rewritten and what was left from
before. Not signal, and not a converter fault: a torn read.

`ksps measured` printed 0, which only says the window was far longer than the sample count
suggests - the same starvation seen from the other side.

**Changed in reaction (no board run yet): one buffer, and the DMA interrupt stops the stream.**

`capture_oneshot()` fills the buffer exactly once and the ISR ends the run at DONE instead of
restarting the burst. The main loop being slow no longer matters - it only has to notice,
eventually, that the burst is over. Afterwards the whole buffer is one contiguous window that
nothing is writing any more, so there are no gaps to count and no torn halves by construction:
the ADC burst is CNT = 2 * half_len, which is exactly one buffer, with HALF at the middle and
DONE at the end.

2048 samples are about 48 us at the rates seen, and one slope of the triangle lasts 220 us at
the boot clock, so the window covers roughly a fifth of a slope: a monotonic ramp of some 700
counts, with at most one turning point if it straddles a peak. `dac on <slpdat>` with a
smaller slpdat makes the triangle faster and the ramp steeper - slpdat 2 gives about a quarter
of a period per buffer.

The verdict is now three things a torn window cannot fake: peak-to-peak (a frozen register
gives 0), at most one slope reversal, and no step larger than 200 counts between neighbouring
samples - the triangle moves well under one count per sample at these rates, so a jump of
hundreds is a missing sample or a seam. The store shrank from 16 KB to 4 KB with it.

Both builds `-Wall -Wextra` clean.

## 2026-09-24, run 12 - master 528b370, build 13:10:35 - a PASS that does not hold

`dac on`, `test dac`. The one-shot capture works: one contiguous window of 2048 samples, no
gaps, no torn halves, and the test printed PASS. The PASS is not trustworthy, for three
reasons, and all three are fixed.

```
window ticks (12.5 MHz): 0
min 686   max 758   peak-to-peak 72   largest step 49   slope reversals 0
overrun during the burst: 664
first 24 samples: 740 x17, then 747 x7
every 64th: 740, 724, 695, 686, 717, 717, 717, ... 717
```

**The reversal check was vacuous.** The hysteresis was a fixed 96 counts and the window moved
72, so the detector never armed: "reversals 0" said only that the signal was smaller than the
threshold. It is derived from the measured peak-to-peak now, with a floor of 8.

**Timer1 was never running.** `timebase_init()` was called only from `timebase_check()`, which
only the sweep calls - a run that types `test dac` and nothing else has no clock, hence zero
ticks. It is started at boot now.

**And the window barely moved.** At the DAC settings the triangle should cover about 800
counts in 2048 samples; it covered 72. The raw values show it plainly: 740, a short settle
down to 686, then 717 for the remaining 1800 samples. Nearly constant, not a ramp.

What does hold: the values change smoothly, in steps of at most 49, with no jumps. A frozen
result register would give a peak-to-peak of zero. **The ADC converts and the DMA places the
results in order** - it is what the ADC sees that does not follow the triangle the way the
arithmetic expects.

Two possibilities remain and they are now separable: either the sample rate is not what the
window length says, or the DAC clock is not what its registers say. The test computes both
sides independently - window length from Timer1, slope rate from the DAC registers - and
prints how far the triangle should have moved next to how far it did. A window that stands
still cannot pass any more. The same run now also yields the first uncontended rate
measurement: one burst, no interrupt storm, stopped with Timer1. Every rate in runs 4 to 11
came from a CPU that was drowning in interrupts.

Next at the board: `dac on 64` then `test dac`. SLPDAT is the step per DAC clock, not the
period - larger is faster - so 64 shortens the period from 439 us to 55 us and one buffer
covers nearly two full periods instead of a fifth of one slope. Then a triangle has to be
visible in the raw dump by eye.

## 2026-09-24, run 13 - master b57e310, build 13:22:59 - THE CHAIN IS PROVEN

`dac on 64`, `test dac`. The raw dump is the result this project exists for:

```
2418 2541 2603 2714 2816 2960 3044 3128 3260 3386 3496 3581 3706 3789 3851
3688 3581 3512 3397 3281 3195 3063 2998 2861 2755 2666 2539 2416
```

A triangle. Clean rise to 3851, clean fall to 2416, one turning point, largest step between
two neighbouring samples 113 counts out of a swing of 1440, no jump anywhere, no gap.

**The ADC converts a real changing signal and the DMA places every result in the ping-pong
buffer, complete and in the order it was converted.** That is the statement the customer
needs, and it is now on silicon.

**Second finding, and it overturns eleven runs of measurements:**

```
window ns 513280   sample rate ksps in this burst: 3990   (nominal 4081)
```

2.2 % off the setting. **The PLL rate control works.** The 42 to 44 MSPS that every sweep from
run 8 onwards reported at every setting are an artefact of measuring while the CPU drowns in
the overrun interrupt. A single clean burst with Timer1 around it says something completely
different - and it says the ADC follows the PLL.

**The FAIL was our own yardstick.** The test compared the swing against a triangle period
computed from the DAC registers, 54.9 us at slpdat 64. The capture contains exactly one period
in 513 us, so the real period is about 449 us - a factor of eight out. And the triangle runs
between 2416 and 3851: the upper end matches `DACDAT` 3840 exactly, the lower end has nothing
to do with `DACLOW` 256. Both are properties of the DAC that `dac2_period_ns()` models wrongly,
and a chain that works must not fail on them.

Changed in reaction (no board run yet):

- **The DAC test judges from the data.** The period is derived from the distance between
  turning points and printed next to the computed one, which is marked as not matching this
  hardware and is not judged against. What is judged is what the test is for: a changing
  signal (peak-to-peak above a floor) whose steps between neighbouring samples stay below an
  eighth of the swing. Relative, so it holds at any rate and any triangle speed - the absolute
  limits only ever fitted one setting. Run 13's numbers pass it: step limit 180, largest step
  113.
- **The sweep measures its rate on a clean single burst.** Each row now runs one `capture_
  oneshot()` with Timer1 around it before the three loaded runs, and prints `clean` next to
  `loaded`. The difference between the two columns is the artefact itself, so both stay in the
  table.

Both builds `-Wall -Wextra` clean.

## 2026-09-24, the SCCP path was never actually tested - three errors, all ours

Two AI analyses of Microchip's internal support cases, plus the device pack's ATDF, between
them explain every SCCP failure in runs 5 to 7. The path was never a test of the hardware.

**1. The trigger number selected the wrong module.** Datasheet Table 16-4 lists `100010` = 34
as "SCCP1 trigger" and 32 as "PTG trigger 12", and on 23.09. this code was "fixed" from 32 to
34 on the strength of it. The ATDF names what each number selects (value group
`AD_CH_CON1__TRG1SRC`): `0x20` = 32 is **"SCCP1 OC/IC Event"**, 0x21 SCCP2, **0x22 = 34 is
SCCP3**, and PTG is 0x1e = 30. So the ADC was told to listen to SCCP3 while SCCP1 was being
configured. The original 32 was right; the "fix" was the regression.

**2. The auxiliary output carried the wrong signal.** ATDF value group
`CCP_CCP1CON2__AUXOUT`: 0 disabled, **1 = "Timer Base Reset/Rollover"** - what this code used -
**2 = "Special Event Trigger" / "OC Event"**, 3 = no output / OC signal. Microchip's knowledge
base says the same in words: the special event trigger is what the ADC listens for, the
rollover is not. And it is available in timer mode, so the mode was never the problem.

**3. The trigger module ran off the wrong clock.** `CCP1CON1.CLKSEL` has two values: 0 is the
standard-speed peripheral clock, which comes from PLL2, and **1 is Clock Generator 13**. The
ADC runs off PLL1. From a support case: "ADC triggers go through synchronizers. If the trigger
source is clocked from a different clock source than the ADC, trigger timing can be jittery.
To avoid this the ADC trigger source module must be clocked from the same clock source used
for ADC." The same case describes a working setup with CLKGEN6 and CLKGEN13 both on PLL1.

Any one of the three was enough for "no conversion at all".

**Also from the support cases, and it answers the customer's question:**

> "The total all DMAs transfers rate is 33.3MHz (transfers per second). It means that
> interleaving of DMAs will not help."
> "CPU is only way to store the 40MSPS ADC result."
> "20 MSPS confirmed PASS." / "Stable PASS was achieved only at effective 20 MSPS interleave."
> "ADC triggers for DMA on this device have an issue. A few transfers are possible per one
> trigger. We are working to fix this problem in the next device revision."

One of those cases is titled "DMA cannot keep up with 40msps ADC - DSPIC33AK512MPS512", the
same device. 8 MSPS sits at a quarter of the stated ceiling and below a rate others have
confirmed stable. The trigger issue is a caveat that belongs in any customer statement.

**Built in reaction: `test matrix` (no board run yet).**

Every documented way to set the rate, asked of the board rather than reasoned about, each at
three rate points (4, 8 and 20 MSPS):

```
  back-to-back, rate from PLL1
  SCCP1 timer + special event, peripheral clock      | the 2x2 over the two
  SCCP1 timer + special event, CLKGEN13              | suspected mistakes:
  SCCP1 output compare, peripheral clock             | event type and clock
  SCCP1 output compare, CLKGEN13                     | source
  SCCP1 as in runs 5-7 (trigger 34, rollover)   - must fail, confirms the diagnosis
  SCCP1 special event as TRG2 inside a burst
  ADC repeat timer (RPTCNT)                     - for the record
  oversampling, ACCNUM divides the event rate
  CLKGEN6 divider                               - for the record
```

Four questions per variant, each with an instrument that has earned trust: does it convert at
all (bounded, so a dead variant costs milliseconds); does the rate follow, measured on **one
clean burst** with Timer1 and never under load; how many **DMA transfers per trigger**
(2048 / (window / trigger period) - the direct measurement of the acknowledged issue); and are
the data intact, from the DAC triangle, run only for the variants that got that far so the log
stays readable. A summary table at the end names what passed.

New: `sccp.c/.h` with clock source, mode and event as parameters rather than fixed values -
the documentation has been wrong about this module twice, so the board decides. `clock.c`
gains CLKGEN13 on PLL1 and a derived peripheral-clock figure. `adc.c` gains the channel modes
the matrix needs back (single conversion, oversampling, TRG2 and RPTCNT setters).

Both builds `-Wall -Wextra` clean; RAM about 9 KB of 64 KB.

## 2026-09-24, run 14 - master 7a9331a, build 18:20:43 - THE RATE FOLLOWS THE SETTING

`test all`. Self-test 3825, DAC test PASS, and the sweep answers the question the project
was built for.

**The rate control works across the whole ladder.** The `clean` column - one burst, nothing
else running, timed with Timer1 - follows the setting over a factor of ten:

```
postdiv  adc clock Hz  nominal   clean    postdiv  adc clock Hz  nominal   clean
  7/7      32653061      4081     3991      5/4       80000000    10000     9492
  7/6      38095238      4761     4641      6/3       88888888    11111    10474
  6/6      44444444      5555     5392      5/3      106666666    13333    12421
  7/5      45714285      5714     5537      6/2      133333333    16666    15229
  6/5      53333333      6666     6433      5/2      160000000    20000    18053
  7/4      57142857      7142     6876      5/1      320000000    40000    32862
  5/5      64000000      8000     7660
  6/4      66666666      8333     7965
```

**The shortfall is ours, and it is a constant.** It grows from 2.2 % at the bottom to 17.8 %
at the top, which looks like a rate-dependent error and is not. Converted to window
durations, every row shows the same offset:

```
  7/7   should 501.8 us   measured 513.2 us   +11.4 us
  5/5   should 256.0 us   measured 267.4 us   +11.4 us
  5/2   should 102.4 us   measured 113.4 us   +11.0 us
  5/1   should  51.2 us   measured  62.3 us   +11.1 us
```

Eleven microseconds, independent of the rate: `capture_oneshot()` started the clock before
`capture_settle()`, so taking the DMA channel down and setting it up again sat inside the
measured window. The same fixed cost is 2 % of a 500 us burst and 18 % of a 51 us one.
**Corrected, the delivered rate matches the setting to better than 1 % at every point** -
5/1 works out at 40157 against 40000 nominal. Fixed after this run: the clock starts after
`capture_start()` and `capture_oneshot_ticks()` reports the burst alone.

**The DAC test passes on its own merits**, with the ramp visible in the dump: 3728 falling
monotonically to 629 across the window, no reversal, largest step 90 counts out of a swing
of 3240, against a step limit of 405. Complete and in order.

**`test clkoff` unchanged:** 390 halves with CLKGEN6 switched off. The ADC does not run on
that generator, whatever Table 16-1 says.

**And one contradiction is left, sharper than before.**

The `loaded` column reads about 41 000 kSPS at *every* setting - including the rows where a
clean burst at the same configuration measures 3991. A factor of ten, same registers, same
board, seconds apart. That finally settles that every rate figure from runs 4 to 11 was an
artefact; it also asks how the stream can count ten times as many halves under load.

Together with the second oddity: there are overruns **at 4 MSPS** - 70 702 of 2 048 000
samples in the idle run, 3.5 %, and 702 in the DAC test's single burst. At 4 MSPS the DMA
has eight times the headroom it needs against the 33 M transfers/s Microchip quotes. That is
not bandwidth.

Two explanations fit and they need separating:

- **One conversion produces several DMA transfers.** Microchip acknowledges exactly this for
  this silicon: *"ADC triggers for DMA on this device have an issue. A few transfers are
  possible per one trigger. We are working to fix this problem in the next device revision."*
- **The handler books the same HALF or DONE more than once**, because the status flag did not
  clear and every later entry sees it again - and at full rate there are 1.6 million entries
  a second, one per overrun. That would be our bug, and fixable.

**How the next run separates them, and it is sharper than a guess.** The two explanations
scale with different things: booking the same event twice scales with the number of
interrupt entries, and therefore with the OVERRUN count; several transfers per conversion
scales with the CONVERSION count. At 4 MSPS with 3.5 % overrun those are very different
predictions - 140 000 entries per second against 3906 halves per second - so one sweep at a
low rate and one at a high rate pin it down even if both effects run at once. (The
observation is from the parallel session working on the GUI.)

Built in reaction (no board run yet): three counters, `isr_entries`, `half_events` and
`done_events`, printed per sweep row next to `blocks`. If `half_events` is of the order of
the overrun count, the flags are not clearing and it is us. If it stays at one per 1024
transfers while the counts still race, the transfers really are happening and it is the
silicon. One more `test sweep` decides it.

**What can be said to the customer already:** ADC to DMA to RAM with a double buffer works on
this device, the samples arrive complete and in the order they were converted, and the sample
rate is settable over PLL1 from 4 to 40 MSPS to better than 1 %. What is still open is the
loss at a given rate - the number that turns "it works" into "at 8 MSPS nothing is lost".

## 2026-09-24, a correction to how every one of these logs reads `dma_overrun`

Working out the prediction for the next sweep turned up something that changes the wording
of a lot of what is written above.

`OVERRUN` is **one bit** in `DMA0STAT`, and the handler counts it like this:

```c
if (st & DMA0_OVERRUN) { dma_overrun++; dma0_clear(DMA0_OVERRUN); }
```

`st` is a single read of `DMA0STAT` taken when the handler is entered. So `dma_overrun` is
incremented **once per handler entry in which the bit was found set** - not once per lost
sample. If three samples are lost between two entries, the bit is set once and the counter
moves by one.

**`dma_overrun` is therefore a lower bound on the samples lost, not a count of them.** Every
sentence of the form "about 4 % of the samples are lost as overruns" - in earlier entries
here and in the README - should be read as "the overrun bit was seen set in as many handler
entries as 4 % of the sample count". The true loss is that or worse, and how much worse
depends on how often the handler runs, which at these rates is exactly what is in dispute.

This does not change any conclusion drawn so far: the chain is proven by the DAC triangle,
which counts nothing and simply shows the samples arriving in order, and the rate is proven
by Timer1 against the PLL setting. It does change what can be promised about loss at a given
rate, which is the one number still outstanding for the customer - and it is a further reason
why the next run matters.

A cleaner measure exists and costs nothing: **`blocks_done` against `burst_starts`**. One
burst is a whole buffer, so blocks must be exactly twice bursts; no sampling of a flag is
involved. That relation is now in `status` and in every sweep row.

**The prediction for the next sweep, written down before the run.** A sweep point stops when
`blocks_done` reaches its target, so blocks is fixed at 2000 by construction and the effect
shows in the *time* and in `bursts`:

- If the handler books stale events (our fault), `bursts` comes out far below `blocks/2`.
  Run 14's loaded column was a factor of 10.2 too fast, so a 2000-block point would show
  roughly 98 bursts instead of 1000.
- If the counting is honest, `bursts` is 1000 and the discrepancy has to be real transfers -
  the trigger defect Microchip acknowledges.

The two are not subtle: 98 against 1000.

## 2026-09-24, run 15 - the rate control confirmed to 0.5 %, the counters still missing

`test sweep`, run by the colleague and relayed through the session working on the GUI. The
build banner says `7a9331a+local changes` - **three commits before the counters**, so the one
relation the run was asked for (`bursts` against `blocks`) is not in it. The decisive question
is still open and the run has to be repeated with master at `7a20674` or later.

**What it does settle, and it settles it well.** The `clean` column, corrected for the fixed
offset found in run 14 and fixed in `f3143a3` - this build predates that fix, so the offset
is in every row - lands on the nominal rate everywhere:

```
postdiv  nominal   clean   window    ideal   offset   corrected   deviation
  7/7      4081     3985   513.9 us  501.8    12.1 us     4075     -0.16 %
  5/5      8000     7662   267.3     256.0    11.3        8000     +0.00 %
  5/4     10000     9488   215.9     204.8    11.1       10012     +0.12 %
  5/2     20000    18066   113.4     102.4    11.0       20066     +0.33 %
  5/1     40000    32820    62.4      51.2    11.2       40078     +0.19 %
```

All fourteen rows are inside 0.5 %, and the offset itself only varies between 11.0 and
12.2 us across a factor of ten in rate. That is the strong part: **a rate-dependent error
could not be removed by subtracting a constant.** The fact that one number, the same at
4 MSPS and at 40, straightens every row is what makes the offset diagnosis and the rate
control both solid.

So, on the board and measured: **the sample rate is set by PLL1 and follows the setting to
better than half a percent from 4 to 40 MSPS.** Together with run 14's DAC triangle - samples
complete and in order - that is the working chain the customer asked about.

Two numbers unchanged from run 14 and still unexplained: `loaded` reads between 40652 and
42034 kSPS at every setting, and the slowest row shows 70702 overrun observations on
2 048 000 samples at 4 MSPS. The prediction written down before the next run stands: a
2000-block sweep point will show about 98 bursts if the handler books stale events, or 1000
if the counting is honest.

The full terminal output is in `docs/logs/run15-test-sweep.txt`. The derivation above came
from the parallel session working on the GUI and was re-computed here; the verbatim log is
kept because the conclusions below are drawn from its numbers and should be checkable against
the source.

## Run 15, read again with all fourteen rows - and the answer is nearly there

Two things in the full table are not visible in an excerpt, and together they almost settle
the open question before the counters have even run.

**Every sweep point took the same time, whatever rate it was set to.** The point ends when
`blocks_done` reaches 2000, and the `loaded` column says how long that took: between 40652
and 42034 kSPS in all fourteen rows, which is 48.7 to 50.4 ms. A spread of three per cent -
while the configured rate spans a factor of ten.

If `blocks_done` counted real half-completions, a 2000-block point would take 512 ms at
4 MSPS and 51 ms at 40. It took about 50 ms at both. **Whatever drives `blocks_done` under
load, it is not the rate at which halves are filled.**

**And that turns the next run into a choice between a rising curve and a flat line.** The
number of bursts actually started follows from the point duration and the configured rate:

```
postdiv   nominal   point takes   bursts if the handler books stale events   if honest
  7/7       4081       50.4 ms                    100                          1000
  7/6       4761       50.3                       117                          1000
  6/6       5555       49.9                       135                          1000
  7/5       5714       49.7                       139                          1000
  6/5       6666       49.8                       162                          1000
  7/4       7142       50.1                       175                          1000
  5/5       8000       49.5                       193                          1000
  6/4       8333       50.2                       204                          1000
  5/4      10000       49.5                       241                          1000
  6/3      11111       50.3                       273                          1000
  5/3      13333       49.1                       320                          1000
  6/2      16666       49.6                       404                          1000
  5/2      20000       49.0                       479                          1000
  5/1      40000       48.7                       952                          1000
```

One hypothesis predicts a column that climbs from 100 to 952 in step with the rate; the other
predicts 1000 fourteen times. There is nothing to interpret.

**Which rows to trust when it comes in.** The two models separate far better at the bottom of
the ladder than at the top: 100 against 1000 at 4 MSPS is a factor of ten, 952 against 1000 at
40 MSPS is five per cent and within the noise of a single measurement. The slow rows decide
the question almost on their own - and the slowest row is exactly the one that behaves oddly
(`missed 19`, `process` overrun nearly double `idle`).

The rule, stated as the criterion rather than as positions: **use every row in which the two
predictions differ by at least a factor of two, and drop the first row of the sweep.** The two
exclusions have two different reasons - the first row is a cold start, the fast rows have no
discriminating power - and naming the reasons keeps the rule right for a ladder with different
steps. For run 15's ladder that is rows two to thirteen, twelve usable rows rather than the
four a positional "rows two to five" would have kept. Row one stays in the table and gets its
own comparison, just not as evidence. (First raised and then sharpened by the session building
the evaluation into the GUI; the criterion form is theirs and is better than the positional
one written here first.)

**And the prediction rests on a number that was not in the output.** Every duration and every
rate derived from a sweep row is (halves x samples per half) divided by a rate, so the
evaluation above silently assumed 1024 samples per half - which `buf` can change at run time.
An evaluation made against the wrong length would be wrong without looking wrong. Fixed: the
sweep header now prints `samples per half` and `samples per point`.

**Second: the overrun counts fall as the rate rises.** 70702 at 4 MSPS down to 48096 at
40 MSPS - a third fewer at ten times the conversion rate. Read as a loss fraction that is
absurd. Read as what it is - the number of handler entries that found the flag set - it fits:
every point ran for the same 50 ms, the handler was saturated throughout, and at the higher
rate more losses fall into the same entry. That is the correction of the previous section
arriving from the data side, and it was the GUI session that spotted it.

**Third, unexplained: the first row behaves differently from all the others.** Row 7/7 has
`missed 19` where every other row has about 1900, and its `process` overrun count is 127278
against 70702 idle - nearly double - while in all other rows idle and process are within a
per cent of each other. The two hang together: in row 7/7 the main loop kept up and did its
work, and that work cost bus cycles and produced more overruns; everywhere else the main loop
got nothing and the process run therefore looks like the idle run. What is not explained is
why only the first row. It is the first point after the boot, so nothing has streamed before
it - a stateful difference in the start-up of a point is the obvious suspect, and
`counters_clear()` and `seen_blocks` are where to look. Worth watching in the repeat: if the
repeat shows the same thing in its first row, it is systematic and not noise.

**Practical note for the next attempt.** The banner says `+local changes`, which means the
working tree in front of the board differs from the commit it names - the same thing happened
on 23.09. before run 2. Before the repeat: `git status` to see what is modified, then
`git reset --hard` and `git pull`, and check that the banner afterwards reads the bare
revision with no `+local changes`. Otherwise the next log is as hard to interpret as this one.

## 2026-09-24, the repeat run is cancelled - the question stays open

The sweep with the counters will not be run for now. It would have been the third board run
asked of the colleague for the same question, and the user does not want to impose it - he is
glad the earlier ones were done at all. That is his call and it is a reasonable one.

**So this is the state the question rests in, and it should not be mistaken for an oversight
later.** Whether `blocks_done` is inflated by the handler booking stale events, or the ADC
really produces more DMA transfers than conversions, is **undecided**. The prediction written
down for it stands unchanged, and the firmware to answer it is in place - `bursts`, `blocks`,
`isr_entries`, `half_events`, `done_events` in every sweep row and in `status`, and the row
selection criterion above. It needs one `test sweep` on a board carrying `553f378` or later.

Runs 14 and 15 will therefore remain the newest board data for some time, and neither carries
the counters. Any evaluation that expects a `bursts` column will find none in them; that is
expected, not a broken log.

**What this does not touch.** The two results the customer statement rests on are not affected,
because neither depends on a counter:

- The chain carries every sample, in the order it was converted. That is the DAC triangle in
  run 14 - a known signal through the whole path, read out of a window nothing was writing.
  It counts nothing.
- The sample rate follows the PLL setting from 4 to 40 MSPS to better than half a per cent.
  That is Timer1 against the configured divider across fourteen points in run 15.

What stays open is the loss at a given rate, which is a refinement of the answer rather than
the answer.

**When it will be settled.** The user gets his own board around 01.10.2026 - CLAAS ordered an
EV17P63A, which is what the `nano-board` branch exists for. On his own hardware this is one
command and half a minute, with nobody to ask.

## 2026-09-24, run 16 - the counters answer, and the answer is not the one predicted

`test sweep` on `e52701e`, the first build carrying the counters. Full text in
`docs/logs/run16-test-sweep.txt`. The `+local changes` in the banner is `configurations.xml`,
which MPLAB X rewrites, plus untracked build leftovers - nothing that touches the firmware,
and `git log --oneline -1` confirmed `e52701e`.

**The prediction was wrong. The handler counts honestly.**

```
postdiv   half + done   2 x bursts      isr      overrun
  7/7        2019          2002        69792      69592
  7/6        2016          2016        66416      66192
  6/6        2014          2002        64134      63882
  5/5        2014          2014        64600      64325
  5/2        2010          2010        57285      56682
  5/1        2004          2004        48429      47761
```

One HALF and one DONE per burst, in every row, with the few extra counts belonging to the
clean one-shot that precedes each point. **There is no double booking.** The hypothesis that
the sweep's tenfold rate was our own arithmetic is dead, and with it the comfortable ending in
which the table was fine all along.

**A defect of my own instrumentation, and it nearly hid the result.** The line printed
`blocks (must be 2x bursts)` against values like 6078 and 85011, which looks like a factor of
three and then of forty. `blocks_done` is **not** reset by `counters_clear()` - it is free
running by design, because `seen_blocks` uses it - while every counter beside it is per point.
A total was being compared with a sample. The relation only appears when the per-point delta
is taken: 6070 per row, three sweep points of 2000 blocks each plus the one-shot. Fixed: the
line now prints `half+done` beside `bursts`, which are the two quantities that belong together.

**What the numbers leave standing is a sharper contradiction than before.**

With the counting honest, each loaded point moved 2000 blocks - 1000 bursts of 2048
conversions - and the `loaded` column says how long that took:

```
postdiv   nominal   clean (1 burst)   loaded    point took   conversions/s implied
  7/7       4081         4044          40197      50.9 ms         40.2 M
  5/5       8000         7872          40315      50.8 ms         40.3 M
  5/2      20000        19161          40903      50.1 ms         40.9 M
  5/1      40000        36728          41813      49.0 ms         41.8 M
```

**A single burst delivers the rate the PLL was set to. A thousand bursts deliver 40 MSPS
whatever the PLL is set to.** Same registers, same board, one second apart. And the interrupt
rate agrees with the second figure and not the first: 1.0 to 1.4 million entries per second in
every row, which a converter running at 4 MSPS could not produce.

The clean column is internally sound, which is what makes this hard to dismiss. After the
timing fix of `f3143a3` its residual offset is a constant 4.2 to 5.3 us across the whole
ladder - down from 11.3 - so a single burst really does take 2048 conversions divided by the
configured rate.

**Consequence for what may be claimed.** The statement "the sample rate follows the PLL
setting" holds for **one isolated burst**. For continuous streaming - which is what a
ping-pong application does and what the customer asked about - the measurement says the
opposite. That is a walk-back of what run 15 was read to mean here, and it has to be said
plainly rather than left in a footnote.

Unaffected: the DAC triangle of run 14. It counts nothing and shows a known signal arriving
complete and in order. The chain carries what the ADC converts; what is in dispute is how fast
the ADC converts when it is not left alone.

**Built in reaction, and it is one number that decides it.** `capture_oneshot_n(bursts)` runs
N bursts back to back - the ISR restarts each one exactly as continuous streaming does - and
times the lot. The sweep now measures the rate over **one** burst and over **ten** and prints
both as `clean1` and `clean10`:

- `clean10` equals `clean1`: bursts in a stream behave like an isolated one, and the
  difference is created by something in continuous operation itself.
- `clean10` jumps towards 40 MSPS: the **first** burst is the odd one, and every "clean" rate
  measured so far describes a start-up rather than the stream. In that case the PLL setting
  never influenced the streaming rate at all, and the agreement of run 15's clean column with
  the setting was an artefact of measuring exactly one burst each time.

The second outcome would mean the rate is not settable in continuous operation on this
silicon, which is the opposite of what the last two entries concluded. Written down here before
the run, as the previous prediction was - that one turned out wrong, and it should be visible
that it did.

## 2026-09-25, the chain test is ready - nothing of it has run on the board yet

`chain all` implements ANALYSIS.md C.8 to C.12 as one run of under a minute:
SCCP1 -> ADC core 5 in Single Conversion mode -> DMA0 Repeated Continuous ->
ping-pong -> CPU, DAC2 on RA8 as the signal, stages S0 to S9
(`docs/CHAIN-TEST-PLAN.md`). Built clean for the board (`tools/build.bat`, and
the IDE configuration through `tools/_test_mplabx.bat`) and for the simulator,
where it only prints SKIP. Interrupt vectors checked on the ELF: IRQ 51
`_CCT1Interrupt`, 52 `_CCP1Interrupt`, 241 `_AD5CH0Interrupt`.

Changed on the way, each a correction that applies to every earlier run as well:

- DAC clock 320 -> 400 MHz (PLL1 VCO divider, `VCO1DIV.INTDIV = 2`); SCCP1 clock
  320 -> 160 MHz (`CLK13DIV.INTDIV = 1`). Both were out of Table 40-24.
- `DAC2CON.UPDTRG = 11`: the DAC's data registers had never been given an update
  trigger (p1409).
- `dac2_period_ns()` returned one slope as the period (factor 2).
- `CCP1RB` is written in timer mode too.
- SCCP1 has two interrupts: the timer period raises `CCT1` (IRQ 51), not `CCP1`.

Found by testing the triangle evaluator on the host before the run (synthetic
windows with DNL +-5, noise, a modelled DAC filter): judging single steps at 14 LSB
per sample gave false "repeated" and "lost" samples on clean data, and a partial
last segment put a turning point 0.7 samples off. The evaluator now judges the
"slip" over two periods between full slopes, steps only from 40 LSB per sample.

**Predictions, written before the run:**

- S0: the clock monitor reads CLKGEN6 at 320 MHz, the VCO divider and CLKGEN7 at
  400 MHz, the PLL2 VCO divider at 500 MHz. If the monitor counts nothing, the
  monitor is misconfigured - not the clocks.
- S1: SCCP1 at 160 MHz. 320 MHz would mean the CLKGEN13 divider does not divide -
  which would reopen C.3 for every CLKGEN divider.
- S2: one ADC result per SCCP1 event, in timer mode, at every low rate. This is the
  first run of the triggered path with the three corrections of 24.09.2026; a zero
  here would mean a fourth error in the trigger path.
- S4: clean up to 16 or 20 MSPS; overrun or a transfer deficit from 26.7 MSPS on
  (the DMA ceiling of about 33 M transfers/s from support); 40 MSPS fails because a
  conversion takes 31 ns against a period of 25 ns.
- S5: the slope in samples within a few per cent of the model now that the DAC is in
  spec and takes its data - if not, open question 4 has a cause not yet named.
- S6: the placeholder processing (a sum over the half) needs about 4 to 5 CPU cycles
  per sample, so the CPU keeps up to about 20 MSPS and misses halves at 40.
- S8: CLKGEN6 divides as documented (160 and 80 MHz at ratios 2 and 4).

## 2026-09-25, GUI stream cycle prepared, not yet run on the board

The GUI's chain tile drives the standing chain on its own: `stream on <ksps>`, then
repeatedly `stream grab` (halt the trigger, send one contiguous window as a `GRAB`
frame, restart the SAME trigger), until stopped. New firmware:
`capture_chain_halt()`/`_resume()` (`capture.c`, pause/restart the SCCP1 trigger in
place, DMA channel left armed), `chain_stream_grab_begin()`/`_end()` (`chaintest.c`,
the per-cycle counters and the DAC triangle fields), `cmd_stream_grab()` (`cli.c`, the
`GRAB` frame, reusing `blk`'s binary writer and CRC). No new parser slot: `grab` is a
sub-command of `stream`, dispatched the same way `on`/`off` already are, so the count
stays 26 commands + help = 27 of 32 slots. Both firmware builds (`tools\build.bat`,
`tools\build.bat sim`, `tools\build.bat nano`) and both MPLAB X hardware
configurations (`EV74H48A_Curiosity_Platform_MPS512`, `EV17P63A_Curiosity_Nano_MPS506`,
`tools\_test_mplabx.bat`) are `-Wall -Wextra` clean; the 7-minute simulator acceptance
run was not run (the simulator has no SCCP/ADC/DMA and skips the chain test outright,
same as `chain all` always has). GUI side (`tools/adc_gui.py`): `python
tools/adc_gui.py --selftest` exercises the new frame parser and the grab cycle against
the fake target - a refusal before `stream on`, a clean grab that passes the grid
check, the next grab landing in the other buffer half (`from > 0`), an injected
lost-sample grab that correctly fails the same `tri_eval`/`grid_ok` the firmware's own
chain test is judged by, a CRC mismatch, a truncated frame, and `Target.grab()` timing
out against a serial stub that never answers.

**Not tested without a board, and what the colleague should watch for on first run:**

- Whether `capture_chain_halt()` really leaves the trigger off within the 25-tick wait
  it uses (the same bound `capture_chain_stop()` already relies on) - if not, the first
  grab after `stream on` would show a torn or short window rather than a clean triangle.
- Whether the half `capture_completed_half()` returns right after the halt is always
  the LAST FULLY completed one and never a partially-written one - the design relies on
  `ready_half` only ever being set at a HALF/DONE event, which the halt does not touch,
  but this has not been watched happen on silicon.
- The GUI cycle's real throughput: each grab is a serial round trip (up to 2048 samples,
  4 KB, at whatever the port's baud rate is) plus the halt, so the achievable grabs/s at
  115200 baud is expected to land around 2-3 for the default 1024-sample half, not the
  "grab interval" the GUI is set to - the interval is a floor, not a guarantee, and the
  status line says so.
- Whether the overrun brake (chaintest.c's `OVERRUN_LIMIT`) can fire BETWEEN two grabs
  while the trigger is halted - it should not, since nothing converts while halted, but
  if `stream` reports "off" after a grab that returned `ok`, that is the symptom to look
  for.
- The first real-hardware measurement of `chain_stream_grab_begin()`'s per-cycle
  counters: on a clean board they should read 0/0/0 every cycle at low rates; anything
  else on the very first grab points at the halt/resume pair rather than the chain
  itself, since the same counters read 0 throughout `chain all` and `stream on` already.

## 2026-09-25, run 17 - master 28e88fa (+ local changes), `test sweep`

The colleague ran the old back-to-back sweep. New in this sweep: the rate over one
burst (`clean1`) and over ten (`clean10`). Converted into durations:

```
pll  nominal  t1 us  t10 us  (t10-t1)/9 us   one burst at the set rate
7/7    4081   506.1   517.4      1.3         501.8
7/6    4761   434.6   894.8     51.1         430.2
5/5    8000   261.1   520.3     28.8         256.0
5/2   20000   107.1   564.8     50.9         102.4
```

The first burst takes the time the PLL setting predicts; bursts 2 to 10 take 1 to 52 us
each whatever the setting - 2048 transfers at DMA speed, not 2048 conversions. The 5/1
row is void (the overrun brake fired, clean1 = 609 GSPS). Read at the time as "reading 2
of open question 1: repeats"; run 18 found the reason.

## 2026-09-25, run 18 - master 28e88fa (+ local changes), `chain all` - THE DMA MODE WAS WRONG

The log reached us cut off inside S5.6 (pasted into a message that has a length limit);
S6 to S9 and the summary are missing. Logs should come as files.

- S0: Timer1 1 250 002 per 100 ms. Clock monitor: CLKGEN6 319.996 MHz, CLKGEN7 399.996 MHz
  - both as intended. The three PLL-output readings failed: code 0xB read 7.996 MHz,
  0xC 159.996 MHz (that is CLKGEN13), 0xE 399.996 MHz - the ATDF's CNTSEL codes for the
  PLL outputs do not select what they say. An instrument error, not a clock error.
  Core 5: no calibration, no other channel; RA8 analog.
- S1: SCCP1 159.999 MHz - the CLKGEN13 divider divides. Timer mode: 100, 1000, 10006
  events in 100 ms, exact. Output-compare mode: no event at all, neither CCT1 nor CCP1.
- S2: static transfer over RA8 at 8 levels: gain 1.030, offset -2.7 LSB, largest
  deviation 27 LSB; SAMC 31 against 0 at mid scale 1989 against 2051. **SCCP1 -> ADC:
  100/100, 1000/1000, 10006/10006 results per event** - the triggered path works on
  silicon for the first time. OC mode 0/0. CPU-stepped DAC: 1 of 256 samples off.
- S3: 6144 transfers, **15 conversions**, overrun 12; the buffer holds runs of about 400
  equal values (0x72E, 0xC4C, 0x365, 0x7AA, 0xCC1 ...).
- S4: about 2.05 M transfers in 50 ms at 100 kSPS and 1 MSPS (41 M/s), 13.6 M with the
  brake at 4 MSPS and above - at every rate, with CH0RES and with CH0DATA/IRQSEL=1 alike.
- S5: every window is a staircase of equal values, no triangle.

**Cause:** `dma.c` had `TRMODE = 3`, Repeated Continuous. DS70005591D 13.4.8.4 (p833):
"a single trigger starts a sequence of back-to-back transfers"; 13.4.8.5 (p834):
"multiple transfers can occur with each trigger". One conversion, one trigger, then the
DMA copies the result register at its own speed (~41 M/s) until the block is full. The
mode for one transfer per trigger is Repeated One-Shot, `TRMODE = 1` (13.4.8.3, p832).
This was set on the first day and never questioned; it is the "40 MSPS whatever the
setting", the overrun storms, run 17's `clean10` and in all likelihood the support
statement "a few transfers per trigger". **Every rate, overrun and missed figure of runs
1 to 18 was measured in the wrong DMA mode.**

**Changed:** `TRMODE = 1` in `dma.c` (master and nano-board). Nothing else.

**Predictions for the repeat of `chain all`:** S3 passes (6144 transfers for 6144
conversions, the stepped codes in every buffer index). S4 passes at least up to 16 or
20 MSPS with overrun 0; above that the DMA's real ceiling and the 31 ns conversion
time decide. S5 shows triangles, and the slope/model ratio answers open question 4.
S1's output-compare mode and S0's PLL-output codes stay failing (instrument, not chain).

