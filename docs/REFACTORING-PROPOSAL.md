# Refactoring for Portability: Proposal

As of: 26.09.2026, revision `28e88fa`. Based on: `docs/FIRMWARE-STRUCTURE.md` and
a dependency analysis of the drivers. None of this has been implemented yet.

**Goal:** Every module can be copied into another project without dragging along
files it does not functionally need. A driver brings only its port header, an
algorithm nothing but `<stdint.h>`.

## 1. What prevents porting today

The findings come from a search for calls from drivers into higher layers.

| Problem | Where | Consequence |
|---|---|---|
| **Drivers call the application** | `dma.c` calls `dma0_event()` (capture.c); `adc.c` calls `adc_ch0_event()` (chaintest.c); `clock.c` calls `capture_halt()` and `console_force_up()` in the clock-fail ISR | Whoever takes over `dma.c` must bring capture.c along, or write a function with exactly this name |
| **Drivers print to the console themselves** | `console_kv_hex` & co.: clock.c 25x, dma.c 18x, adc.c 16x, dac.c 12x, sccp.c 13x (mostly `*_regs_dump()` and the boot trace) | Every driver needs `console.h` and thus cli.c |
| **Drivers abort by themselves** | `fail()` in dma.c and clock.c | Every driver needs diag.c |
| **Instances are fixed in the name** | `dma0_*`, `sccp1_*`, `U2*`, `_DMA0Interrupt`, `_CCP1Interrupt` | A second DMA channel or a different SCCP requires a copy of the module |
| **Inconsistent instance abstraction in the ADC** | adc.c selects the core via `adc_cur`/`ADCBITS()`, but the ISR is fixed as `_AD5CH0Interrupt` and reads `AD5CH0RES` | The abstraction only holds as long as core 5 is used |
| **Application knowledge in the driver** | `SAMPLES_PER_BUF_MAX` in adc.c; clock.c knows "ADC PLL", "trigger clock", "DAC clock" as a fixed mapping | clock.c is a clock plan for exactly this project, not a clock driver |
| **Board knowledge scattered** | `board.h` mixes pins, UART PPS, ADC instance and PLL dividers at boot; is included directly by adc.c, led.c, cli.c, capture.c | A new board means changes in several modules |
| **UART driver in the CLI** | `uart2_setup()`, PPS, `_U2RXInterrupt` in cli.c | The console cannot be ported without the 2,000 lines of the CLI |
| **Simulator hooks in the application** | `SIM_CHECK_HALF`, `SIM_DMA_TICK` in capture.c and main.c | The ping-pong logic carries simulator knowledge with it |
| **Hardware-free algorithms in hardware modules** | `tri_eval`/`fit_line` in chaintest.c, `u32_to_str`/`half_stats` in cli.c, frame assembly in `cmd_blk_fn` | Reusable, host-testable logic cannot be taken over individually |

## 2. Target architecture

```
            app/      main, acquisition (rate, variant), clock plan
              |
   +----------+-----------+-------------+
   |          |           |             |
 link/      cli/       tests/        meter/          <- may use everything below
   |          |           |             |
   +----------+-----+-----+-------------+
                    |
                 pingpong/                              <- hardware-free, callbacks only
                    |
 drivers/   adc  dma  sccp  dac  uart  clkgen  pll  tmr  gpio
                    |
 port/      log.h  panic.h  irq.h  (implemented by the project)
                    |
 lib/       crc16  tri_eval  linefit  stats  fmt            <- only <stdint.h>, testable on the host
```

**Dependency rule:** Every layer may only include layers below it. `lib/` is
independent of everything. Drivers only know `<xc.h>`, `port/` and their own
header. Upward, a layer only communicates via registered callbacks.

### Directory structure

```
src/
  lib/        crc16.c  tri_eval.c  linefit.c  stats.c  fmt.c
  port/       log.h  panic.h           (interfaces)
  drivers/    adc.c  dma.c  sccp.c  dac.c  uart.c  pll.c  clkgen.c  clkmon.c  tmr.c  gpio.c
  pingpong/   pingpong.c
  meter/      meter.c  (counters, cost, rate)
  link/       frame.c  gui_link.c
  cli/        cli.c  cmd_parser.c
  tests/      chaintest.c  dactest.c  bench.c
  app/        main.c  acquisition.c  clock_plan.c  port_impl.c
  boards/     ev74h48a.h  ev17p63a.h  board.h (selection)
  sim/        sim_dma.c  sim.h
host/
  protocol.py (frame + CRC)  eval_chain.py  adc_gui.py  test_lib/ (gcc tests for lib/)
```

## 3. The proposals in detail

