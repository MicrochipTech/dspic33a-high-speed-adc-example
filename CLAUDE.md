# Working on this repository with Claude Code

A bare-metal demonstration example for the dsPIC33AK512MPS512 on the EV74H48A
(dsPIC33 Curiosity Platform Development Board, GP DIM). What it is meant to show is
one sentence, and the whole repository is measured against it:

> **At a sample rate you choose, the ADC streams samples through the DMA into RAM
> continuously, and the CPU processes them on the free half of a ping-pong buffer.**

That sentence has four parts - *chosen rate*, *continuously*, *through the DMA*, *CPU
processes* - and they are not equally far along. Some are proven on silicon, one is
currently not met at any rate. Do not describe the example as working without reading
`docs/ANALYSIS.md` first.

## Read these before changing anything

| Document | What it is |
|---|---|
| `docs/ANALYSIS.md` | **Start here.** Sorted by question: what was checked, with what instrument, what that instrument can and cannot say, which earlier results were withdrawn and why, what holds and what is open. The fastest way into the state of the project. |
| `docs/HARDWARE-LOG.md` | The dated diary of every run on the board, in order, including the predictions that turned out wrong. |
| `README.md` | The user-facing document: what the example does, how to build it, how to drive the console. |
| `docs/TROUBLESHOOTING.md` | The debugging guide, symptom first. |

The register writes in the code cite the datasheet (DS70005591D) page or table they
come from. That convention is kept.

## Modules and who may touch what

Since P1.1 (27.09.2026) the sources live under `src/`, one folder per role; the file
names below are unique across the tree, and every folder is on the include path of
every build (`-I` in `tools/build.bat`, `tools/Makefile`, `extra-include-directories`
in all three MPLAB X configurations, `tools/trace_build.py`, `tools/hosttest.bat`), so
the sources still include each other as `"name.h"` without a folder prefix:

| Folder | Files |
|---|---|
| `src/drivers/` | `adc`, `dma`, `sccp`, `dac`, `clock`, `timebase`, `led`, and since P5.1 (27.09.2026) `uart` (`.c/.h`) |
| `src/app/` | `main.c`, `capture.c/.h`, `config_bits.c`, `port_impl.c`, `board.h` (and the generated `version.h`), since P9.1 (27.09.2026) `pingpong.c/.h` (the ping-pong buffer's bookkeeping, out of `capture.c`), since P9.3 (27.09.2026) `capture_priv.h` (the narrow, non-public header `capture.c` shares with `src/meter/meter.c` and, since P9.4, `acquisition.c` - not on any other file's include path in spirit, only in practice), since P9.4 (27.09.2026) `acquisition.c/.h` (choosing and running the acquisition: the rate setters and variant matrix out of `capture.c`, the standing stream out of `src/tests/chaintest.c`; since P9.4b also the chain setup itself, out of `chaintest.c`), since P9.4b (27.09.2026) `acquisition_priv.h` (the narrow, non-public header `acquisition.c` and `src/tests/chaintest.c` share - replaces P9.4's `chaintest_priv.h`, which pointed the wrong way), and since P11.1 (27.09.2026) `routing.c/.h` (the routing core's types and resource checks; host-only so far, `routing_apply()` follows in P11.3) |
| `src/boards/` | since P7.1 (27.09.2026): `board_cfg.h` (the `board_cfg_t` type) and one `const board_cfg_t board_cfg` per board, `ev74h48a.c`/`ev17p63a.c` - exactly one linked per build, see the `board.h` row below |
| `src/cli/` | `cli.c`, `console.h`, `cmd_parser.c/.h` |
| `src/tests/` | `chaintest.c/.h`, `dactest.c/.h`, and since P6.2 (27.09.2026) `bench.c/.h` (the back-to-back test suite, out of `cli.c`) |
| `src/lib/` | `crc16.c/.h`, `fmt.c/.h`, `stats.c/.h`, `tri_eval.c/.h`, and since P3.7 (27.09.2026) `iir1`, `goertzel_f`, `goertzel_i`, `detect`, `wavegen` (`.c/.h`) - in every build, called from nowhere yet; and since P6.3 (27.09.2026) `frame.c/.h` (the `blk`/`stream grab` binary frame writer, out of `cli.c`) |
| `src/diag/` | `diag.c/.h` |
| `src/port/` | `log.h`, `panic.h`, `wait.h`, `regs.h` - the port layer (V2), headers only: what a driver may call outside itself; implemented by `src/app/port_impl.c` (`regs.h` needs no implementation: the caller passes the visitor in) |
| `src/link/` | since P6.4 (27.09.2026) `gui_link.c/.h`: `snap`/`rate`/`blk` and `stream grab`'s body, out of `cli.c` |
| `src/meter/` | since P9.3 (27.09.2026) `meter.c/.h`: the back-to-back measurement instruments, out of `capture.c` |
| `src/sim/` | `sim.h`, `sim_dma.c` |

