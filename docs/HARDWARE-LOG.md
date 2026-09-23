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
repeat timer and SCCP1. The boot sweep now ends with a decision: the highest rate whose
`process` run had overrun 0 and missed 0 becomes the measurement rate (`[sweep] using
period ...`), or `[sweep] NO CLEAN RATE` if none. The overrun interrupt cannot be
switched off on its own - DS70005591D 13.6.1 says any channel event flag raises the
channel interrupt and `DMA0CH` has enables only for HALF, DONE and MATCH - so the
storm at 40 MSPS is avoided by not running there, not by masking it. If the console
was starved by that storm, `rx` will count at the chosen rate; if it stays 0, the
bytes never reach RD1.
