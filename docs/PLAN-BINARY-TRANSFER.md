# Plan: binary block transfer from the board to the PC

Status: plan, nothing implemented (23.09.2026). Branch `nano-board`.

## Why

`dump` prints samples as decimal text: about 5.9 bytes per sample, 1024 samples in
half a second at 115200 baud, and only the last completed buffer half. The GUI
(`tools/adc_gui.py`) works in capture cycles, so this is usable, but it caps the
refresh at about two blocks per second and never delivers a contiguous 2048-sample
buffer. Binary transfer of the whole buffer takes 4 KB instead of 12 KB - 0.36 s at
115200 baud, 0.09 s at 460800 - and gives the FFT twice the resolution.

The person implementing this cannot test on hardware directly; every board run has
to be delegated. So the plan is ordered so that each step is proven as far as it can
be without a board (host tests, the fake target, the simulator build), and the board
runs at the end are few and each one has an expected output written down.

## What is built

1. **`blk <n>` console command** (firmware): a contiguous block of `n` samples,
   1..2048, as binary. Sequence: stop the stream, let the burst end, run one fresh
   burst so that the whole 2048-sample buffer is written in one go, stop, send.
2. **Frame format** on the wire, after the parser's echo of the command:

   ```
   BIN n=<count> pace=<trg2src> per=<period> samc=<samc> in=<pinsel>\r\n
   <2*count bytes: samples as uint16 little-endian, 12-bit value in bits 11:0>
   \r\nCRC <hex4>\r\n
   > <ACK or NAK>
   ```

   The header is a text line, so a human on a terminal sees what is coming and a
   machine knows exactly how many bytes to read. The CRC is CRC-16/CCITT-FALSE
   (poly 0x1021, init 0xFFFF, no reflection, no xorout; check value of "123456789" is
   0x29B1) over the payload bytes only. The prompt and ACK/NAK follow as for every
   command, so the client's synchronisation rule does not change. NAK means the block
   could not be produced (stream did not deliver, out of range); the header is then
   `BIN n=0 ...` and no payload follows.
3. **Raw output path** (firmware): `cmd_parser_write()` takes a C string and cannot
   carry a 0x00 byte, so `blk` writes its payload with a new `console_write_raw(const
   uint8_t *, size_t)` in cli.c: same FIFO loop as `console_write()`, bounded waits,
   and the same Ctrl+C check as the parser's yield hook (an aborted block simply ends
   early; the client sees a short read and reports it).
4. **`baud <rate>` console command** (firmware, optional, separate step): reply with
   ACK at the old rate, flush, switch. Rates 115200, 230400, 460800, 921600. The
   MCP2221A on the EV74H48A is specified up to 460800; the Nano's nEDBG CDC limit is
   to be found out on the board. The client re-opens the port at the new rate.
5. **Client** (`tools/adc_gui.py`): `Target.blk(n)` reads the header line, exactly
   2·n bytes, the CRC line, then waits for ACK/NAK as usual; verifies the CRC; returns
   a `numpy` int array. `FakeTarget` produces the identical byte stream. The GUI's
   capture cycle uses `blk` when the board has it (probe once with `help`) and falls
   back to `dump` otherwise; "samples per capture" grows to 2048.
6. **Firmware self-demonstration in the simulator build**: the simulator has no UART
   receiver, so no command can be typed there - but its transmitter writes to a file.
   With `SIM_BLK_DEMO` (sim.h, simulator build only) the firmware calls the `blk`
   handler itself once after the self-test, for 64 samples of the synthetic sine.
   `tools/sim_trap.py` gains a check that parses the UART file bytes, verifies the
   header, the length and the CRC, and compares the samples with the sine vector.
   That proves the firmware's encoder end to end without a board.

## Steps, and how each one is verified before a board sees it

| # | Step | Files | Verified by (no hardware) | Verified by (board) |
|---|---|---|---|---|
| 1 | CRC-16/CCITT-FALSE in C (`crc16.c/.h`) and in Python (`adc_gui.py`) | new `crc16.c/.h`, `adc_gui.py` | both give 0x29B1 for "123456789"; Python unit test in `--selftest`; a C test vector printed by the simulator build (`[crc] 123456789 -> 0x29B1`) | – |
| 2 | `capture_block(n)`: stop, one fresh burst, stop; read access to the whole buffer | `capture.c/.h` | simulator: the ping-pong check already proves the buffer contents; a `SIM_BLK_DEMO` print of the first 8 samples matches the sine table | `status` after `blk`: `running 0`, counters unchanged |
| 3 | `console_write_raw()` with bounded waits and Ctrl+C check | `cli.c`, `console.h` | simulator: the demo block appears in the UART file with the right length | – |
| 4 | `blk <n>` command: header, payload, CRC line, ACK/NAK | `cli.c` | simulator: `sim_trap.py --blk-check` parses the UART file: header fields, 2·n bytes, CRC ok, samples == sine vector | terminal: `blk 16` shows the header, 32 bytes of noise, the CRC line, the prompt; GUI: one capture with 2048 samples |
| 5 | Client: `Target.blk()`, `FakeTarget.blk()`, GUI uses it | `adc_gui.py` | `--selftest` runs the fake target through `blk 2048`, CRC checked, FFT peak checked; a deliberately corrupted byte must be reported | GUI live mode on the board for a minute: cycle time printed, no CRC errors |
| 6 | `baud <rate>` (optional) | `cli.c`, `adc_gui.py` | fake target accepts it; client re-open logic exercised with the fake | `baud 460800` on the EV74H48A, then `status`; same on the Nano to find the nEDBG limit |
| 7 | Docs: README console table, HARDWARE-LOG entry per board run | `README.md`, `docs/HARDWARE-LOG.md` | – | – |

Order matters: 1 to 4 are firmware-only and fully checkable in the simulator; 5 is
PC-only and checkable with the fake target; only then does a board run add anything.

## What the board run must show (the delegated test)

Sent to whoever has the board, with the branch name and the commit:

1. `git reset --hard`, `git pull`, configuration for the board, Clean and Build, flash,
   terminal log from power-up.
2. On the console: `blk 16`. Expected: a `BIN n=16 ...` line, 32 bytes of binary (looks
   like noise in a terminal), a `CRC xxxx` line, the prompt. Send the log.
3. `tools\adc_gui.bat --port COMx`, "single" with 2048 samples: the time signal and
   the spectrum appear, the cycle line shows `2048 samples`; then "live" for a minute:
   no `cycle failed`, no CRC error. Send a screenshot and the cycle line.
4. If step 6 is in: `baud 460800` on the console, then reconnect the terminal at
   460800 and send `status`. Report whether it answers. On the Nano the same.

## Risks and where they show

- **Ctrl+C or a terminal typing during a block**: the raw writer checks the receiver
  like the parser's yield hook does; an abort ends the block early, the CRC line and
  the prompt still come, the client reports "short block". Not a hang.
- **The parser's echo before the header**: the client already skips the echo line
  (`Target.cmd`); the binary reader must skip it the same way before reading the
  header. Tested with the fake target, which echoes too.
- **`console_puts()` from the DMA path during a block**: the status line from
  `main()` could interleave with the payload. During `blk` the stream is stopped, so
  no `[stat]` line is due; the raw writer runs inside the receive interrupt like every
  command, and nothing else prints while it does.
- **The nEDBG's baud limit**: unknown; if 460800 fails, 230400 is the next try, and
  115200 always works. The `baud` command is the one step that can only be settled on
  the board, which is why it is last and optional.
- **12-bit values in 16-bit words**: bits 15:12 are zero from `AD3CH0RES`; the
  client masks them anyway.