| File | Owns | May call |
|---|---|---|
| `main.c` | start-up order, main loop | everything below |
| `board.h` | two board profiles selected by `BOARD` (EV74H48A with the MPS512 DIM, default; EV17P63A Curiosity Nano with the MPS506): pins (console PPS/TRIS, LED port/polarity), ADC core/input, the DAC route constants, `ADC_SAMC`/`ADC_CLKDIV`, `BOOT_VERBOSE`. Since P7.1 (27.09.2026) `#include`s `board_cfg.h` and no longer carries `ADC_PLL_POSTDIV1/2` - that pair moved to `board_cfg` (below), the one value out of all of these that turned out to be safe as run-time data. Everything else was tried as `board_cfg` data too and reverted, each for a different, checked reason - `src/boards/board_cfg.h`'s own comment has the full account: ADC core/input (`ADC_INSTANCE`/`ADC_PINSEL`) broke four P0.5 golden traces (`variants`, `b2b`, `clk`, `regs`) that rely on `adc_cur`'s compile-time default without ever calling `adc_select()` themselves (`tools\trace.bat` 7/13 FAIL) and sit on a file-scope static initializer (`adc.c`) plus a `#if` bound check (`adc.h`); LED port/polarity measured slower, larger code in `led_toggle()` (`capture_service()`'s hot path) and in `led_init()`/`led_on()`/`led_off()` (3 -> 24 instructions each, a throwaway fncmp experiment, not kept); the console's PPS/TRIS registers pack non-uniform bit widths only the compiler's own bitfield layout gets right, on `console_force_up()`, a path no golden trace exercises at all | - |
| `board_cfg.h`, `ev74h48a.c`, `ev17p63a.c` (`src/boards/`) | `board_cfg_t`: the boot sample rate as PLL1's two output dividers (`adc_pll_postdiv1/2`, board.h's old `ADC_PLL_POSTDIV1/2`) - `capture_set_pll()`'s four call sites (`main.c`'s boot, `bench.c`'s "sweep"/"matrix", `acquisition.c`'s `acq_chain_restore()` since P9.4b) read `board_cfg` directly instead of the macro. Exactly one of `ev74h48a.c`/`ev17p63a.c` is linked per build, the same way `dma.c`/`sim_dma.c` is: `tools/build.bat`'s `%BOARDFILE%`, `tools/Makefile`'s `SRC`/`SRC_NANO`, the MPLAB X project's per-configuration file exclusion (`nbproject/configurations.xml`, the `dma.c`/`sim_dma.c` pattern) | - |
| `config_bits.c` | every configuration word, with reasons | - |
| `clock.c/.h` | PLLs, clock generators, clock-fail interrupt, `clock_cpu_on_pll()`, `clock_adc_set_pll()`, `clock_adc_set_rate()`, the CLKGEN6 divider `clock_adc_set_div()` with its result codes, CLKGEN13 for the trigger (PLL1 out / 2 = 160 MHz), CLKGEN7 for the DAC (PLL1 VCO divider, 400 MHz), `clock_monitor_hz()` (clock monitor 4 as a frequency meter). On the port layer since P4.7 (27.09.2026): the 13 bounded waits of `clock_init()` are `PORT_WAIT_WHILE(.., 1..4)`, its start-up trace `port_trace*()`, its `console_flush()` before the CPU clock changes `port_flush()`, the register dump `port_log*()` (since P4.8 `clock_regs_visit(visit)`, one `visit()` per register through `port/regs.h`, no print call in it), and `_CLKFInterrupt` reports through `port_log*()` and stops with `port_panic(10)`; what the application must do first (halt the capture, bring the console up on the new clock) and the boot stage it names is `clock_fail_hook()` - declared in clock.h, a weak default in clock.c that does nothing and returns 0, the real one in `port_impl.c`. No `console.h`, `diag.h`, `capture.h` | port, timebase; `clock_fail_hook()` in port_impl.c |
| `adc.c/.h` | the ADC core: init, burst trigger, PINSEL/SAMC, the trigger-source registers, IRQSEL, calibration bits, core 5's CH0 interrupt as a counter (`adc_ch0_event()` in chaintest.c) - every register and vector core 5 uses is identical on the MPS506 (checked against the pack header 25.09.2026), so the chain test needs no board guard. On the port layer since P4.5 (27.09.2026): its register dump is `adc_regs_visit(visit)` since P4.8 (27.09.2026: one `visit()` per register through `port/regs.h`, no print call left in the driver - diag.c's `reg_print()` does the printing), its start-up trace prints through `port/log.h`'s `port_trace*()`, its ADRDY wait stops through `port_panic(5)` via `port/wait.h`'s `PORT_WAIT_WHILE` (P4.6a; until then a private copy of diag.h's macros), and `adc_init(pinsel, samc, burst_len)` takes the buffer length from the caller (`SAMPLES_PER_BUF_MAX`, passed by capture.c and main.c) instead of reading capture.h; no `console.h`, `diag.h`, `capture.h`. `adc_ch0_event()` remains a plain extern into chaintest.c until P8.3 | port, chaintest (`adc_ch0_event()`) |
| `dma.c/.h` | DMA channel 0: window = the buffer, HALF/DONE interrupt shell, status flags, `dma0_remaining()`. On the port layer since P4.6 (27.09.2026): the buffer check's two lines and its stop go through `port_log_kv()`/`port_panic(8)`, the start-up trace through `port_trace*()`, the register dump through `port_log*()` - since P4.8 `dma0_regs_visit(visit)`, one `visit()` per register through `port/regs.h`, `sim_dma.c` visits its three stand-in variables under the same name; `port_log_kv()` remains for the two lines of the buffer check; no `console.h`, `diag.h`, `capture.h` (it never included capture.h - `dma0_event()` is declared in dma.h). `_DMA0Interrupt` unchanged: 42 instructions, no indirect call (tests/baseline.md) | port; calls `dma0_event()` in capture.c (plain extern until P8.1) |
| `sim_dma.c` | replaces `dma.c` in the simulator build; implements `dma.h` without a DMA; its own ping-pong-order check (`sim_check_half()`, via `SIM_CHECK_HALF()`) is independent of pingpong.c and unaffected by P9.1 - it compares the completed half against the sine table, pingpong.c never enters into it | adc, capture, console |
| `pingpong.c/.h` | the two-half buffer's bookkeeping, out of `capture.c` on P9.1 (27.09.2026): `pingpong_on_half()` (which half just completed, the last sample, the free-running block count - `static inline`, called once per half exactly where `dma0_event()`'s old `if (st & DMA0_HALF)`/`if (st & DMA0_DONE)` blocks did, so the ISR stays 124 instructions, unchanged - see the design note in pingpong.h for the two designs that measured worse and why), `pingpong_completed_half()`, `pingpong_guard_ok()` (the guard-word check `capture.c`'s `guard_check()`/`capture_guard_ok()` call into), `pingpong_service()`/`pingpong_counters_clear()` (the main-loop side: `missed`, and the service bookmark). No driver include; the buffer, its guard words and the caller's `blocks_done`/`ready_half`/`last_sample` are passed in on every call, never cached, which is what keeps the ISR cheap. `late_service`/`dma_overrun` stay `capture.c`'s own globals, incremented exactly as before (pingpong.h explains why). `tests/host/test_pingpong.c` covers plain alternation, a gap with a service call in between, the late case (both flags in one event), a skipped service call (`missed`), and guard-word corruption (broken then reverted) | - |
| `routing.c/.h` | the routing core (P11.1, 27.09.2026, `docs/DESIGN-MULTICHANNEL.md` 4.4): `route_src_t`/`route_sink_t`/`route_t`/`route_err_t` and the resource table (5 ADC cores, 8 DMA channels, 8 SCCP, 2 DAC output pins, 1 UREF, a RAM budget), `routing_add()`/`routing_clear()` and every conflict/resource check - no `routing_apply()` yet (P11.3) and not linked into any firmware build yet (P11.3 does that). Hardware-free: no device header, no driver include, builds with host gcc; pin reachability comes from `tools/gen_route_pins.py` (parses `tools/pins128.py`) rather than a hand-typed table. `tests/host/test_routing.c` covers every rule with a passing and a triggering case (P11.2) | - |
| `capture.c/.h` | the measurement: the DMA buffer (private, with guard words; the ping-pong bookkeeping itself moved to `pingpong.c` in P9.1), `dma0_event()` (now mostly DMA-channel bookkeeping - `isr_entries`/`half_events`/`done_events`/`dma_overrun`/`dma_addr_err`/`dma_bus_err`/`late_service`/the overrun brake - calling into `pingpong.c` for the rest), the counters, start/stop/input, and the triggered stream `capture_chain_*()`, and `capture_chain_halt()`/`_resume()` - pausing and restarting an ALREADY RUNNING chain stream's trigger in place (DMA channel left armed, counters untouched), for the GUI's halt/grab/restart cycle. Since P9.3 (27.09.2026) the self-test and the other back-to-back instruments moved to `meter.c` (below); `process_buffer()`, `wait_for_blocks()` and `oneshot_left`/`oneshot_ticks` stay here (non-static, `capture_priv.h`) because `capture_service()`/`dma0_event()` also need them - see that header's comment for why moving them further would have meant exposing the DMA buffer's guard words outside this file. Since P9.4 (27.09.2026) the rate setters (`capture_set_pll/_rate/_clkdiv()`) and the variant matrix (`capture_select_variant()` and its reporting functions) moved to `acquisition.c` (below); `clkdiv_cur` stays here (non-static, `capture_priv.h`) because `capture_clkdiv_wanted()` also reads it | adc, dma, sccp, led, console, diag, pingpong |
| `meter.c/.h` (`src/meter/`) | the back-to-back measurement instruments, moved out of `capture.c` on P9.3 (27.09.2026): `capture_process_bench()`, `capture_selftest()`, `capture_clkoff_probe()`, `capture_oneshot()`/`_oneshot_n()`, `capture_measure_rate()` - bodies unchanged except two direct reads of capture.c's private `buf`/`half_len` that became calls to the existing public `capture_buffer()`/`capture_half_len()`. Declared in `meter.h`, which `capture.h` `#include`s so every existing caller (`cli.c`, `bench.c`, `gui_link.c`, `chaintest.c`, `dactest.c`) needed no change. Reaches `process_buffer()`/`wait_for_blocks()`/`oneshot_left`/`oneshot_ticks`, which stay defined in `capture.c`, through `capture_priv.h` - not part of the public API | capture, adc, clock, timebase, console, stats |
| `acquisition.c/.h` (`src/app/`) | choosing and running the acquisition, moved out of `capture.c` and `chaintest.c` on P9.4 (27.09.2026), bodies unchanged: `capture.c`'s rate setters (`capture_set_pll/_rate/_clkdiv()`) and variant matrix (`capture_select_variant()`, `capture_variant_name/_ksps/_regs()`, `capture_trigger_period_ns()`); `chaintest.c`'s standing stream (`chain_stream_on/_on_input/_off/_streaming/_state/_grab_begin/_grab_end()` and their private state - `chain_streaming()`/`chain_stream_state()` were not named in the P9.4 card but moved too, both being one-line readers of the same `s_on`/`s_ticks` the named functions own). P9.4 also left `chaintest.c`'s chain setup itself - `setup()`/`restore()`/`triangle_for()`/`rate_hz()`/`ksps_of()`/`wait_ticks()` - in `chaintest.c`, reached from `chain_stream_on_input()` above through a `chaintest_priv.h`: the application layer depending on the test layer's internals, backwards, and through non-static globals with generic names in the whole firmware's namespace. **P9.4b (27.09.2026) inverted it**: those six functions moved here too, renamed `acq_chain_setup()`/`acq_chain_restore()`/`acq_triangle_for()`/`acq_rate_hz()`/`acq_ksps_of()`/`acq_wait_ticks()` (bodies unchanged apart from the rename), and `chaintest.c`'s own stages call them now - test depending on app, the normal direction. `s_core`/`s_pinsel`/`s_samc`/`s_test_dac` (the input `acq_chain_setup()` applies) moved with them and became a plain static here: both the only reader (`acq_chain_setup()` itself) and the only writer (`chain_stream_on_input()`) are in this file now. Declared in `acquisition.h`, which `capture.h` and `chaintest.h` both `#include` so every existing caller needed no change. Reaches `capture.c`'s `clkdiv_cur` through `capture_priv.h`, and shares with `chaintest.c` - through `acquisition_priv.h`, replacing P9.4's `chaintest_priv.h` - the raw state both files still touch directly: `acq_trig_hz` (the measured trigger frequency, written here and by `chaintest.c`'s S1 stage), `acq_setup_rc_pll`/`_trig`/`_dac`/`_ok` (`acq_chain_setup()`'s own report, read back by `chaintest.c`'s S0 stage) and `acq_step_on` (`chaintest.c`'s CPU-stepped ADC flag, cleared defensively by `acq_chain_restore()`), plus the shared compile-time constants `CHAIN_CORE`/`_PINSEL`/`_SAMC`, `TRIG_HZ_NOMINAL`, `CPU_PER_TICK`, `TICKS_PER_MS`, `CHAIN_ON_SIMULATOR`. Trace scenarios `b2b`/`clk`/`variants` link `capture_set_pll()`/`capture_set_clkdiv()`/`capture_select_variant()` straight out of this one object, so their `.sources` still need `acquisition.c`, `dac.c` and the board file (`acq_chain_setup()`/`_restore()` call into both) - but no longer `chaintest.c`, `tri_eval.c`, `meter.c` or `-DHAVE_CHAINTEST` (P9.4b removed them: nothing in this file calls into `chaintest.c` any more) - the linker resolves every symbol a linked `.o` references, same rule `regs.sources`'s `sccp.c` already stood for | capture, adc, clock, sccp, dac, timebase, console, diag |
| `crc16.c/.h` | CRC-16 over a sample block, for the `blk` binary transfer | cli |
| `frame.c/.h` | the binary frame writer for `blk`/`stream grab` (P6.3, 27.09.2026): `frame_send()` writes a caller-built header line, the payload in chunks (`FRAME_CHUNK` = 64 bytes per `write()` call, the CRC folded in per chunk before it is sent), then the CRC-16 tail line - or, when `n == 0`, the bare `CRC 0000` failure shape both commands use. Output goes through a caller-supplied `size_t (*write)(const uint8_t*, size_t)` (`console_write_raw()`'s own signature, so the firmware caller needs no wrapper) and an optional `frame_aborted_fn`, polled only between payload chunks - never mid-chunk, never around the header or CRC line, exactly where `cli.c` checked `cmd_parser_aborted()` before this task. Does not build the header text itself (the fields differ between `blk` and `stream grab`); hardware-free otherwise, only `crc16.c`/`fmt.c`. `tests/host/test_frame.c` checks the clean/zero-length/chunk-boundary/abort-after-one-chunk shapes against a memory-buffer sink; `tests/host/test_frame_xcheck.py` (run by `hosttest.bat` after the C tests) dumps a realistic "blk"- and "stream grab"-shaped frame from the same binary and parses both back on the Python side - the GRAB one with `tools/protocol.py`'s own `parse_grab_frame()`, the same function the GUI uses - checking the CRC and the payload round-trip | - |
| `fmt.c/.h` | `u32_to_str()`, `u32_to_hex()`, `copy_str()` - the printf-free formatting helpers, moved out of cli.c on 27.09.2026 (P2.1); hardware-free, tested on the host by `tests/host/test_fmt.c` | - |
| `stats.c/.h` | `half_stats()` (min/max/mean) and `half_mean()` over `const uint16_t *` samples - no `volatile`, no knowledge of the DMA: the callers (cli.c's `completed_half_stats()`, `capture_selftest()`) pass the completed half and cast the volatile away there, with the reason in the comment. Moved out of cli.c/capture.c on 27.09.2026 (P2.2); the mean truncates (`acc / n`), pinned by `tests/host/test_stats.c` | - |
| `tri_eval.c/.h` | the chain test's triangle evaluator: `fit_line()`, `tri_eval()` (fills a `tri_t`: min/max, turning points, slope lengths, `step`, `zero`/`dbl`, `slip`) and the grid verdict `tri_grid_ok()`, with `TP_MAX`, `STEP_CHECK_LSB`, `GRID_SLIP_MAX`. Moved verbatim out of chaintest.c on 27.09.2026 (P2.3); the sample pointer keeps its `volatile` because the chain test evaluates the DMA buffer in place. No register, no DMA. `tests/host/test_tri_eval.c` repeats the 25.09.2026 host test on synthetic windows (DNL, noise, DAC filter; 2100 clean windows, 0 false alarms; a single lost/repeated sample in the middle half of the window found in 100 % of 4200 windows) and, as `--eval`, evaluates windows from stdin for `tests/host/test_tri_eval_xcheck.py` (P2.4, run by `hosttest.bat` after the C tests), which feeds the same `eval_chain.synth()` windows plus edge cases to both this C code and `tools/eval_chain.py`'s port and requires every integer field and verdict equal and every float field equal to within the float rounding C's `tri_t` applies | - |
| `iir1.c/.h` | first-order IIR low-/high-pass (`iir1_init/reset/lp/hp`, shift coefficient `k`), one `iir1_t` per filter (P3.2). In every build since P3.7 (27.09.2026), unused until N+4 | - |
| `goertzel_f.c/.h` | damped Goertzel in float (`goertzel_f_init/reset/block`), one `goertzel_f_t` per channel (P3.3); `cosf`/`sinf` become the FPU's `cos.s`/`sin.s`. Included, unused until N+4 | - |
| `goertzel_i.c/.h` | the same Goertzel in Q16 fixed point with int64 multiplies (`goertzel_i_init/reset/block`) (P3.4). Included, unused until N+4 | - |
| `detect.c/.h` | pulse detector with hysteresis and window (`detect_init/set_threshold/reset/sample/block/amplitude/adapt`), one `detect_t` per channel (P3.5). Included, unused until N+4 | - |
| `wavegen.c/.h` | signal-generator table from the `tab_wave_gen.py` formula (`wavegen_fill()`, `wavegen_snap_hz()`; `expf` from libm, `sinf` the FPU instruction) (P3.6). Included, unused until N+3 | - |
| `sccp.c/.h` | SCCP1 as a trigger source, with clock source, mode and event as parameters; its timer and compare interrupts as event counters - same registers and vectors on both boards, see `adc.c/.h` above. Its register dump moved to `port/log.h` in P4.3 (27.09.2026) and is `sccp1_regs_visit(visit)` since P4.8 (one `visit()` per register through `port/regs.h`; the driver no longer includes `log.h` at all); no `console.h` | port, capture, chaintest |
| `led.c/.h` | LED0 | - |
| `timebase.c/.h` | Timer1 as a stopwatch (12.5 MHz) for measuring the delivered rate. It sits on the CPU branch (PLL2) while the ADC is on PLL1, so it cannot flatter the ADC. Not involved in producing the rate. | - |
| `dac.c/.h` | DAC1 and DAC2 in Triangle Wave mode (DACOUT1 = RA1, DACOUT2 = RA8), one unit table, shared CLKGEN7, plus the internal UREF route to any core; the known signal for `test dac`. `dac2_*()` are DAC2-only aliases for the chain test's fixed pin route and its low-latency ISR path (`dac2_set()`). Its register dump moved to `port/log.h` in P4.4 (27.09.2026) and is `dac_regs_visit(visit)` since P4.8 (one `visit()` per register through `port/regs.h`, the DAC clock as `REG_DEC`; the driver no longer includes `log.h` at all); no `console.h` | port, clock (`clock_dac_hz()`); used by console, diag, chaintest |
| `dactest.c/.h` | judges captured halves against the DAC settings (min/max, reversals vs period, jumps); works with either DAC unit via `dac_active()` | capture, dac |
| `chaintest.c/.h` | the chain test `chain all` (S0..S9), the triangle evaluator (turning points by line fits, "slip"), the `@` log format, and `chain run` - builds and links unchanged for both boards (`tools\build.bat nano`, 25.09.2026); not yet run on Nano hardware, and the GUI cycle not yet run on either board. Since P9.4 (27.09.2026) the standing stream (`chain_stream_on/off`, `chain_stream_grab_begin()`/`_end()` - one halt/grab/restart cycle for `stream grab`, cli.c, the counters reported as the delta since the previous grab) moved to `acquisition.c`: it is acquisition, not a test, even though it grew here first. Since P9.4b (27.09.2026) the chain setup itself moved there too: `chain_all()`'s stages and `chain_run()` call `acquisition.c`'s `acq_chain_setup()`/`acq_chain_restore()`/`acq_triangle_for()`/`acq_rate_hz()`/`acq_ksps_of()`/`acq_wait_ticks()` (`setup()`/`restore()`/`triangle_for()`/`rate_hz()`/`ksps_of()`/`wait_ticks()` until then) - test depending on app, not the other way round, which is why P9.4's `chaintest_priv.h` (this file exposing them to `acquisition.c` as plain externs) is gone. What is still shared in both directions - `acq_trig_hz`, `acq_setup_rc_pll`/`_trig`/`_dac`/`_ok`, `acq_step_on`, and the constants `CHAIN_CORE`/`_PINSEL`/`_SAMC`/`TRIG_HZ_NOMINAL`/`CPU_PER_TICK`/`TICKS_PER_MS`/`CHAIN_ON_SIMULATOR` - comes from `acquisition_priv.h` instead (`src/app/`, not part of `acquisition.h`'s public API) | capture, acquisition, adc, sccp, dac, clock, dma (register dumps), diag |
| `bench.c/.h` | the back-to-back test suite, moved verbatim out of `cli.c` on 27.09.2026 (P6.2): the `sweep` command (`sweep_point()`, `sweep_row()`, `console_sweep()` - `static` here now, its only two callers moved with it) and the `test` command with its subcommands (`test_self/clock/clkoff/bursts/rate/sweep/dac`, `test_list()`, the variant matrix `cmd_matrix()`/`matrix_point()`/`matrix_stream()`). Registers through `bench_register_sweep()`/`bench_register_test()` (bench.h) - two functions, not one, because `cli.c`'s own `clk`/`pll` commands sit between "sweep" and "test" in the registration order that `help` must keep. Calls back into `cli.c`'s `put_kv()`/`put_line()`/`arg_u32()`/`usage()`/`run_dactest()` (no longer `static` there) rather than duplicating them | capture, adc, clock, timebase, dac, uart, chaintest; put_kv/put_line/arg_u32/usage/run_dactest in cli.c |
| `diag.c/.h` | `fail()` codes, trap handler, boot record in persistent RAM (including `chain_mark`, the chain test's stage), `RCON` report, `regs_dump()`, and since P4.8 (27.09.2026) `reg_print()` - the printing register visitor (`port/regs.h`: `REG_HEX` -> `console_kv_hex()`, `REG_DEC` -> `console_kv()`, `REG_TITLE` -> `console_puts()` of the whole line), which `regs_dump()` hands to every driver's `*_regs_visit()` and chaintest.c/capture.c hand to theirs; the trace harness's `stubs.c` carries the same function under `#ifndef HAVE_DIAG` | every driver's `*_regs_visit()`, `capture_regs_dump()`, `console_regs_dump()` |
| `port/log.h`, `port/panic.h`, `port/wait.h`, `port/regs.h` | the port layer (P4.1, 27.09.2026): `port_log(s)`, `port_log_kv(key, v, hex)`, `port_panic(code)` (noreturn), since P4.8 `regs.h`'s `reg_visit_t` = `void (*)(const char *name, uint32_t v, reg_fmt_t fmt)` with `REG_HEX`/`REG_DEC`/`REG_TITLE` - the one callback a driver's `xxx_regs_visit()` hands each register to (a title is the whole line, `"\r\n"` included, so it reaches the console in one call; three forms because the old dumps had exactly three kinds of line; the title stays with the driver because `dma.c` and `sim_dma.c` title the same interface differently), since P4.5 the start-up trace pair `port_trace(s)`, `port_trace_kv(key, v, hex)` (printed only when the project says so - the `BOOT_VERBOSE` gate stays in cli.c's `console_trace*()`, no driver carries it), and since P4.6a `PORT_WAIT_WHILE(cond, code)`/`PORT_WAIT_LIMIT` (`wait.h`: diag.h's `WAIT_WHILE`/`WAIT_LIMIT` with the same values - 2 000 000 iterations on silicon, no loop in the simulator - stopping through `port_panic(code)`; the one bounded wait for every driver) and since P4.7 `port_flush()` (block until the text handed over so far has left - clock.c before the CPU clock changes) - the only way a driver under `src/drivers/` may print, wait or stop. Headers only; nothing under `src/port/` is compiled | - |
| `port_impl.c` (app) | this project's implementation of the port layer: `port_log*()` onto `console_puts()`/`console_kv()`/`console_kv_hex()`, `port_trace*()` onto `console_trace()`/`console_trace_kv()`/`console_trace_kv_hex()`, `port_flush()` onto `console_flush()`, `port_panic()` onto `fail()`; and the clock driver's one upward call `clock_fail_hook()` (P4.7): `capture_halt()`, then `console_force_up()`, returns `boot_stage` - kept here rather than in a hooks file of its own until P8 adds the DMA/ADC hooks, so that no file list changed. A trace scenario (`tests/trace`) that links a driver using `port_*` lists this file in its `.sources` too (since P4.7 every scenario that links clock.c, `fail` and `clock` included); the harness's console/`fail()` stubs then turn the text into `C` lines exactly as before - no port stubs of its own, and the strong `clock_fail_hook()` here overrides clock.c's weak default in the MinGW link too (the `fail` golden proves it: `D capture_halt()`, `C <force_up>` in the same order) | console, diag, capture, clock |
| `uart.c/.h` | UART2 transport: pins/PPS routing (board.h; tried as `board_cfg` data in P7.1 and reverted - the PPS/TRIS registers pack non-uniform bit widths only the compiler's own bitfield layout gets right, on a path, `console_force_up()`, no golden trace exercises; `src/boards/board_cfg.h` has the full reasoning), the fractional baud generator, `uart_init/reinit/set_baud`, the non-blocking `uart_write()`, the bounded `uart_flush()`, the raw `uart_tx_full/putc/rx_empty/getc` accessors cli.c's own send/receive loops are built from, `uart_stat_probe()` (cli.c's "sweep" bus-load benchmark), `uart_enable_rx_irq()`, and the receive interrupt `_U2RXInterrupt` (moved out of cli.c in P5.1, 27.09.2026) calling the weak `uart_rx_hook(byte)` cli.c overrides strongly. Its register dump is `uart_regs_visit(visit)`, called from cli.c's `console_regs_dump()` rather than from diag.c directly - cli.c is not linked into the register-trace harness (`tests/trace/README.md`, decision 1), so the "regs" golden keeps the harness's stub line unchanged. Unlike the other drivers on the port layer, it reports through none of `port/log.h` (it IS what `console_puts()`/`console_kv()` write through - going through `port_log()` here would call back into itself) and panics through none of `port/wait.h` (none of its polling loops stop; "better a garbled line than none", cli.c's `console_puts()`); it still includes none of `console.h`, `diag.h`, `capture.h` or `cmd_parser.h` | cli (console_*), port_impl.c (none - see above) |
| `cli.c`, `console.h` | the commands (`snap`/`rate`/`blk` and `stream grab`'s body moved out to `gui_link.c` in P6.4, below - `cmd_stream_fn()` here still owns "stream on|off|grab" dispatch and the plain "stream" status report, calling `gui_link_stream_grab()` for "grab"), the reply helpers `put_kv()`/`put_line()`/`arg_u32()`/`usage()` and the DAC/UREF routing `run_dactest()` (not `static` any more - `bench.c` and `gui_link.c` both call them), and since P5.1/P5.2 (27.09.2026) the console's three output framings built on `uart.c` (`console_puts()` blocking, `console_write()` the parser's non-blocking sink, `console_write_raw()` the binary transfer's abortable one, now called from `gui_link.c` via `frame_send()`) plus the receive callback `uart_rx_hook()` - UART2 itself moved to `uart.c`, no register left here (`grep -E "U2|RPCON|RPOR|RPINR|IPC" src/cli/` finds nothing). Since P6.1 (27.09.2026) `cli_init()` calls three per-module registration functions (`bench_register_sweep/_test()`, `link_register()`, `chain_register()`) at exactly the old registration positions, so `help`'s order is unchanged; since P6.2 the `sweep`/`test` command bodies moved to `bench.c`, `clk`/`pll` stay here; since P6.4 `link_register()` itself is an extern into `gui_link.c` | clock, capture, dactest, led, diag, uart, bench, gui_link |
| `gui_link.c/.h` | the binary block transfer `snap`/`rate`/`blk` and the "stream grab" cycle's body (`gui_link_stream_grab()`), moved out of `cli.c` on 27.09.2026 (P6.4), with `link_register()` (P6.1's group, called from `cli_init()` as before, now an extern). `blk` and `stream grab` build their header text with `copy_str()`/`u32_to_str()` (`lib/fmt`, same field order cli.c used) and hand it to `frame_send()` (`lib/frame`, P6.3) together with the payload - the old private chunked-payload-plus-CRC loop, carried twice in cli.c, is now one function called twice. The restart-always rule of `stream grab` (`chain_stream_grab_end()` unconditionally after the transfer, `stream` off if the restart fails) is unchanged. Borrows `put_kv()`/`put_line()`/`arg_u32()`/`usage()` from `cli.c` (extern, like `bench.c` does) | adc, capture, clock, timebase, chaintest, console, cmd_parser, frame; put_kv/put_line/arg_u32/usage in cli.c |
| `sim.h` | the hooks the simulator build needs; all empty on silicon | - |
| `tools/adc_gui.py` | NiceGUI front end for the triggered chain only (25.09.2026 on - back-to-back retired from this tool, owner's decision): one acquisition card drives `stream on <ksps> [core pinsel [samc]]` / `off` / `grab` in a loop, plots the time signal and FFT from each grab, and evaluates the test signal's triangle with `tools/eval_chain.py`'s `tri_eval`/`grid_ok` (imported, not re-implemented) when the frame's own `slp > 0`; `--fake` uses a built-in stand-in (`FakeTarget`: the same triangle for the test signal, a configured sine with harmonics for any other input), `--selftest` runs the pipeline without GUI. CRC-16/CCITT-FALSE, the GRAB frame parser and `Target` (the serial console client) moved out to `tools/protocol.py` on 27.09.2026 (P6.5) - imported from there, not re-implemented; `FakeTarget`, `probe_grab()`/`query_buf()` stay here (they play the board or work generically over any target's `cmd()`, not the wire protocol itself). On connect it reads the board from the `version` reply (`[build] board: EV...`) and switches profile and default input; `--fake --fake-board EV17P63A` makes the stand-in report the Curiosity Nano. `tools/gui_ui_test.py` drives the page itself with a headless browser (Playwright) against `--fake`, both board profiles, on free ports. `tools/gui_setup.bat` makes its venv (`tools/.venv`, ignored). `tools/boards.py`, `tools/pins64.py`, `tools/pins128.py` hold the board/pin tables the GUI's board tile reads | protocol, the console protocol only |
| `tools/protocol.py` | the board's console wire protocol, moved verbatim out of `adc_gui.py` on 27.09.2026 (P6.5): `crc16_ccitt_false()`, the "stream grab" GRAB frame parser `parse_grab_frame()` with its header/CRC-line regexes, and `Target` (one command at a time over a COM port, synchronised on ACK/NAK, `grab()` for the halt/transfer/restart cycle). No NiceGUI, no board tables; `pyserial` only inside `Target.__init__`, so importing this module needs no port open. `tools/eval_chain.py` has no copy of any of these to redirect - the plan's "imported by adc_gui.py and eval_chain.py" is moot for the eval_chain.py half: it never had them. `tests/host/test_protocol.py` covers the CRC check value, a clean/NAK/CRC-mismatch/truncated GRAB frame, and `Target.grab()`'s timeout, the same checks `adc_gui.py --selftest` already made, now runnable without the GUI tool | - |
| `tools/eval_chain.py` | `chain all` log evaluator (`tri_eval`, `grid_ok`, `synth`) and CLI report; imported by `adc_gui.py` for the chain tile and by `FakeTarget.grab()` for its synthetic frames, so there is one triangle evaluator and one synthetic-triangle generator, not two | - |
| `cmd_parser.c/.h` | the command parser, unchanged from github.com/zabooh/cmd_parser (Apache 2.0) - do not edit | - |

Nobody outside `dma.c` touches a DMA register, nobody outside `adc.c` an ADC register,
nobody outside `clock.c` reads `CLK1CON`, nobody outside `uart.c` touches a UART
register. A driver under `src/drivers/` logs, waits and
panics only through `port/` (`port_log()`, `port_log_kv()`, `port_trace*()`, `port_flush()`,
`PORT_WAIT_WHILE()`, `port_panic()`) - never `console_*`, `WAIT_WHILE()` or `fail()`
directly, and it includes none of `console.h`, `diag.h`, `capture.h`. Its register dump
does not print at all: `xxx_regs_visit(visit)` hands each register to the caller's
`reg_visit_t` (`port/regs.h`, P4.8), and diag.c's `reg_print()` is the visitor that
prints. Since P4.7
(27.09.2026) every driver complies; what a driver needs from the application it asks
for through a hook declared in its own header (`clock_fail_hook()`, clock.h; the P8
hooks for `dma0_event()`/`adc_ch0_event()` follow the same pattern).

**Board data (P7.1, 27.09.2026): a driver gets its board-specific value through a
parameter of its own `*_init()`, or keeps it a `board.h` macro - never `board_cfg.h`
directly.** `board_cfg` (`src/boards/board_cfg.h`) is read only by the application layer
(`main.c`, `bench.c`, `chaintest.c`), at the one call site that needs the boot PLL setting;
no file under `src/drivers/` includes `board_cfg.h`, and `grep -rn board_cfg src/drivers/`
finds nothing. This is a narrower rule than V8 (`docs/REFACTORING-PROPOSAL.md`) set out
to prove, on purpose: P7.1 tried the wider move (ADC core/input, LED port/polarity, the
console's PPS/TRIS pins) and reverted each one for a reason specific to it, not a reason
against the pattern in general - see the `board.h` row above and `src/boards/board_cfg.h`
for the full account of what was tried, what broke, and why.
`timebase.c` and `led.c` comply as they are (P4.2, 27.09.2026: they never called either -
`led.c` mentions `fail()` and `capture_service()` in its header comment only, and keeps
`board.h` for good: P7.1 tried moving `LED_TRIS`/`LED_LAT`/`LED_ACTIVE_LOW` into
`board_cfg` and reverted it - `led_toggle()` runs from `capture_service()`'s hot path and
measured slower with a runtime pointer+mask, `led_init()`/`led_on()`/`led_off()` 3 -> 24
instructions each; see `src/boards/board_cfg.h`); `sccp.c` moved over in P4.3 (27.09.2026: the 13 `console_*` calls of
its register dump became `port_log()`/`port_log_kv()`, the `console.h` include went, and
every trace scenario that links it lists `src/app/port_impl.c` too); `dac.c` likewise in
P4.4 (the 12 `console_*` calls of its register dump, `dac.sources` gained
`port_impl.c`); `adc.c` in P4.5 (the 12 `console_*` calls of its register dump, the
three `console_trace*()` calls of `adc_init()` - for which `port/log.h` gained the
`port_trace*()` pair - `WAIT_WHILE`/`WAIT_LIMIT` first as a private copy with `port_panic(5)`, since P4.6a `port/wait.h`'s `PORT_WAIT_WHILE`,
and `SAMPLES_PER_BUF_MAX` as `adc_init()`'s third parameter; every scenario linking
`adc.c` already listed `port_impl.c`); `dma.c` in P4.6 (the two `console_kv_hex()` and the
`fail(8)` of the buffer check, the three `console_trace*()` of `dma0_init()`, the 13
`console_*` of its register dump; every scenario linking `dma.c` already listed
`port_impl.c`); `clock.c` in P4.7 (13 `WAIT_WHILE` -> `PORT_WAIT_WHILE`, four
`console_trace*()`, `console_flush()` -> the new `port_flush()`, the 17 `console_*` of its
register dump, `_CLKFInterrupt`'s five report lines and `fail(10)` -> `port_panic(10)`,
`capture_halt()` + `console_force_up()` + `boot_stage` -> `clock_fail_hook()`; `<stddef.h>`
for `NULL`, which had come in through the removed headers; `fail.sources` and
`clock.sources` gained `port_impl.c`). `uart.c` is the one exception to the port-layer
paragraph above, by design, not by omission: it moved out of cli.c in P5.1 (27.09.2026)
with every UART/PPS/interrupt-controller register cli.c used to touch, but it reports
through none of `port/log.h` and panics through none of `port/wait.h` - it is what
`console_puts()`/`console_kv()` end up writing through, so calling `port_log()` from
inside it would call back into cli.c through port_impl.c, and none of its polling loops
stop with a panic (`console_puts()`'s "better a garbled line than none", `uart.h`'s
`UART_TX_WAIT_LIMIT`). What a byte received means is asked through `uart_rx_hook()`
(weak in uart.c, strong in cli.c, not in port_impl.c: unlike a clock failure, this is
the console's business specifically, not a generic application concern). Its register
dump, `uart_regs_visit(visit)`, is called from cli.c's `console_regs_dump()` rather than
from diag.c directly, because cli.c is still not linked into the register-trace harness
(decision 1, `tests/trace/README.md`) - keeping the indirection there means the "regs"
golden keeps the harness's stub line for this section unchanged, and no `.sources` file
needed `uart.c` added.
The console never
reads the buffer directly;
it uses `capture_completed_half()`, or `capture_oneshot_n()` when it needs a window that
nothing is writing.

## Build and verify - every change, both variants

Command-line build (paths in `tools/build.bat`; `-mdfp` must point at the pack's `xc16`
subdirectory, `-T` at the linker script inside the pack):

```
tools\build.bat        hardware  -> build\adc_dma_40msps.elf/.hex   (must be -Wall -Wextra clean)
tools\build.bat sim    simulator -> build\adc_dma_40msps_sim.elf    (same)
```

`src/app/version.h` (git-ignored) carries the git revision for the banner.
`tools/version.bat` writes it before every build: called by `build.bat`, by the
`.build-pre` hook in `adc_dma_40msps.X/Makefile` (which the IDE runs, so a colleague's
build gets it too) and, for `tools/Makefile`, by `tools/version.sh`. `board.h` includes
it through `__has_include` and falls back to "unknown"; it sits next to `board.h` so
that `"version.h"` resolves through the including file's own directory and no build
needs an extra include path for it (P1.1 moved it there from the repository root). Two
pitfalls, both hit on 23.09.2026 with
MPLAB X 6.35: do **not** put the step into `configurations.xml`
(`makeCustomizationPreStep`) - the headless makefile generator then silently writes no
`Makefile-*.mk` at all; and in the hook use exactly
`cmd /c "$(subst /,\,$(CURDIR))\..\tools\version.bat"` - the IDE's make reports
`SHELL=sh.exe` without having one, and neither a quoted relative path nor
`cd ../tools &&` reached cmd intact (`'..' is not recognized`).

MPLAB X project: configurations `EV74H48A_Curiosity_Platform_MPS512` (the Curiosity
Platform board, PKOB4, `dma.c`), `EV17P63A_Curiosity_Nano_MPS506` (the Curiosity Nano:
device dsPIC33AK512MPS506, `nEdbgTool`, `BOARD=2`) and `sim` (Simulator, `sim_dma.c`,
`__MPLAB_DEBUGGER_SIMULATOR=1`); hardware configurations are named after the evaluation
kit, order number first, so that the name in the IDE says what gets programmed. Command
line: `tools\build.bat`, `tools\build.bat nano`, `tools\build.bat sim`.
`tools\_test_mplabx.bat` builds the Curiosity Platform configuration from the command
line through MPLAB X's own makefile generator. Two pitfalls: the generator rewrites
`languageToolchainVersion` in `nbproject/configurations.xml` to whatever compiler it
finds first - restore that one line, never `git checkout` the whole file (that once
threw away a file-list change); and after any change to the file list delete
`adc_dma_40msps.X/build` and `dist`, otherwise make links stale objects and a missing
entry goes unnoticed. A third one, hit on 27.09.2026 (P3.7): `_test_mplabx.bat` runs
the generator only when `nbproject/Makefile-impl.mk` is missing, so the git-ignored
`nbproject/Makefile-*.mk` keep an old file list and old include path (they were from
before P4.1 and failed with `log.h: No such file`) - after a change to the file list or
the include directories delete those generated makefiles too, so that the generator
runs again (and then restore `languageToolchainVersion`).

GUI tool: `tools\gui_setup.bat` once, then `tools\adc_gui.bat --fake`; `python
tools\adc_gui.py --selftest` is its check.

The simulator has no PLL, no ADC, no DMA and dispatches no interrupts (any pending
interrupt aborts with E0110). It proves the ping-pong buffer logic and nothing else. It
also runs `__delay32()` at a small fraction of real time: the 100 ms time-base check
(20 M cycles) took longer than the whole test budget and looked like a hang after the
self-test (24.09.2026, in `tools/sim_trap.py` and in the MPLAB X simulator alike), so
the simulator build delays 1 % of that. Keep every other long wait out of the simulator
path the same way.

```
tools\build.bat sim
python tools\sim_trap.py --run-seconds 420        expect "[simtest] PASS"  (about 7 minutes)
tools\build.bat sim 256 && python tools\sim_trap.py --elf build\adc_dma_40msps_sim256.elf --run-seconds 420
                                                  the same at 256 samples per half
python tools\sim_trap.py --fault 65536            expect "[simtest] FAIL", one mismatch at index 0
```

**The simulator acceptance run may run without asking since 27.09.2026** (user's
standing permission; until then it ran only on request, 24.09.2026). It takes about
seven minutes of wall clock: run it where the plan calls for it (P9.5, P12.4) or where a
change needs it, one simulator run at a time, never several in parallel.

**The smoke run [SMOKE] is the short one** (`docs/IMPLEMENTATION-PLAN.md` P0.7, rule 6):
the simulator build with `-DSIM_SMOKE=1` boots exactly like the normal simulator build,
then - instead of starting the stream - types `help`, `version`, `status` into the
parser through `cmd_parser_feed_char()` (`smoke_run()` in `main.c`; the simulator's
UART receiver takes no injected bytes, so this is the only way in) and prints
`[smoke] DONE`. It proves that boot and console reach the far side without a CPU trap
after a change to either; it says nothing about the ping-pong logic (that is the
acceptance run) or about registers (that is `tools\trace.bat`).

```
tools\build.bat smoke                     -> build\adc_dma_40msps_smoke.elf       (-Wall -Wextra clean)
python tools\sim_trap.py --smoke          expect "[smoke] PASS"; console text in build\smoke.log
tools\build.bat smoke fault [n]           -> build\adc_dma_40msps_smokefault.elf  (SIM_SMOKE_FAULT=n: a
python tools\sim_trap.py --smoke --elf build\adc_dma_40msps_smokefault.elf --log build\smokefault.log
                                          deliberate trap after the script; expect "[smoke] FAIL")
```

The negative test has four cases (`smoke_run()`, `main.c`): 1 = the plan's misaligned
32-bit read, 2 = a read from an unmapped address, 3 = the stack pushed past SPLIM,
4 = a call into an illegal opcode. **The simulator (MPLAB X v6.35) raises no trap for
1 and 2** - the read returns 0 and the firmware reaches `[smoke] DONE`, so only the
expected.log diff fails the run. Case 3 sets `INTCON1.STKERR` and the simulator aborts
with E0110 at the dispatch; case 4 aborts with W0014 "Invalid opcode". `build.bat smoke
fault` therefore defaults to case 3 (the one real trap), and `sim_trap.py --smoke` reads
INTCON1 after the halt for exactly that outcome. Do not read a clean smoke run as "no
misaligned access anywhere": that trap only exists on silicon.

`sim_trap.py --smoke` fails on a `[TRAP]` block or fail code in the console text, on
`trap_seen`/`fail_code` != 0 after the halt, on a simulator error message, on no
`[smoke] DONE` within `--smoke-timeout` (180 s), and on any difference from
`tests/smoke/expected.log`. The comparison masks the three lines that carry `BUILD_ID`
(`[boot] adc_dma_40msps <date> <time> git ...`, `build: ...`, `[build] ...` - date, time
and revision differ between any two builds, and even between the three lines of one
build, because `__TIME__` is per translation unit) on both sides; everything else must
match byte for byte, ACK bytes included. A task that changes the console on purpose (a
new command in `help`) runs `python tools\sim_trap.py --smoke --update-expected` and
commits the new `expected.log` with the change; the diff it prints first is the review.
Measured 27.09.2026 (`tests/baseline.md`): 82-96 s wall clock per run, of which MDB
start-up and programming are 35-40 s and the run itself 40-49 s (4.3 k UART characters
at about 10 ms each); the firmware side is under a minute, so under rule 6 the smoke run
may run without asking - **confirmed by the user on 27.09.2026**: run it without asking
after tasks that touch boot, console or memory layout, one run at a time (four in
parallel took 131-136 s each). This covers `--smoke` only, never the ~7-minute
acceptance run. The smoke path is preprocessor-guarded (`SIM_SMOKE`,
`SIM_SMOKE_FAULT`) and `tools\fncmp.py --ignore-strings` shows the hardware, sim and nano
ELFs function-identical with and without it. No MPLAB X configuration exists for it; the
command line is the way to build it.

## Rules

- Say what has and has not run on silicon. `docs/HARDWARE-LOG.md` is the record; add a
  dated entry for every board run and for every change made in reaction to one,
  including predictions that turned out wrong.
- A board run costs a person their afternoon. Before asking for one, make the firmware
  answer as many open questions as it can in a single pass.
- Every register value gets the datasheet table or page it comes from, in the comment
  next to it. A value copied from MCC or from Microchip's example says so. Where the
  device pack's ATDF and the datasheet disagree, the ATDF has been right every time -
  see the SCCP trigger code below.
- Status flags are cleared by writing the word once with 0 only in the bits to clear
  (`dma0_clear()`), never with bit-field read-modify-write; interrupt flags are cleared
  at the start of the handler, not the end. Both were real bugs on the board.
- Two configuration-bit names are pack-version dependent (`FICD_NOBTSWP`,
  `FWDT_RCLKSEL`) and are written as numbers. Keep it that way.
- Console output for humans goes through `console_puts()`/`console_kv()`; command replies
  through the parser's sink. A line longer than a buffer is a stack overrun: size buffers
  from the longest possible line and say so in the comment.
- Commit messages: what changed, why, what was verified. **No attribution trailers of any
  kind**, in this repository without exception.
- Do not edit `cmd_parser.c/.h` - with one deliberate exception:
  `CMD_PARSER_MAX_COMMANDS` is 32 instead of upstream's 16 (24 on 24.09.2026 for `core`,
  `dac`, `dactest`; 32 on 25.09.2026, when `chain` and `stream` had filled all 24 slots).
  4 bytes of RAM per slot. When updating the parser from github.com/zabooh/cmd_parser,
  re-apply that one line. `help` takes a slot too, and `cmd_register()` fails silently
  when the table is full: with `chain`/`stream` (the chain test) and `snap`/`rate`/`blk`
  (binary transfer, below) both merged in, 26 commands + help = 27 in use, 5 free.
  **`stream grab` (the GUI's halt/transfer/restart cycle, below) is a sub-command of
  `stream`, dispatched inside `cmd_stream_fn()`, exactly like `stream on`/`off` - it did
  not need a 27th slot, and was chosen that way on purpose to keep the 5 free.**

## Binary block transfer (`snap`, `rate`, `blk`) - firmware only, since 25.09.2026

`docs/PLAN-BINARY-TRANSFER.md`: a `blk <n>` command sends a contiguous block of up to
2048 samples as binary with a text header and a CRC-16 line (`crc16.c/.h`, the frame
itself built by `frame_send()`, `src/lib/frame.c` since P6.3, 27.09.2026); `snap` fills
the buffer once and stops so the block being read is not being overwritten; `rate <ksps>`
sets the sample rate directly. `snap`/`rate`/`blk` and their registration
(`link_register()`) moved out of `cli.c` into `src/link/gui_link.c` on 27.09.2026
(P6.4) - `cli.c` still calls `link_register()` from `cli_init()`, now an extern. These
are the back-to-back commands; the firmware keeps them for a terminal, but
`tools/adc_gui.py` no longer sends any of them (owner's decision, 25.09.2026: the
triggered chain, `stream grab`'s `GRAB` frame below, is the GUI's only data path now).
An optional `baud` command to raise the UART rate is designed
(`docs/PLAN-BINARY-TRANSFER.md` step 6) but not yet implemented.

## The GUI's chain stream cycle (`stream grab`, since 25.09.2026)

The goal: from the GUI, configure and start the chain stream, and then, continuously and
automatically, halt it, transfer a contiguous window, restart it, visualise and analyse -
until the user stops it. Since 25.09.2026 this is the GUI's ONLY capture path: the
back-to-back capture tile, the PLL rate selector and the sweep tile were removed from
`tools/adc_gui.py` (owner's decision - back-to-back is obsolete), and what used to be a
second "chain stream" tile is now the one and only acquisition card, with one chart and
one set of evaluation chips. `chain_stream_on_input()` (`src/app/acquisition.c` since
P9.4, 27.09.2026; `chaintest.c` before that - cli.c's
`stream on <ksps> <core> <pinsel> [<samc>]`) lets the GUI point the same cycle at any ADC
input, not only the DAC2 test triangle on RA8 - the DAC is then left alone (`slp=0` in
the frame) and the GUI plays a sine with harmonics through `--fake` instead, so
SNR/THD/harmonics still show something with a custom input. Design decisions, all
documented where the code that implements them lives, restated here because they were
not obvious:

- **The halt does not tear the chain down.** `capture_chain_halt()`/`_resume()`
  (`capture.c`) stop and restart the SCCP1 trigger only - the DMA channel stays armed,
  the ADC core and clock tree untouched. What is sent is the half that completed last
  (`capture_completed_half()`), the SAME mechanism `capture_service()` (the main loop)
  already reads from continuously - not a new "grab a few blocks and hope" scheme like
  `grab_window()` in the chain test's own S5 stage. Reusing it means the grab cycle
  inherits everything already proven about that half being complete, contiguous and not
  overwritten before it is read.
- **The frame is `blk`'s framing, with a wider header**, so the GUI's existing binary
  reader (`Target._read_line`/`_read_exact`, `crc16.c`) needed no changes, only a second
  regex: `GRAB n=<count> from=<from> ksps=<ksps> ov=<overrun> late=<late>
  missed=<missed> halves=<halves> xfer=<transfers> slp=<slpdat> dachz=<dac_hz>`, then the
  payload and a CRC line exactly like `blk` - both built by the same `frame_send()`
  (`src/lib/frame.c`, P6.3), called from `gui_link_stream_grab()` (`src/link/gui_link.c`,
  P6.4; `cli.c`'s `cmd_stream_fn()` still owns "stream on|off|grab" dispatch and calls
  it for "grab") and from `parse_grab_frame()` (`tools/protocol.py`) on the GUI side.
- **The counters are per cycle, not the running total.** `chain_stream_grab_begin()`
  keeps its own baseline of `dma_overrun`/`late_service`/`proc_missed`/`blocks_done`/
  `capture_transfers()`, zeroed when `chain_stream_on()` starts the stream (it already
  calls `counters_clear()`), and reports the delta since the previous grab. A colleague
  watching the GUI wants to know what happened in the window just shown, not a number
  that only grows.
- **The restart always runs**, whether the transfer went out whole or was cut short
  (Ctrl+C, a disconnect): `gui_link_stream_grab()` (`src/link/gui_link.c`) calls
  `chain_stream_grab_end()` unconditionally after the payload loop. If the restart itself fails,
  `chain_stream_off()` is called and the stream is left off cleanly - `stream` then
  reports it, and the GUI is expected to send `stream on` again. The chain is never left
  half-configured.
- **The DAC triangle's range is not in the frame** - only `slp` (SLPDAT) and `dachz` are.
  `acq_triangle_for()` (acquisition.c, since P9.4b) always picks the widest range the fixed limits in
  `dac.h` and `slp` allow, so the GUI reconstructs the same range from `slp` alone
  (`chain_triangle_range()`, `adc_gui.py`) rather than transmitting two more numbers.
- **One evaluator, one place.** The GUI's chain tile imports `tri_eval`/`grid_ok` from
  `tools/eval_chain.py` - the same module `chain all` logs are judged with - instead of a
  second implementation that could quietly disagree with the firmware's own. The fake
  target's synthetic triangle uses the same module's `synth()`.
- **Tested without a board**, per the design brief: `python tools/adc_gui.py --selftest`
  exercises `FakeTarget.grab()`/`parse_grab_frame()` for a refusal before `stream on`, a
  clean test-triangle grab that passes the grid check with the frame's own actual rate
  used as the FFT's fs, the next grab landing in the OTHER buffer half (`from > 0`), a
  lost-sample grab that correctly fails, a custom-input grab (`stream on <ksps> <core>
  <pinsel> <samc>`) with `slp=0` and a real FFT peak from the fake sine, a CRC mismatch, a
  truncated frame, and `Target.grab()` timing out against a serial stub that never
  answers - the same bounded-wait code path a real disconnected board would hit.
  `tools/gui_ui_test.py` (Playwright, headless Chrome) additionally drives the page
  itself against `--fake`: connect, LIVE with the test input (fs follows the rate, a PASS
  verdict), a rate change while LIVE, LIVE with a custom input (a spectrum with a
  fundamental, the triangle card hidden), STOP, SINGLE, no server-side exception. Not
  tested without a board: whether `capture_chain_halt()` really lands cleanly on real
  silicon timing, and whether the halt/grab/restart cycle holds up at the higher rates
  (`docs/HARDWARE-LOG.md`, 25.09.2026 entry).

## How the example is built, and what that costs

**Back-to-back is the working path.** The ADC runs in Integration mode (`MODE = 2`) with
`CNT` conversions per burst, `TRG1SRC = 1` (software start) and `TRG2SRC = 2`
(back-to-back), `IRQSEL = 0` so that every conversion raises the event the DMA triggers
on. The DMA is channel 0, Repeated One-Shot (`TRMODE = 1`, one transfer per trigger),
its address window exactly the buffer, with HALF and DONE interrupts and guard words
behind the buffer. **Never `TRMODE = 3`**: Repeated Continuous copies a whole block per
trigger at DMA speed - it was set until 25.09.2026 and was the cause of every "40 MSPS
whatever the setting", every overrun storm and the "few transfers per trigger"
(run 18, `dma.c`).

**The rate comes from PLL1, not from the CLKGEN6 divider.** `capture_set_pll(p1, p2)`
sets PLL1's two output dividers; the ADC clock is 1600 MHz / (p1 * p2), p1 >= p2, both
1..7, which is 40 down to 4.08 MSPS with 5/5 = 8 MSPS exactly. `clock_adc_set_rate()`
searches `PLLFBDIV` (63..200, VCO 500-1600 MHz) on top of that gear for a finer step.
PLL1 feeds nothing but the ADC path, and its output-divider switch is what
`clock_init()` does at every boot, so the mechanism is known to work on this silicon.
What is **not** settled is whether it reaches the converter during continuous streaming -
see the open questions.

The CLKGEN6 divider (`capture_set_clkdiv()`, the `clk` command) is kept for the record
only. Every ratio was written, read back and confirmed by `DIVSWEN` and `CLKRDY` - with
the generator switched off around the write and with it left running as Example 12-2
prescribes - and the rate never changed. **Do not build a rate on it.**

Both switches run in the boot order and report the step that failed rather than a bool:
DMA channel down, ADC core off, clock changed and read back, core on, DMA from scratch.

**The pacing variants are in the code again, as a matrix, not as a claim.** They were
deleted after run 7 and brought back on 24.09.2026, because the reason they had failed
turned out to be three errors of ours rather than the silicon:

- the SCCP trigger code was "corrected" from 32 to 34 on the strength of datasheet
  Table 16-4; the pack's ATDF names `0x20` = 32 "SCCP1 OC/IC Event" and `0x22` = 34
  **SCCP3**, so the ADC was told to listen to a module nothing had configured;
- `CCP1CON2.AUXOUT` carried the timer rollover (1) instead of the special event trigger
  (**2**), which is what the ADC's trigger input expects;
- the module was clocked from the peripheral clock (PLL2, `CLKSEL = 0`) while the ADC
  runs off PLL1; it must be CLKGEN13 (`CLKSEL = 1`).

Each one alone was enough for "no conversion at all", which is exactly what runs 5 to 7
reported. **The triggered family has therefore never really been tested.** `capture.h`'s
`capture_variant_t` lists all of them and `test matrix` walks them.

**Nothing runs by itself.** The firmware boots, brings the console up, sets the slowest
rate (`ADC_PLL_POSTDIV1/2` = 7/7) and waits: `[boot] READY - nothing is converting`.
Everything else is typed. The reason for the idle boot is that through seven runs the
console never received a byte (`rx = 0`) and it could not be told whether the bytes never
arrived or whether the receive interrupt (priority 1) was starved behind the DMA
interrupt (priority 4, over a million entries per second). With nothing converting, that
question answers itself - and it did: the console is fine.

**The sweep runs from the slowest rate up**, not from the fastest down. The slowest point
is the one the DMA should manage, so the first row is the row most likely to pass - and a
failure there means the chain is broken, not the rate. The ladder mixes integer and
fractional ratios on purpose: a row that lands halfway between its neighbours proves the
fractional part works, one that snaps to a neighbour proves it is ignored.

## The test suite

`test` on its own lists the parts. `test all` is the whole examination in one pass,
because a board run costs a person:

| Subcommand | The question it answers |
|---|---|
| `test self` | is the chain wired up (it samples a constant, so it cannot tell a working converter from a frozen result register) |
| `test clock` | do divider writes arrive - and only that; the clock that comes out is a different question |
| `test clkoff` | does the ADC still convert with CLKGEN6 switched off entirely |
| `test bursts` | **does the rate depend on how many bursts run** - 1, 10 and 100 at the same setting, the bridge between "one burst follows the setting" and "a thousand do not" |
| `test sweep` | the rate ladder with all counters per row |
| `test rate` | one rate point, measured |
| `test matrix` | every variant: does it convert, does the rate follow, **does it stream** (the acceptance test), are the data intact |
| `test dac` | the DAC triangle through the internal UREF route |

**The chain test (`chain all`, since 25.09.2026) is the run the colleague makes.**
It tests the target architecture of ANALYSIS.md C.8 - SCCP1 -> ADC core 5 in Single
mode -> DMA0 -> ping-pong -> CPU, DAC2 on RA8 as the signal - in stages S0..S9 within a
minute, and `tools/eval_chain.py` evaluates the log it sends back. Plan, stages and
the decisions behind them: `docs/CHAIN-TEST-PLAN.md`. Its triangle evaluator was
tested on the host before any board run (gcc on the code extracted from chaintest.c,
synthetic windows with DNL, noise and a modelled DAC filter): no false alarm in 2100
clean windows, a single lost or repeated sample found in 96-100 % of windows; the
Python port in eval_chain.py gives identical results on the same data. The `test ...`
suite below is the older back-to-back instrument and stays as it is.

`matrix_stream()` in `cli.c` is the acceptance test and it is the one that matters: the
stream runs for `MATRIX_STREAM_HALVES` halves with `capture_service()` called throughout,
exactly as the main loop does, and it passes only when `overrun`, `late` and `missed` are
all zero. `missed` above zero means the CPU never saw a half, which is precisely the
failure the example must not have. The matrix ends either with `USE THIS ONE: <variant>`
or with the plain statement that none of them streams cleanly.

The DAC test is the only instrument that looks at data rather than counting events. It
goes through the chip, not over a pin: `UREFCON.INSEL = 7` puts DAC2 on the internal
reference line, which every ADC core can read as `ANn7`. It runs twice per variant - as
an isolated burst and as the 100th burst of a stream - because the triangle's period in
seconds is a property of the DAC and cannot change. If the measured rate rises by ten and
the period in samples rises by ten as well, the samples in the stream are repeats and the
converter never sped up.

## What the board has settled (as of run 19, 25.09.2026)

- **The chain streams up to 8 MSPS** (run 19): SCCP1 -> ADC core 5 -> DMA0 -> ping-pong ->
  CPU, 15 s at 8 MSPS, 120 M samples, overrun, late and missed 0, the CPU processing every
  half. The DAC triangle comes through without a lost or repeated sample up to 10 MSPS,
  and its slope matches the model to 0.1 % (open question 4 closed).
- **Limits:** a few hundred DMA overruns per 0.5 M transfers from 10 MSPS; lost triggers
  from 16 MSPS; above that the ADC converts at only 18 to 20 MSPS in triggered single
  mode (every second trigger lost at 40 MSPS).
- **The CLKGEN6 divider divides** (clock monitor: 320/160/80 MHz), and `CLK6CON.ON = 0`
  does not stop the generator. Back-to-back streaming follows the PLL setting (8 MSPS: one
  burst and the 100th give the same slope). Open questions 1 and 3 are closed.
- **The processing cost was the limit at 8 MSPS:** the first loop took ~23 cycles per
  sample, 92 % of the half period. Rewritten 25.09.2026 (32-bit reads, unrolled); run 20
  will say by how much.

- **The DMA was misconfigured from the first day.** `TRMODE = 3` (Repeated Continuous)
  makes one trigger start back-to-back transfers until the block is full (DS70005591D
  13.4.8.4/13.4.8.5, p833 f.). Run 18: 6144 transfers for 15 conversions at 100 kHz,
  about 41 M transfers/s at every rate. Fixed to `TRMODE = 1` (Repeated One-Shot). Every
  rate and overrun figure of runs 1 to 18 was taken with the wrong mode.
- **SCCP1 -> ADC works** (run 18, S2): one result per SCCP1 period at 1, 10 and 100 kHz,
  timer mode, CLKGEN13 measured at 160 MHz. The clock monitor reads CLKGEN6 at 320 MHz and
  CLKGEN7 at 400 MHz. Output-compare mode produced no events; its CM codes for the PLL
  outputs (0xB/0xC/0xE) do not select what the ATDF says - 0xC read 160 MHz, i.e.
  CLKGEN13.

- **The chain carries data, complete and in order.** Run 14 captured the DAC triangle
  through UREF: a clean monotonic fall from 3728 to 629, no reversal, largest step 90
  counts out of a swing of 3240 against a limit of 405. Across the half boundary and
  across the burst restart.
- **A single burst runs at the rate the PLL is set to**, better than 0.5 % over all
  fourteen settings from 4 to 40 MSPS, with a constant residual of 4.2-5.3 us that a
  rate-dependent error could not have produced.
- **The counters are honest.** Run 16: `half + done = 2 x bursts` in every row. There is
  no double booking.
- **The console works.** `rx = 0` in runs 6 and 7 was interrupt starvation.
- ~~The CLKGEN6 divider does not reach the converter~~ - **withdrawn** (ANALYSIS.md C.3):
  both observations behind it were made with instruments later found void. Every
  document names CLKGEN6 as the ADC clock. The chain test's S8 measures it at the
  generator itself with the clock monitor.
- **Two clocks ran out of specification until 25.09.2026** (Table 40-24, p2016): the DAC
  at 320 MHz (minimum 400) and SCCP1 at 320 MHz (maximum 200). Now DAC on the PLL1 VCO
  divider at 400 MHz, SCCP1 on CLKGEN13 = PLL1 out / 2 = 160 MHz. And the DAC's data
  registers need an update trigger (`UPDTRG`, p1409) that was never set; it is now 11
  (every write taken at once).
- **Every rate measured under load in runs 4 to 11 is wrong** by about a factor of ten -
  it was taken by a CPU drowning in the overrun interrupt. Do not quote those numbers.
- **`dma_overrun` is a lower bound, not a count of lost samples.** `OVERRUN` is one bit
  in `DMA0STAT` and the handler increments once per entry in which it finds it set;
  several losses between two entries count as one.

## Open questions (as of 25.09.2026, after run 19)

Questions 1, 3 and 4 below are answered by run 19 (see "settled" above and the
HARDWARE-LOG); question 2 is answered up to 8 MSPS. Still open: the DMA overruns from
10 MSPS (bus contention with the CPU?), the ADC's triggered ceiling of ~18-20 MSPS, the
processing budget, and two instrument faults (output-compare mode of SCCP1, the clock
monitor's CNTSEL codes for PLL outputs). The list as it stood before:

1. **Is the rate selectable in continuous streaming?** This is the example's central
   claim and it is not settled. A single burst follows the setting; a thousand bursts
   delivered about 40 MSPS at every setting, three per cent apart while the setting spans
   a factor of ten. Two readings remain: the converter really runs flat out under
   streaming, or each conversion lands in the buffer more than once - which Microchip
   acknowledges for this silicon ("ADC triggers for DMA on this device have an issue. A
   few transfers are possible per one trigger."). `test bursts` and the twice-run DAC
   test are built to decide it.
2. **Can any variant stream without loss while the CPU processes?** No variant has done
   so in any run. At every rate the sweep walks, 1614 to 1977 of 2000 halves were missed
   by the main loop, because every lost sample raises the DMA channel interrupt and that
   event has no enable bit of its own (DS70005591D 13.6.1 - `DMA0CH` has HALFEN, DONEEN
   and MATCHEN only). The handler brakes itself past `OVERRUN_LIMIT` and reports the rate
   as unusable. **Until one variant streams cleanly, the example does not demonstrate
   what it says it demonstrates, and that belongs on the console rather than in a
   footnote.**
3. **Why does the ADC ignore CLKGEN6?** Table 16-1 names it as the clock source, its
   divider has no effect in either switching sequence, and the converter keeps running
   with the generator off. A question for the product line; the register evidence is
   complete in the log.
4. **`dac_period_ns()` is still wrong even after the two-slope fix.** 25.09.2026 fixed
   one factor of 2 (a period is two slopes, not one - `dac.c`), but run 13 measured 513
   us against a formula that claimed 54.9 us, a gap the factor of 2 does not close. And
   `DACLOW` is not reproduced - the upper end matches `DACDAT` exactly, the lower end
   does not. Nothing is judged against it any more (`dactest.c` compares against a
   *measured* period instead), but it should be corrected.