### V1: Hardware-free algorithms into `lib/`

`crc16.c` (already clean), `tri_eval()` and `fit_line()` from chaintest.c,
`u32_to_str()`/`u32_to_hex()` from cli.c as `fmt.c`, `half_stats()`/`half_mean()`
as `stats.c`. Interface only with `const uint16_t *`, length and a result
structure, no `volatile`, no global state.

Plus a host test (`host/test_lib/`, gcc), which already exists today for the
triangle evaluator, but works with extracted code. After that, it tests the
original file.

*Risk:* none. Pure move, no register change, no board run needed.

### V2: Port layer for output and abort

```c
/* port/log.h - implemented by the project; a driver only calls these */
void port_log(const char *s);
void port_log_kv(const char *key, uint32_t v, bool hex);

/* port/panic.h */
void port_panic(uint32_t code) __attribute__((noreturn));
```

All `console_*` calls in drivers become `port_log*`, every `fail()` in drivers
becomes `port_panic()`. `app/port_impl.c` redirects both to the console and to
`fail()`. In a foreign project these are two empty functions or a printf.

Better still for the register dumps: the driver does not print itself, but
calls a passed-in visitor:

```c
typedef void (*reg_visit_t)(const char *name, uint32_t value);
void dma_regs_visit(const dma_ch_t *ch, reg_visit_t visit);
```

Then the caller decides on the format (console, `@` log, binary frame), and
the driver no longer contains any text code.

*Risk:* low. The output stays the same, only the path changes.

### V3: Callbacks instead of fixed calls upward

```c
typedef void (*dma_event_cb_t)(void *ctx, uint32_t status);
void dma_set_callback(dma_ch_t *ch, dma_event_cb_t cb, void *ctx);

typedef void (*adc_result_cb_t)(void *ctx, uint16_t result);
void adc_set_result_callback(adc_t *adc, adc_result_cb_t cb, void *ctx);

typedef void (*clock_fail_cb_t)(void);
void clock_set_fail_callback(clock_fail_cb_t cb);   /* instead of capture_halt() + console_force_up() */
```

The ISR stays in the driver, but only calls the pointer now. The cost is one
indirect call per interrupt. For the DMA interrupt with over a million entries
per second, this has to be measured before committing to it (see open question
2 in `CLAUDE.md`). Alternative without runtime cost: a weak symbol
(`__attribute__((weak)) void dma_event_hook(...)`) with an empty default
implementation in the driver.

*Risk:* medium, because of the ISR runtime. Measurable before/after with
`capture_process_bench` and the matrix.

### V4: Extract the UART driver from cli.c

`drivers/uart.c`: init with instance, baud rate, pin configuration from the
board, transmit FIFO loop, receive callback. `cli/` builds a character-wise
input/output (`putc`, `write`, `rx_cb`) on top of it and no longer knows any
U2 registers. The recovery functions (`console_force_up`, `console_sync_baud`)
become `uart_reinit()` and `uart_set_baud()`.

*Risk:* low to medium. The console is the only diagnostic tool. If the change
breaks it, the board run is lost. Therefore check it in the simulator, where
the UART runs.

### V5: Instances as parameters instead of in the name

```c
typedef struct {
    volatile uint32_t *con, *stat, *src, *dst, *cnt;   /* from the ATDF */
    uint8_t irq;
    dma_event_cb_t cb;  void *ctx;
} dma_ch_t;

extern dma_ch_t DMA_CH0;          /* in dma.c, addresses from <xc.h> */
void dma_init(dma_ch_t *ch, const dma_cfg_t *cfg);
```

Same pattern for SCCP (`sccp_t`), UART (`uart_t`), DAC (`dac_t`). The ADC
already has it half done with `adc_cur`. There, bring the ISR onto the active
core, or, if the interrupt vectors do not allow that, create one ISR per core
with a shared body.

The ISR vectors stay fixed per instance (`_DMA0Interrupt`). They delegate to a
shared function with the instance pointer.

*Risk:* **high.** Every register access changes its path, and an error only
shows up on silicon. Only with a complete `chain all` before and after.
Therefore as the last step.

### V6: Split `clock.c` into generic drivers and a clock plan

| New | Content | Portable? |
|---|---|---|
| `drivers/pll.c` | Set PLL feedback, pre-divider, output divider and wait for lock | yes |
| `drivers/clkgen.c` | Clock generator n: source, divider, `DIVSWEN`/`CLKRDY` | yes |
| `drivers/clkmon.c` | Clock monitor as a frequency meter | yes |
| `app/clock_plan.c` | "PLL1 feeds the ADC, CLKGEN13 = PLL1/2 the trigger, CLKGEN7 the DAC", `clock_adc_set_rate()` with the search for `PLLFBDIV` | no, this is project knowledge |

