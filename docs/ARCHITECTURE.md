# Firmware Architecture

An overview in two pictures: which modules exist and who may call whom, and what
happens on the way from the ADC to the CPU while the chain streams. `CLAUDE.md`'s
module table stays the complete, authoritative file list; this page is the map to it.

The diagrams are generated: `python docs/gen_architecture.py` writes both SVGs next to
this file. When a module is added, moved or renamed, change the generator and
regenerate - do not edit the SVGs by hand.

**Test status.** The dot in each box's top-right corner says what is still to be tested:
green = fully tested on silicon with the current code; amber = ran on silicon, but the
code changed since, so a board run has to confirm it again; red = never ran on silicon.
A hollow red ring next to it means no test without a board covers the module either - a
failure there can only be narrowed down on the board. A box holding several modules
shows the worst of them; hovering over a dot names what is still open.

**A box turns green by itself** once it is fully tested: after a board run,
`python docs/gen_architecture.py --apply-run <session zip>` turns every box green whose
board-run blocks all passed in B with no deviation (`tools/eval_board.py`) and which has
nothing in scope left open, writes that into `docs/test_status.json` and regenerates the
diagrams. It prints every box that stays as it is, and why. A green box whose source
files change afterwards is drawn amber again at the next regeneration - the code that
was tested is gone. The data per box (covering blocks, open gaps, files) is
`docs/test_status.json`; the prose, module by module, is
[TEST-COVERAGE.md](TEST-COVERAGE.md).

## Layers and modules

![Firmware layers and modules](architecture_layers.svg)

Calls go downwards. A driver under `src/drivers/` reaches upwards only through the port
layer (`port_log()`, `port_trace*()`, `port_flush()`, `PORT_WAIT_WHILE()`,
`port_panic()`), implemented by `src/app/port_impl.c`; its register dump does not print
at all but hands each register to the caller's `reg_visit_t` (`xxx_regs_visit()`).
Dashed boxes do not run in the firmware (host tools) or are linked but not called yet.
Each driver sits directly above the peripheral it owns.

The only calls that go upwards are hooks:

| Hook | From | To |
|---|---|---|
| `dma0_event()` | `_DMA0Interrupt` in `dma.c` | `capture.c` |
| `adc_ch0_event()` | `adc.c` | `chaintest.c` |
| `clock_fail_hook()` | `clock.c` (weak default) | `port_impl.c` (strong) |
| `uart_rx_hook()` | `uart.c` (weak default) | `cli.c` (strong) |

Register ownership: nobody outside `dma.c` touches a DMA register, nobody outside
`adc.c` an ADC register, nobody outside `clock.c` reads `CLK1CON`, nobody outside
`uart.c` touches a UART register. `board_cfg` is read by the application layer only,
never by a driver. Every build links exactly one board file (`ev74h48a.c` or
`ev17p63a.c`) and exactly one of `dma.c` / `sim_dma.c`.

## Data path while streaming

![Data path while streaming](architecture_datapath.svg)

Top, the chain in silicon; bottom, what the firmware does with it. `stream on` sets the
chain up through the routing core (`routing_apply()` -> `acq_chain_setup_input()`: DMA
off, cores off, clock and trigger, cores on, DMA from scratch). SCCP1 paces the
conversions, so its period is the sample rate. DMA0 moves one result per trigger into
the ping-pong buffer and raises HALF/DONE; the interrupt books the completed half
(`dma0_event()` -> `pingpong_on_half()`), and the main loop's `capture_service()`
processes it while the DMA fills the other half. `stream grab` halts the trigger, sends
the last completed half as a GRAB frame with CRC-16 and restarts the trigger - the DMA
channel stays armed throughout.

Solid arrows are clock, data and calls; dashed ones are configuration and the main
loop's read access to the buffer.

## What has run on silicon

As of run 19 (25.09.2026, `docs/HARDWARE-LOG.md`): the chain SCCP1 -> ADC core 5 ->
DMA0 -> ping-pong -> CPU streamed for 15 s at 8 MSPS, 120 M samples, with overrun, late
and missed all 0; the DAC triangle came through without a lost or repeated sample up to
10 MSPS. The N+1 structure shown above (routing core, `acquisition.c`, the port layer)
has **not** run on silicon yet - the first board run to say whether it holds is phase BR
(`docs/IMPLEMENTATION-PLAN.md`). Still open: DMA overruns from 10 MSPS and the ADC's
triggered ceiling of about 18-20 MSPS (`docs/ANALYSIS.md`).
