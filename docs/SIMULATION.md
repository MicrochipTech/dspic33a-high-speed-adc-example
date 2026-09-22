# Testing the console without a board

The MPLAB X simulator cannot run the measurement: it models neither the PLLs
nor the ADC nor the DMA of the dsPIC33A, so the firmware would stop at the
first clock wait (blink code 1). What it *can* do is run the command parser
and the UART transmitter. This document says how that is used, and what was
measured to find out — everything below was tried on MPLAB X v6.35 with the
dsPIC33AK-MP_DFP 1.4.260, on 2026-09-22.

## What runs in the simulator

The simulator build (`build.bat sim`, or the MPLAB X project with the tool set
to *Simulator*, both define `__MPLAB_DEBUGGER_SIMULATOR`) changes two things:

- `main()` initialises the console and then does nothing but poll it. Clock,
  ADC and DMA are not touched. The measurement commands still answer —
  `start` sets the run flag, `stats` and `dump` read the (empty) buffer,
  `selftest` reports NAK because no data arrives — so every command path,
  including the failure paths, is exercised.
- Console input comes from a RAM mailbox instead of the UART receiver: two
  variables in `cli.c`, `sim_rx_words[16]` (the line, four characters per
  32-bit word) and `sim_rx_len` (byte count, written last). `cli_poll()` feeds
  the bytes to the parser and clears the count.

Console output goes through the real UART1 transmitter in both builds. The
simulator writes it to a file.

## Running it

```
cd tools
build.bat sim                                            simulator ELF with symbols
python sim_cli.py --elf ..\build\adc_dma_40msps_sim.elf --test        built-in sequence
python sim_cli.py --elf ..\build\adc_dma_40msps_sim.elf               interactive
python sim_cli.py --elf ..\build\adc_dma_40msps_sim.elf --send status --send "samc 3"
```

`sim_cli.py` needs MPLAB X (for `mdb.bat`) and XC-DSC (for `xc-dsc-nm`, to
find the mailbox addresses in the ELF). It starts the simulator, programs the
ELF, waits for the parser's first prompt, and then talks to it exactly like a
script talks to the real board: send a line, read until the readiness byte,
ACK means success, NAK means failure. Starting takes about 30 s, each command
one to three seconds — the simulator runs at roughly 1/80 of real time.

The built-in sequence checks every command once, including the error paths
(bad arguments must give NAK and a usage line, an unknown command must give
NAK). The result of the run on 2026-09-22: 20 of 20 passed.

## How the plumbing works, and why it looks like this

MDB, the command-line debugger that ships with MPLAB X
(`mplab_platform\bin\mdb.bat`), is driven over its stdin/stdout from Python.
The relevant commands, in the order they have to come:

```
Device dsPIC33AK512MPS512
Set uart1io.uartioenabled true          before Hwtool, or it is ignored
Set uart1io.output file
Set uart1io.outputfile <absolute path>
Set oscillator.frequency 8
Set oscillator.frequencyunit Mega
Hwtool SIM
Program "<elf>"
Run
write 0x<addr> <word> <word> ...        while running
Print <symbol>
Halt / Quit
```

Things that were measured, because the documentation does not say them:

| Question | Answer |
|---|---|
| Does UART1 output reach the file while the simulation runs? | Yes. Three bytes fed through the mailbox produced their echo in the file within 3 s each; the firmware's transmit counter matched exactly. |
| Does the simulator take UART input? | **No.** `Set uart1io.input file` / `inputfile` are accepted silently and do nothing (the MDB guide lists only the output options), `RXBE` stays set. An SCL stimulus with `packetin(..., U1RXB, ...)` does not deliver either. That is why the mailbox exists. |
| Can memory be written while the simulation runs? | Yes, `write <addr> <values>`. The address must be numeric, the values decimal, and **every value is one 32-bit word** — a byte write overwrites the three bytes after it. Hence the mailbox is made of 32-bit words. |
| Does SCL work? | Yes, but only if `Stim` is issued **after** `Program`; loaded before, the stimulus is silently discarded. Simple processes that write RAM or SFRs work; reading a file from SCL (`file_open`/`readline`) delivered nothing and was not pursued. |
| How fast is it? | About 10^5 instructions per second, 231 634 loop iterations of ~8 instructions in 20 s. |
| Does `Print` need anything? | Debug symbols: build with `-g`, otherwise "Symbol does not exist". |

## What this proves, and what it does not

Proven in the simulator: the parser, the line editor, every command's
argument handling and error path, the ACK/NAK protocol, the UART transmit
path with the flow control the parser relies on, and that the firmware
builds and starts on the dsPIC33AK512MPS512 toolchain.

Not proven: UART reception (the simulator has none), the interrupt-driven
console on hardware (`_U1RXInterrupt` runs at priority 1 under the DMA
interrupt at priority 4 — that interplay only exists on silicon), and
anything the commands do to the ADC or DMA. Those are the first things to
look at on the board; `docs/TROUBLESHOOTING.md` has the list.