The datasheet citations stay with the register values in the generic drivers.
The justifications for the clock plan (limits from Table 40-24) move along
into `clock_plan.c`.

*Risk:* medium. The boot order of the clocks is delicate. The clock plan at
boot must stay bit-identical. This can be compared via `regs_dump` before and
after the change.

### V7: Split `capture.c` into three parts

| New | Content |
|---|---|
| `pingpong/pingpong.c` | Half logic: `on_half(idx)` from the DMA callback, `service()` with `missed`/`late`, guard words. **No driver include.** The buffer is passed in from outside |
| `app/acquisition.c` | Wiring ADC → DMA → pingpong, variants, `capture_set_pll/rate`, `chain_stream_*` from chaintest.c |
| `meter/meter.c` | Counters, cost, `measure_rate`, `oneshot_n`, `selftest`, `clkoff_probe` |

The simulator hooks move out of `pingpong.c`: `sim_dma.c` implements the same
DMA callback and checks the half there. This way the simulator proves exactly
the file that also runs on the board.

*Risk:* medium. The ping-pong logic is proven (simulator and run 14). Run the
simulator acceptance run after the change. It takes about 7 minutes and only
runs when the user says so.

### V8: Board configuration as data

`boards/ev74h48a.h` and `boards/ev17p63a.h`, each with one `const board_cfg_t`
(UART pins and PPS, LED port, ADC instance and input, DAC route). Drivers no
longer include `board.h`. They get their configuration at init. Only `app/`
knows the board.

On the host side: generate `boards.py`, `pins64.py`, `pins128.py` from the
same source, or read them jointly from a JSON file, so that C and Python do
not drift apart.

*Risk:* low, if the macros are initially just moved into the structure.

### V9: GUI transport as its own protocol module

- `link/frame.c` (hardware-free, movable into `lib/`): header line, payload in
  chunks, CRC line. Writes via `size_t (*write)(const uint8_t *, size_t)`.
- `link/gui_link.c`: the `blk` and `stream grab` commands, which feed
  `frame.c` with data from `acquisition`.
- Host: `host/protocol.py` with `parse_grab_frame` and `crc16_ccitt_false`,
  extracted from adc_gui.py, so that `eval_chain.py` and other tools can use
  them without the GUI.

The frame format from `docs/PLAN-BINARY-TRANSFER.md` stays unchanged. This
keeps the GUI compatible.

*Risk:* low. Checkable with the GUI self-test (`adc_gui.py --selftest`,
`FakeTarget`).

### V10: Separate CLI and tests

- `tests/bench.c`: `test_*`, `matrix_*`, `sweep_*` from cli.c (about 700
  lines).
- Every module registers its own commands (`bench_register()`,
  `gui_link_register()`, `chain_register()`). `cli.c` only contains the basic
  commands and calls the registrations. A project without tests simply leaves
  out `tests/`.
- chaintest.c no longer reads `IPC` registers directly, but via
  `irq_priority(IRQ_DMA0)` from `port/irq.h` or via the register visitor from
  V2.

*Risk:* low. Pure move.

## 4. Order

Ordered by risk. Every step is its own commit, both variants build without
warnings (`tools\build.bat`, `tools\build.bat sim`), and the behavior on the
console stays the same.

| Step | Proposal | Board run needed? |
|---|---|---|
| 1 | V1 `lib/` + host tests | no |
| 2 | V2 port layer (log, panic, register visitor) | no, check console in simulator |
| 3 | V10 separate CLI/tests | no |
| 4 | V9 GUI transport | no, GUI self-test |
| 5 | V8 board configuration | short: boot and console |
| 6 | V4 UART driver | short: boot and console |
| 7 | V3 callbacks | yes: matrix and `chain all`, compare ISR runtime |
| 8 | V7 split capture.c | simulator acceptance run (on request), then `chain all` |
| 9 | V6 split clock.c | yes: `regs_dump` bit-identical before/after, `chain all` |
| 10 | V5 instance parameters | yes: full `chain all` |

Steps 1 to 4 run without a board. Steps 5 to 10 can be checked in **one**
board run, if they are applied together and the firmware writes `regs_dump`
and `chain all` with a revision identifier to the log beforehand. This matches
the rule "A board run costs a person their afternoon".

After every change to the file list: update `nbproject/configurations.xml`
accordingly (only the file list, do not change `languageToolchainVersion`) and
delete `adc_dma_40msps.X/build` and `dist`.

