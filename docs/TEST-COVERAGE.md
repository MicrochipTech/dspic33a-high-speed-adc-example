# Test Coverage: which module the board run tests, and what it leaves out

As of 29.09.2026, after run 20 - the first board run after N+1 (phase BR,
`docs/IMPLEMENTATION-PLAN.md`; `docs/HARDWARE-LOG.md` 29.09.2026) - and three changes
checked by hand on the board the same day (below, "Since run 20"). One row per module of `CLAUDE.md`'s module table:
where it stood on silicon before N+1, what N+1 did to it, which board-run block
(`tools/board_run.py`, R0..R7) exercises it, what no block reaches, and which test
without a board covers it (**Off-board**: `tools\hosttest.bat`'s host tests, `tools\trace.bat`'s
register-trace scenarios, the simulator runs [SMOKE]/[SIM], the Python self-tests). A
scenario that only links a module without calling it does not count.

**Status**, before the board run:

| Status | Meaning |
|---|---|
| proven | ran on silicon (run 19 or earlier) and N+1 moved the file without changing its code (P1: `fncmp` 0 functions differing) |
| restructured | the function ran on silicon, but N+1 changed or moved code in it; the board run decides whether it still holds |
| never | never ran on silicon in any form |
| n/a | not in the hardware build, or linked but not called |

**The blocks** (A and B identical unless noted): R0 `sync`/`version`/`help`/`status` -
R1 `regs` - R2 `chain all` (S0..S9) - R3 `test all` (self, clock, clkoff, bursts, sweep,
matrix, dac) - R4 `stream on` at 1/4/8 MSPS, >= 50 `stream grab` each, then R4.gui (`buf 512`,
`stream on 4000`, `dac 2 on 256 3000 39`, 10 grabs that must each carry 512 samples,
everything restored) - R5 `stream on
1000 3 5 0` (core 3, PINSEL 5), 10 grabs - R6 `route list` (B only) - R7 `status`.

Every block needs boot and console, so `main.c`, `config_bits.c`, `clock.c`'s
`clock_init()`, `uart.c`, `cli.c`, `cmd_parser.c` and `fmt.c` are exercised by all of
them; the table names the block where a module is the thing being tested.

