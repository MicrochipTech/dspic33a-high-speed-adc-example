# Firmware Architecture

An overview in two pictures: which modules exist and who may call whom, and what
happens on the way from the ADC to the CPU while the chain streams. `CLAUDE.md`'s
module table stays the complete, authoritative file list; this page is the map to it.

The diagrams are generated: `python docs/gen_architecture.py` writes both SVGs next to
this file. When a module is added, moved or renamed, change the generator and
regenerate - do not edit the SVGs by hand.

**Test status.** The dot in each box's top-right corner answers one question: *is this
module functionally tested on silicon with the current version?* Green = yes; a grey
ring = not tested with this version yet - no verdict on whether it works, only that the
test for this version is still to come. Hovering over a dot names the revision it was
tested at and where that is written down, or what is known from earlier versions, and
any finding still open. (Until 30.09.2026 the dots were green/amber/red plus a red
ring; that read as "broken" where it only meant "not re-tested by a board run".)

**Green needs evidence with a revision**: a board run - `python docs/gen_architecture.py
--apply-run <session zip>` marks every box whose board-run blocks all passed in B with
no deviation (`tools/eval_board.py`) and which has nothing in scope left open - or a
dated `docs/HARDWARE-LOG.md` entry that exercised the module on the board -
`python docs/gen_architecture.py --mark-tested <rev> "<evidence>" "<box>" ...`. Both
write `docs/test_status.json` and regenerate the diagrams. **A green box turns grey by
itself** at the next regeneration once one of its source files changes after the tested
revision - the code that was tested is gone. The data per box (covering blocks, open gaps, files) is
`docs/test_status.json`; the prose, module by module, is
[TEST-COVERAGE.md](TEST-COVERAGE.md).

## Layers and modules

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="architecture_layers_dark.svg">
  <img alt="Firmware layers and modules" src="architecture_layers.svg">
</picture>

Calls go downwards. A driver under `src/drivers/` reaches upwards only through the port
layer (`port_log()`, `port_trace*()`, `port_flush()`, `PORT_WAIT_WHILE()`,
`port_panic()`), implemented by `src/app/port_impl.c`; its register dump does not print
at all but hands each register to the caller's `reg_visit_t` (`xxx_regs_visit()`).
Dashed boxes do not run in the firmware (host tools) or are linked but not called yet.
Each driver sits directly above the peripheral it owns.

The only calls that go upwards are hooks:

| Hook | From | To |
|---|---|---|
| `dma0_event()` | `_DMA0Interrupt`/`_DMA1Interrupt` in `dma.c` | `capture.c` |
| `adc_ch0_event()` | `adc.c` | `chaintest.c` |
| `clock_fail_hook()` | `clock.c` (weak default) | `port_impl.c` (strong) |
| `uart_rx_hook()` | `uart.c` (weak default) | `cli.c` (strong) |

Register ownership: nobody outside `dma.c` touches a DMA register, nobody outside
`adc.c` an ADC register, nobody outside `clock.c` reads `CLK1CON`, nobody outside
`uart.c` touches a UART register. `board_cfg` is read by the application layer only,
never by a driver. Every build links exactly one board file (`ev74h48a.c` or
`ev17p63a.c`) and exactly one of `dma.c` / `sim_dma.c`.

## Data path while streaming

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="architecture_datapath_dark.svg">
  <img alt="Data path while streaming" src="architecture_datapath.svg">
</picture>

Top, the chain in silicon; bottom, what the firmware does with it. `stream on` sets the
chain up through the routing core (`routing_apply()` -> `acq_chain_setup_input()`: DMA
off, cores off, clock and trigger, cores on, DMA from scratch). SCCP1 paces the
conversions, so its period is the sample rate.

The buffer holds **two ping-pong pairs**, A and B, each a ping half and a pong half (up
to 1024 samples per half, 8 KB in all). DMA channels 0 and 1 run as the controller's
hardware ping-pong pair (DS70005591D 13.4.11): channel 0 fills the ping half, channel 1
the pong half, one result per trigger, and the hardware hands over from one to the
other without a lost sample. Each channel's DONE is one completed half; the interrupt
books it (`dma0_event()` -> `pingpong_on_half()`) and the main loop's
`capture_service()` processes it while the DMA fills the next one - with `sigproc on`,
through `sigproc_block()`: a 4th-order Butterworth low-pass with its cut-off at fs/4,
the middle of the useful band, its result written back into the same half.

`stream grab` does not stop anything. It asks `dma0_event()` to move on to the other
pair: the channel that is waiting at that moment is pointed at the other pair (a
waiting channel can be moved at any time, a running one cannot), so after the next
pong the pair just completed - ping then pong, contiguous - stands still. It goes out
as one GRAB frame with CRC-16 while acquisition and processing carry on in the other
pair, and is released for the next grab. The GUI therefore sees snapshots of the
signal, one pair at a time, and the processing never sees a gap in its input. The
back-to-back commands (`start`, `test`, `blk`) still use channel 0 alone on pair A.

Solid arrows are clock, data and calls; dashed ones are configuration and the main
loop's read access to the buffer. The signal generator (`siggen.c`, not in this
picture) plays its table through DMA channel 2.

## What has run on silicon

As of run 19 (25.09.2026, `docs/HARDWARE-LOG.md`): the chain SCCP1 -> ADC core 5 ->
DMA0 -> ping-pong -> CPU streamed for 15 s at 8 MSPS, 120 M samples, with overrun, late
and missed all 0; the DAC triangle came through without a lost or repeated sample up to
10 MSPS. The N+1 structure shown above (routing core, `acquisition.c`, the port layer)
has **not** run on silicon yet - the first board run to say whether it holds is phase BR
(`docs/IMPLEMENTATION-PLAN.md`). Still open: DMA overruns from 10 MSPS and the ADC's
triggered ceiling of about 18-20 MSPS (`docs/ANALYSIS.md`).

Since then, on the EV74H48A (01./02.10.2026, `docs/HARDWARE-LOG.md`): the two-pair
design - grabs of 2048 samples, the DAC triangle contiguous in every frame, overrun,
late and missed 0 at 1/4/8/10 MSPS, the stream never stopped; at 16 and 20 MSPS the
pair mode raises DMA overruns without losing data (open). The low-pass matches its
design within 0.6 dB on the triangle's harmonics and costs about 43 CPU cycles per
sample, so it keeps up to 2 MSPS; at 4 MSPS and above halves go unprocessed.