## 5. What deliberately stays non-portable

- **ISR vectors and configuration words.** They are device-specific by
  definition and stay in `drivers/` or `app/`.
- **The clock plan.** Which PLL feeds what is the core decision of this
  example.
- **The workarounds for silicon quirks** (clearing status with a single
  write, interrupt flag at the start of the handler, SCCP trigger code from
  the ATDF). They stay in the respective driver with a citation. Whoever
  ports the driver takes them along, and that is intentional.

## 6. Cost and benefit

Portable without effort would then be `lib/` (CRC, triangle evaluation,
statistics, formatting), `pingpong/`, `link/frame.c` and `cmd_parser` - that
is, everything that runs on any controller. Transferable within the dsPIC33A
family would be all drivers in `drivers/`, with a port header and a board
file. Project-specific would remain `app/` and `tests/`.

The effort is estimated at 2 to 3 days of rework for steps 1 to 4, and 2 to 3
days plus one board run for steps 5 to 10. The estimate is rough and not
derived from this project's experience values.

## 7. Restructuring without hardware

Addendum from 26.09.2026: no board is available for the restructuring. This
shifts the standard. A step may only be made if it can be proven
behavior-identical without silicon. Without a board, there is no "it'll
probably work".

### What can check behavioral equivalence without a board

| Instrument | What it proves | What it does not prove |
|---|---|---|
| **Build of both variants without warnings** | Interfaces fit, nothing is missing | Behavior |
| **Host tests with gcc** for `lib/` | Algorithms compute as before (generate golden data from the old code, check against the new one) | everything involving registers |
| **Register trace on the host** (new) | A driver writes the same values in the same order to the same registers after the restructuring | whether these values are correct on the chip (that was already proven, or not, beforehand) |
| **Simulator acceptance** (`sim_trap.py`, on request) | Ping-pong logic, counters, console output | ADC, DMA, PLL, interrupts |
| **Disassembly comparison** (`xc-dsc-objdump`) | For pure moves: identical code per function | nothing, as soon as code changes |
| **GUI self-test** (`adc_gui.py`, `FakeTarget`) | Frame format and CRC unchanged | Transmission over the real UART |

**The register trace is the key.** The drivers are compiled on the PC against
a substitute `<xc.h>`, in which every SFR is a variable and every write
access is logged, e.g. via macros or a C++ proxy. Before the restructuring,
the old code generates a golden log per entry point (`adc_init`, `dma0_init`,
`sccp1_start`, `clock_init`, …). After the restructuring, the new code must
deliver the same log, line for line. This makes V5 (instance parameters) and
V6 (split clock.c) checkable without a board too, as far as the
restructuring is concerned. Busy-wait loops on hardware bits (`CLKRDY`, PLL
lock) get a value in the substitute header that is satisfied immediately.

Effort for this test environment: about one day. It pays off because it
backs up every further step and remains as a regression test even after the
restructuring.

### Adjusted order

| Step | Proposal | Proof without a board |
|---|---|---|
| 0 | Register trace test environment + golden logs of today's state | – (is the instrument itself) |
| 1 | V1 `lib/` | host tests with golden data, disassembly |
| 2 | V2 port layer | register trace unchanged, build |
| 3 | V10 separate CLI/tests | disassembly per function, build |
| 4 | V9 GUI transport | GUI self-test, host test for `frame.c` |
| 5 | V8 board configuration | register trace unchanged |
| 6 | V4 UART driver | register trace, simulator (the console runs there) |
| 7 | V7 split capture.c | simulator acceptance (on request) |
| 8 | V6 split clock.c | register trace bit-identical |
| 9 | V5 instance parameters | register trace bit-identical |
| – | V3 callbacks in the DMA ISR | **defer** |

**V3 in the DMA interrupt is deferred.** Whether an additional indirect call
costs anything at over a million interrupts per second can only be measured
on the board, and open question 2 hangs exactly on this interrupt. Without a
board, this would be an unsupported claim. Interim solution: a weak symbol
(`weak`). This decouples driver and application at link time, without
changing the generated code of the interrupt. This can be proven via
disassembly. The callbacks for the ADC interrupt and the clock-fail interrupt
are uncritical and can be converted right away.

### What still remains open

The restructuring itself can be secured without a board: it demonstrably
does not change the register accesses. Timing cannot be secured this way,
neither the interrupt runtimes nor the processing cost per half. Before the
next board run, an entry in `docs/HARDWARE-LOG.md` naming the restructuring
therefore belongs there. The first `chain all` after that must reproduce the
values from before (`capture_process_bench`, matrix rows). Until then, the
restructured state counts as "not run on silicon".