After the run, update this file from the result. The dots in `docs/ARCHITECTURE.md`'s
diagrams (since 30.09.2026 two states: green = functionally tested on silicon with the
current version, grey ring = not tested with this version yet) follow from
`docs/test_status.json`: `python docs/gen_architecture.py --apply-run <session zip>`
marks a box green once every board-run block covering it passed in B with no deviation
and nothing in scope is left open (fields `blocks` and `open`); a manual board test
written down in `docs/HARDWARE-LOG.md` counts too (`--mark-tested <rev> "<evidence>"
"<box>" ...`, owner's decision 30.09.2026). A green box turns grey again as soon as its
files change after the revision it was tested at. The status column in the tables below
is the history before N+1 (proven/restructured/never), not the dot.

**30.09.2026, the dots after the signal generator session** (`docs/HARDWARE-LOG.md`
29.09.2026, firmware 7b16260 on the EV74H48A): green are the modules those runs
exercised - console (`cli.c`, `cmd_parser.c`, `uart.c`), `gui_link.c`, `main.c`,
`acquisition.c`, `routing.c`, `board.h` (EV74H48A), `capture.c`/`pingpong.c`/
`dma0_event()`/`_DMA0Interrupt`, `diag.c`, `wait.h`, `clock.c`, `adc.c`, `dma.c`,
`sccp.c`, `dac.c`, `timebase.c`, `siggen.c`, and the GUI at 86f4753. Grey: what those
runs did not reach - `chaintest.c` (`chain all`), `bench.c`/`dactest.c`/`meter.c`
(`test`), the libraries box (`tri_eval`/`stats` not exercised by the firmware), `port_impl.c`,
`log.h`/`panic.h`/`regs.h`, `led.c` (not observed), `board_run.py`, `remote.py`. The
intermittent fail 8 after `stream off` stays listed as open on `capture.c`,
`acquisition.c` and `main.c`. When a gap in the "Not reached" column is closed, remove it
from that box's `open` list too.

## Since run 20 (29.09.2026)

**Run 20 turned no box green**: `gen_architecture.py --apply-run
docs/logs/run20-BR-remote-20260929.zip` finds B deviating or timing out in R0-R4 for every
box that was not already green, so the status column below is unchanged by it. Three
changes since, each checked by hand on the board (EV74H48A, `docs/HARDWARE-LOG.md`) but
by no board-run block yet:

| Change | Modules | Checked on the board | In a block |
|---|---|---|---|
| `dac ... force` - any DAC value, past the p1422 limits; the refusal names the broken limit | `dac.c` (`dac_triangle_force()`, `dac_triangle_limits_ok()`), `cli.c` | 79f3340, remote: refused without, taken with `force` | no - R4.gui sends `dac 2 on` without `force` |
| `stream on` keeps the length `buf` chose (it went back to 1024) | `acquisition.c` (`acq_chain_setup_input()`) | 955c473, local: `buf 256` -> grab n=256, `buf 1024` -> n=1024 | R4.gui checks the grab length since 29.09.2026 - A (b41af3b) has the fault and will FAIL it |
| the GUI's DAC cards, buffer tile, DISCONNECT during LIVE | `adc_gui.py` | by hand, both | no block runs the GUI itself |

R4.gui missed the `buf` fault in run 20: it judged `buf` by its reply ("samples per
half: 512"), not by the grabs that followed. It now requires every grab to carry the
length `buf` set; `board_run.py --selftest` checks both a board that honours it and one
that accepts `buf` and ignores it.

## Drivers (`src/drivers/`; `src/sim/` is lab, `sim.h` core since 02.10.2026)

| Module | Before N+1 | N+1 change | Tested by | Off-board | Not reached |
|---|---|---|---|---|---|
| `clock.c` | proven (every boot; S0/S8 clock monitor, run 18/19) | restructured - P4.7: 13 waits on `PORT_WAIT_WHILE`, trace/dump through the port layer, `_CLKFInterrupt` -> `clock_fail_hook()` | R0 boot, R1 dump, R2 S0/S8, R3 clock/clkoff/sweep | trace `clock`, `fail` (clock-fail path with stubbed halt/force_up), every scenario | the clock-fail path on silicon (only on a real clock failure; off-board the `fail` trace proves its order with stubs) |
| `adc.c` | proven (runs 14-19) | restructured - P4.5: port layer, `adc_init()` takes the buffer length as a parameter | R1, R2, R3, R4, R5 (core 3) | trace `boot`, `b2b`, `variants`, `stream_on*`, `regs`, `nano` | cores 1, 2, 4 in triggered mode |
| `dma.c` | proven (run 19: 8 MSPS, 0 overrun) | restructured - P4.6: buffer check, trace and dump through the port layer; `_DMA0Interrupt` unchanged, 42 instructions; 29.09.2026 (SG.1): channel 1 transmit API in `dma_tx.h`, shared window over table and buffer | R1, R2 S3/S4/S6/S9, R3, R4, R5 | trace `boot`, `b2b`, `stream_on*`, `regs`; ISR by `fncmp` (42/0) | the address-error and bus-error branches (fault only); channel 1 (`dma1_tx_*`): hand-run on the board 29.09.2026 (1 kHz loop), no block, host `test_siggen` only with stand-ins |
| `sccp.c` | proven (run 18 S2, run 19) | restructured - P4.3: register dump through the port layer; 29.09.2026 (SG.2): SCCP2 playback clock (TMR16, OC32) | R1, R2 S1/S2/S4, R4, R5 | trace `sccp`, `variants`, `stream_on*`, `sccp2` | output-compare mode (a known instrument fault, not used); SCCP2 by hand only (TMR16 confirmed, OC32 dead on silicon), no block |
| `dac.c` | proven (DAC triangle, runs 14/19) | restructured - P4.4: register dump through the port layer; 29.09.2026 `dac_triangle_force()`/`_limits_ok()` | R1, R2 S2/S5, R3 dac, R4 (`slp > 0`), R4.gui (`dac 2 on ...`) | trace `dac` (the only golden with a DAC slope) | DAC1 on RA1; the `force` path (by hand only) |
| `uart.c` | proven as part of `cli.c` | restructured - P5.1: out of `cli.c`, receive ISR 65 -> 55 instructions | every block; R4/R5: binary frames while the DMA interrupt runs | [SMOKE] transmit; receive ISR none (the simulator takes no injected bytes), `fncmp` 55/0 | `console_force_up()`'s PPS/TRIS path (clock-fail only; no golden trace covers it either) |
| `timebase.c` | proven | none (P4.2: already compliant) | R2 S0/S1, R3 rate/sweep | trace `timebase` | - |
| `led.c` | proven | none | toggled by `capture_service()` in R2/R4 | trace (linked in the capture scenarios) | nothing reads it back; visual only |
| `sim_dma.c`, `sim.h` | n/a (simulator) | P9.1: unaffected | `[SIM]` acceptance run, not the board | [SIM] acceptance run | - |

## Port layer (`src/port/`)

| Module | Before N+1 | N+1 change | Tested by | Off-board | Not reached |
|---|---|---|---|---|---|
| `log.h`, `wait.h`, `regs.h` | never (new) | P4.1-P4.8 | every driver's start-up trace (R0), every register dump (R1) | trace, every driver scenario | - |
| `panic.h` | never (new) | P4.1 | - | trace `fail` (`port_panic(10)` into the stubbed `fail()`) | `port_panic()` (fail codes 5, 8, 10) runs only on a fault |

## Core and app glue (`src/core/`, `src/app/`, `src/boards/`)

| Module | Before N+1 | N+1 change | Tested by | Off-board | Not reached |
|---|---|---|---|---|---|
| `main.c` | proven | restructured - BR.6: `diag_stack_paint()` first; smoke path (`SIM_SMOKE`) preprocessor-guarded | R0 | [SMOKE] | - |
| `sigproc.c` (core) | never (new 01.10.2026) | 02.10.2026: selectable filters at fs/8, Goertzel at fs/16 (folded), the impact counter (CNT) | by hand on the board, 02.10.2026 (HARDWARE-LOG): the filters against their design to three decimals, the Goertzel setups, the counter exact at 500/1000/2000 per second | host `test_sigproc` (91 checks: filters against the analog magnitude, Goertzel against a double DFT, counter on synthetic ring trains) | no `board_run.py` block switches the processing on |
| `example_main.c` (core) | never (new) | CORE.5 (02.10.2026): the core build's `main()` | none (no board-run block flashes the core build); by hand 02.10.2026: stream at 1/4/8 MSPS, `sigproc on`, the generator loop (HARDWARE-LOG) | `tools\build.bat core`/`make core`, link only | the core build under `board_run.py` |
| `config_bits.c` | proven | none | R0 (the board boots) | build only | - |
| `board.h`, `ev74h48a.c` | proven as `board.h` macros | restructured - P7.1: boot PLL dividers as `board_cfg` data | R0 (boot rate 7/7), R2 restore, R3 sweep/matrix | trace `b2b`, `clk`, `variants`, `stream_on*` | - |
| `ev17p63a.c` | never | P7.1 | - | build only (`nano` trace uses `board.h`'s macros, not this file) | the whole Curiosity Nano profile: its own first run later (BR decision 3) |
| `capture.c` | proven (run 19) | restructured - P9.1/P9.3/P9.4: ping-pong, meters and rate setters moved out; the processing loop rewritten after run 19 (32-bit reads, unrolled) | R2 S6/S9, R3, R4, R5 | trace `boot`, `b2b`, `stream_on*`; [SIM] ping-pong | - |
| `capture_chain_halt()`/`_resume()` | never | - | R4, R5 (every grab) | none | - |
| `pingpong.c` | proven as part of `capture.c` | restructured - P9.1: own file, `static inline` in the ISR | R2 S6/S9, R4, R5 | host `test_pingpong`; [SIM] | - |
| `acquisition.c` | proven as parts of `capture.c`/`chaintest.c` | restructured - P9.4/P9.4b: rate setters, variant matrix, standing stream and chain setup moved in; P11.3/P11.4: `stream on` through `routing_apply()`; 29.09.2026 `stream on` keeps `buf`'s length | R2 (`acq_chain_setup()`), R3 (rate setters, matrix), R4, R4.gui (grab length after `buf`), R5 | trace `b2b`, `clk`, `variants`, `stream_on`, `stream_on_input`, `route_stream` | - |
| `routing.c` | never (new, P11) | P11.1-P11.5 | R4 (`ROUTE_STREAM`), R5 (custom route, pin reachability), R6 (`route list`) | host `test_routing` (every rule, pass and trigger); trace `route_stream` | every conflict refusal (host test only); `ROUTE_B2B` is data only |
| `siggen.c` | never (new, SG) | SG.3, 29.09.2026 - table in `.dma_buffer`, DMA1 -> DAC paced by SCCP2, claim in `routing.c` | none yet (SG.8: block R8 not written) | host `test_siggen` (stand-ins for dma/sccp/dac/routing); trace `sccp2` for the clock; [SMOKE] `help` | on silicon by hand only (COM26, 29.09.2026: 1 kHz loop matches the table to 11-14 LSB rms); SG.8 R8; an intermittent fail 8 after `stream off` with the generator on (HARDWARE-LOG 2026-09-29) |
| `port_impl.c` | never (new, P4) | P4.1/P4.7 | indirectly by every block | trace, every scenario; `fail` proves the hook order | `clock_fail_hook()` (clock-fail only) |

## Console and link (`src/core/`; `cli_lab.c`, `b2b_link.c` in `src/lab/`)

| Module | Before N+1 | N+1 change | Tested by | Off-board | Not reached |
|---|---|---|---|---|---|
| `cli.c` | proven | restructured - P5/P6: UART, bench, link split out; P11.5 `route`; BR.6 eleven new `status` fields; 29.09.2026 `dac ... force` and a precise refusal | R0, R6, R7 | [SMOKE] `help`, `version`, `status` | most single commands (see "Commands" below) |
| `cmd_parser.c` | proven | none (32 slots as before) | every block | [SMOKE] | - |
| `gui_link.c` | never as a file | new - P6.4; CORE.2 (02.10.2026): `stream grab` only, `snap`/`rate`/`blk` to `b2b_link.c` | R4, R5 (`stream grab`) | none (the GUI's grab only against its Python stand-in) | - |
| `cli_lab.c` (lab) | proven as part of `cli.c` | CORE.3 (02.10.2026): the lab's commands moved verbatim out of `cli.c`, registered through `cli_register_lab()` | R2, R3 | [SMOKE] `help` (order: core first) | `start`/`stop`/`samc`/`input`/`core`/`clk`/`pll` as typed commands |
| `b2b_link.c` (lab) | `blk` never on silicon | CORE.2 (02.10.2026): `snap`/`rate`/`blk` moved verbatim out of `gui_link.c` | - | none | `snap`, `rate`, `blk`: no block sends them |

## Libraries and diagnosis (`src/lib/`; `diag.c` in `src/core/`, `tri_eval.c` in `src/lab/`)

| Module | Before N+1 | N+1 change | Tested by | Off-board | Not reached |
|---|---|---|---|---|---|
| `frame.c` | never (new) | P6.3 | R4, R5 (every GRAB frame) | host `test_frame` + `test_frame_xcheck.py` (parsed by `protocol.py`) | the `blk` framing |
| `crc16.c` | boot self-check only | none | R0 (boot self-check), R4/R5 (every frame's CRC, checked by the host) | host `test_crc16` | - |
| `fmt.c` | proven as part of `cli.c` | P2.1: moved out | every reply | host `test_fmt` | - |
| `stats.c` | proven as part of `cli.c`/`capture.c` | P2.2: moved out | R3 self | host `test_stats` | the `stats` command |
| `tri_eval.c` | proven (run 19, S5) | P2.3: moved verbatim | R2 S5 | host `test_tri_eval` + `test_tri_eval_xcheck.py` | - |
| `iir1`, `goertzel_f`, `goertzel_i`, `detect`, `wavegen` | n/a | P3: new, linked, not called | - (host tests only) | host tests against a Python reference (within 1 LSB) | everything, by design until N+3/N+4 |
| `diag.c` | proven (boot record, trap report) | restructured - P4.8 `reg_print()`; BR.6 stack high-water mark | R0/R7 (`status`: stack, buffer placement, boot/trap state), R1 (`regs_dump()`) | trace `regs` (`regs_dump()`/`reg_print()`); [SMOKE] `status` fields | the trap handler (fault only) |

## Lab: tests and meters (`src/lab/`)

| Module | Before N+1 | N+1 change | Tested by | Off-board | Not reached |
|---|---|---|---|---|---|
| `chaintest.c` | proven (runs 18/19) | restructured - P9.4/P9.4b: stream and chain setup moved to `acquisition.c` | R2 | none (only its evaluator, `tri_eval`) | `chain run`, `chain <n>`/`from <n>` |
| `bench.c` | proven (runs 16/17) | restructured - P6.2: moved verbatim out of `cli.c` | R3 | none | `sweep` as its own command (same code as `test sweep`) |
| `dactest.c` | proven (runs 13/14) | none | R3 dac | none | the `dactest` command |
| `meter.c` | proven as part of `capture.c` | restructured - P9.3: moved out | R3 self/bursts/rate/sweep | none (linked in `stream_on*`, not called) | - |

## Host side (`tools/`)

| Module | Before N+1 | Tested by | Off-board | Not reached |
|---|---|---|---|---|
| `protocol.py` (`Target`, `parse_grab_frame()`) | never against a board | R4, R5 - the GUI's own grab code | `test_protocol.py` (incl. a real socket loopback) | - |
| `board_run.py`, `eval_board.py` | never against a board (self-tests only) | the run itself | `--selftest` each (local and `--remote` against `fake_bench_client.py`) | `--remote` against the real relay |
| `adc_gui.py` | never against a board | by hand on the board 29.09.2026 (DAC cards with `force`, buffer tile, DISCONNECT during LIVE, remote and local) - no block | `--selftest` (incl. `dac force`, `buf` refused while streaming then taken), `gui_ui_test.py` (Playwright against `--fake`, incl. DISCONNECT during LIVE) | the GUI itself in a board-run block (R4 only runs its protocol code) |

## Commands no block sends

`help` lists 29 commands; the runner sends `version`, `help`, `status`, `regs`, `chain
all`, `test all`, `stream on|off|grab` and `route list`. Not sent, although the function
behind some of them runs inside another block:

- **used by the GUI:** `dac 2 on|off ...` and `buf [n]` - covered by R4.gui since
  28.09.2026, `buf`'s effect on the grab length since 29.09.2026 (DAC1, `dac 1 ...`, and
  `dac ... force` still not).
- **back-to-back transfer:** `snap`, `rate`, `blk`, `dump` - the only callers of
  `gui_link.c`'s back-to-back half and of `frame_send()`'s `blk` framing.
- **single settings:** `start`, `stop`, `samc`, `input`, `core`, `clk`, `pll`, `led`,
  `clear`, `reset` - the underlying functions run inside R2/R3 (`pll` through the sweep,
  `clk` through `test clock`), the command parsing does not.
- **single tests:** `selftest`, `stats`, `sweep`, `dactest`, `chain run`, `chain <n>`.

## Gaps worth a decision before the run

1. ~~**`dac` and `buf`**~~ - closed 28.09.2026 by R4.gui (`tools/board_run.py`).
2. **`snap`/`rate`/`blk`** - `gui_link.c`'s back-to-back half has never run on silicon.
   The GUI no longer uses it (25.09.2026); either a short R-block or an explicit "not
   tested, not used".
3. **Clock-fail and panic paths** (`clock_fail_hook()`, `console_force_up()`,
   `port_panic()`, the trap handler) run only on a fault. Off-board the `fail` trace
   proves the order (`capture_halt()`, then `console_force_up()`, then `port_panic(10)`)
   with stubs; `console_force_up()`'s own PPS/TRIS body and the real `fail()` stay
   untested on purpose - a deliberate fault on silicon is its own task.
4. **Routing conflicts** stay host-only: R6 shows one route; a refused second route or an
   unreachable PINSEL on silicon would take one more command in R5.

## Tests still to add

Measured against one question: when the board run shows a deviation, can it be narrowed
down without a board? Ranked by what they buy.

**Before the run (runner only, no firmware change):**

1. ~~**`buf` and `dac 2 on ...` in R4**~~ - done 28.09.2026: R4.gui in
   `tools/board_run.py`, checked by its self-test. A has both commands too, so
   `eval_board.py` diffs R4.gui between A and B like any other R4 rate.

**After the run (off-board, so the next deviation can be reproduced on the host):**

2. **Trace scenario for the grab cycle** - `capture_chain_halt()`, `_resume()` and
   `gui_link_stream_grab()`'s register sequence as a golden. R4 is the least-founded
   prediction of the run and has no off-board test at all; if it fails, this is the first
   tool needed.
3. **Host test for the grab counters** - `chain_stream_grab_begin()`/`_end()`'s per-cycle
   delta (overrun/late/missed/halves/transfers) against a stubbed capture, and the GRAB
   header `gui_link.c` builds, parsed by `protocol.py` like `test_frame_xcheck.py` does.
4. **Routing on silicon** - a second `stream on` while one is active, and an unreachable
   PINSEL, both in R5 as expected refusals (B only; A has no routing core, so each needs
   an explained A/B deviation in `expected.json`).
5. **`nano` trace linking `ev17p63a.c`** - today the board file is built but no scenario
   reads its `board_cfg`.

**Not worth adding:**

- Host tests for `chaintest.c`, `bench.c`, `meter.c`, `dactest.c`: they are instruments
  whose every result is a property of the silicon; off the board they would test their
  own stubs. Their evaluator (`tri_eval`) is already host-tested.
- A test for `snap`/`rate`/`blk`: the GUI stopped using them on 25.09.2026. Better to
  mark them "not tested, not used" - or retire them - than to write a test for an unused
  path.
- The `uart.c` receive ISR off-board: the simulator takes no injected bytes; every board
  block exercises it anyway.
