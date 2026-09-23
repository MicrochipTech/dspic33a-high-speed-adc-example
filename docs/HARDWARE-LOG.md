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
