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
