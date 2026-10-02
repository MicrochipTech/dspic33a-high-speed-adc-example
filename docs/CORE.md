# Taking the core into your own project

This repository is a lab: it carries the example's measuring instruments, test suites and
simulator stand-ins next to the code that does the work. The part you need for your own
product is split out as the **core**. This project builds from the same core files
(`tools\build.bat core`), so the core is the same code that was tested on the board.

You receive it as `core-<revision>.zip` (made by this project's `tools/export_core.py`):
the folders below, the MPLAB X project `core_example.X`, this document and the GUI. The
revision is in the file name and in the board's boot banner (`version`), so a question
about a behaviour can always name the code it is about.

What the core does, in one sentence: **at a sample rate you choose, the ADC streams samples
through the DMA into RAM continuously, and your code processes each completed half of a
ping-pong buffer while the DMA fills the other.** Around that sits a console with a binary
grab for the GUI (`tools/adc_gui.py`), and a signal generator that plays a table through a
DAC.

## 1. What to copy

| Folder | Take it | What it is |
|---|---|---|
| `src/drivers/` | as is | clock (PLL1/PLL2, CLKGEN6/7/13), ADC, DMA (channels 0+1 in hardware ping-pong, channel 2 for the generator), SCCP1 (ADC trigger) and SCCP2 (generator clock), DAC1/2, UART2 console, Timer1, LED. `disi.h`: the interrupt-disable threshold. |
| `src/port/` | as is | four headers: how a driver logs, waits and stops (`log.h`, `wait.h`, `panic.h`, `regs.h`). You implement them, see 3. |
| `src/lib/` | as is | hardware-free helpers: `frame`/`crc16` (the binary frame), `fmt`, `stats`, `wavegen` (the generator's table). `iir1`, `goertzel_f`/`goertzel_i`, `detect` are building blocks the example does not call yet. |
| `src/core/` | as is, except `sigproc.c` and `example_main.c`, which are yours | the stream (`capture.c`, `acquisition.c`, `pingpong.c`), the routing core (`routing.c`), the generator (`siggen.c`), the console (`cli.c`, `cmd_parser.c`), the GUI's grab (`gui_link.c`), fault handling (`diag.c`), the simulator hook header (`sim.h`, empty on silicon), and the two files you start from: `sigproc.c` and `example_main.c`. |

**Do not copy** `src/lab/` (chain test, back-to-back burst mode, bench, meter, `snap`/`rate`/
`blk`) or `src/sim/` (simulator stand-ins). Nothing in the four folders above includes a
header from them. `tools\build.bat core` proves that: `src/lab/` and `src/sim/` are not even
on its include path.

## 2. The glue you write (model: `src/app/`, `src/boards/`)

Four small files connect the core to your board. Copy them from `src/app/` and
`src/boards/` and adapt them:

- **`board.h`**: the board's constants. The core reads exactly these macros:
  - `ADC_INSTANCE`, `ADC_PINSEL`, `ADC_SAMC`, `ADC_CLKDIV`: the default ADC core, input and sample time.
  - `DAC_ADC_CORE`, `DAC_ADC_PINSEL`, `DAC_UREF_PINSEL`: where the DAC test signal is read back.
  - `CONSOLE_TX_*`, `CONSOLE_RX_*`, `CONSOLE_PORT_NAME`: UART2's pins through PPS.
  - `LED_TRIS`, `LED_LAT`, `LED_ACTIVE_LOW`.
  - `BOARD`, `BOARD_EV74H48A`, `BOARD_NAME`, `BOARD_INPUT_NAME`, `BOOT_VERBOSE`, `BUILD_ID`.

  `board.h` includes `board_cfg.h` and an optional `version.h` (the git revision; without it
  the banner says "unknown").
- **`board_cfg.h` + one board file** (`src/boards/ev74h48a.c`): `const board_cfg_t
  board_cfg`, PLL1's two output dividers for the boot sample rate.
- **`config_bits.c`**: the configuration words. Keep the comments: two names are
  pack-version dependent and are written as numbers on purpose.
- **`port_impl.c`**: the port layer for the drivers, mapped onto your system. These seven
  functions:

  | Function | In this project |
  |---|---|
  | `port_log(s)`, `port_log_kv(key, v, hex)` | console lines |
  | `port_trace(s)`, `port_trace_kv(key, v, hex)` | start-up trace, printed only when `BOOT_VERBOSE` |
  | `port_flush()` | wait until the console has sent everything (before the CPU clock changes) |
  | `port_panic(code)` | `fail(code)`: stop and blink the code on the LED (noreturn) |
  | `clock_fail_hook()` | the clock-fail interrupt: halt the capture, bring the console up again |

  The **weak hooks** need nothing unless you want them: `cli_register_lab()` (cli.c: register
  your own console commands after the core's), `adc_ch0_event()` (adc.c: core 5's
  conversion interrupt), `uart_rx_hook()` (uart.c, overridden by cli.c).

Then `main()`: start from **`src/core/example_main.c`**. It brings the clock tree, the
console and the ADC/DMA chain up and calls `capture_service()` in the main loop. Nothing
converts until `stream on <ksps>` arrives. To stream from boot, set `EXAMPLE_START_KSPS`.

## 3. Where your processing goes

`sigproc_block(uint16_t *x, uint32_t n, const sigproc_info_t *info)` in
**`src/core/sigproc.c`**. `capture_service()` calls it from the main loop, never from an
interrupt, once per completed half, while `sigproc on` is set. You get the half in place:
read it, and write your result back into it. `sigproc.h` has the full contract. The rules
that matter:

- **Return within one half period**: `n` / sample rate, e.g. 1024 samples at 8 MSPS =
  128 µs = 25 cycles per sample at 200 MHz. Longer, and the next half is counted as
  `missed`. The GRAB frame's `load=` field and the GUI's "CPU load" chip show how much of
  the budget you use.
- **No console output and no waiting** inside it.
- Samples are 12-bit, right-aligned in 16 bits. The pointer is 4-byte aligned and `n` is
  even, so two samples per 32-bit access are allowed.
- `info->gap` = 1 means this block does not follow the previous one (first block, restart,
  or a missed half): reset your filter state there.

The example's body is selectable (`sigproc lp|hp|bp|off`): a 4th-order Butterworth low-,
high- or band-pass at fs/8, and independently a Goertzel detector for a tone at fs/16
(`sigproc gz on|off`). The coefficients come from `tools/sigproc_design.py`. A filter costs
about 63-67 cycles per sample, so it keeps up to about 2 MSPS; the Goertzel adds about
7. Replace it with your own; the console command and the GRAB fields (`proc=`,
`gz=`) can stay or go with it.

## 4. Building

**In MPLAB X:** open **`core_example.X`**. It is the core build as an MPLAB X project: two
configurations (`EV74H48A_Curiosity_Platform_MPS512`, `EV17P63A_Curiosity_Nano_MPS506`),
only the core's files plus the app glue, `example_main.c` as `main()`, and `src/lab/` and
`src/sim/` not on the include path. It refers to the sources as `../src/...`, so keep it
next to `src/`. Alternatively, make your own project and copy its file list and its
compiler settings. This project generates it from `adc_dma_40msps.X`
(`tools/gen_core_project.py`).

**Optimization -O1, not MPLAB X's default -O0.** `core_example.X` sets -O1. Keep it in your
own project. At -O0 the same code took twice the CPU time on the board: the example
low-pass used 62-69 % of the budget at 1 MSPS instead of 31 %, and at 8 MSPS halves were
missed with no processing switched on at all.

**On the command line**, as this project's `tools\build.bat core` (or `make core` in
`tools/`) does:

```
xc-dsc-gcc -mcpu=33AK512MPS512 -mdfp="<DFP>/xc16" -O1 -Wall -Wextra
           -I src/drivers -I src/app -I src/core -I src/lib -I src/port -I src/boards
           -T "<DFP>/xc16/support/dsPIC33A/gld/p33AK512MPS512.gld"
           src/core/example_main.c  src/app/config_bits.c  src/app/port_impl.c
           src/drivers/*.c  src/core/{pingpong,sigproc,capture,acquisition,routing,siggen,
           diag,gui_link,cli,cmd_parser}.c  src/lib/{crc16,fmt,stats,iir1,goertzel_f,
           goertzel_i,detect,wavegen,frame}.c  src/boards/ev74h48a.c
```

The sources include each other as `"name.h"` without folder prefixes, so every folder must
be on the include path. Tested with xc-dsc v3.31 (and v3.21 in MPLAB X) and the
dsPIC33AK-MP DFP 1.4.260. The core links clean at `-Wall -Wextra`, at -O1 and at -O0.
Its footprint: about 60 KB flash and 35 KB RAM. Of the RAM, 24.6 KB are the
DMA area (the ADC buffer, 2 pairs × 2 halves × 1024 samples, and the generator's
8192-entry table) and 8 KB the console's transmit ring.

**Keep the DMA area together.** The ADC buffer and the generator table are both in the
section `.dma_buffer`, and the DMA's single address window (`DMALOW`/`DMAHIGH`) covers
them. The default linker script places them next to each other. If you add a large array
of your own, check the map file: the XC-DSC linker places sections by size, and an array
that lands between the two widens the window around RAM the DMA must not write.

## 5. Resources the core uses

| Resource | For |
|---|---|
| PLL1 (+ CLKGEN6, CLKGEN7, CLKGEN13) | ADC clock, DAC clock (400 MHz), SCCP1 clock (160 MHz) |
| PLL2 | CPU at 200 MHz, peripherals |
| ADC core 5 (default; any core through `stream on <ksps> <core> <pinsel>`) | the stream |
| DMA channels 0 and 1 | the two ping-pong pairs, hardware ping-pong |
| DMA channel 2 | the signal generator |
| SCCP1 / SCCP2 | ADC trigger / generator clock |
| DAC1, DAC2 (RA1, RA8) | test triangle and signal generator |
| UART2 + its RX/TX interrupts | console |
| Timer1 | stopwatch (rate measurement, CPU load) |
| LED0 | heartbeat, fault code |

## 6. Talking to it

The console runs at 115200 baud (UART2). `help` lists the core's commands: `version`,
`status`, `regs`, `route`, `siggen`, `sigproc`, `stats`, `dump`, `clear`, `led`, `buf`,
`dac`, `stream`, `reset`. `README.md` describes each. `stream on <ksps> [core pinsel
[samc]]` starts the stream, and `stream grab` sends the last completed pair as a binary
frame with a CRC while the stream keeps running.

`tools/adc_gui.py` (set up once with `tools\gui_setup.bat`, then `tools\adc_gui.bat`)
works against the core build unchanged: it sends only core commands. `tools/protocol.py`
is the wire protocol on its own, for your own host scripts.

## 7. Known limits (board runs, `docs/HARDWARE-LOG.md`)

- The stream runs clean (no overrun, late or missed half) up to 8 MSPS. From 16 MSPS the
  DMA raises occasional OVERRUN flags, so far without a lost sample. At 20 MSPS halves are
  also missed. The ADC's triggered single mode ends at about 18-20 MSPS.
- Your processing budget shrinks with the rate: see 3.
- Open (02.10.2026): in the generator loop (DAC2 → RA8 → ADC core 5), some grabs show the
  DAC standing still for a whole half. Seen with this version and with 4c0cc55 alike, not
  explained yet.
