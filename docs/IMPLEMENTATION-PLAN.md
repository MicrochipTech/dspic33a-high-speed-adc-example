# Implementation plan: version N+1

Written 26.09.2026 at revision `28e88fa`. Work started the same day; the progress is
tracked in "Status" below. Nothing of N+1 has run on silicon.

Scope and decisions: `docs/DESIGN-MULTICHANNEL.md` (section 7, decisions; section 8,
version plan) and `docs/REFACTORING-PROPOSAL.md` (V1..V10, section 7, working without
hardware).

**What N+1 is:** the firmware restructured into portable modules, with the routing
core in place and the Goertzel and wavegen libraries included but unused. **What it
does for the user is exactly what it does today:** `stream on`, `stream grab`, `blk`,
`chain all`, the `test` suite, the GUI.

**Constraint:** no board. Every task has to be verified as behaviour-preserving
without silicon. N+1 as a whole stays "not run on silicon" until the first board run
reproduces today's `chain all`.

## Status

**Every task updates its own row here in the same commit that does the task.** The
commit message stays the detailed record; this table is the one place to see where the
work stands. `done` = committed and re-checked by the lead session (builds, `trace.bat`,
`hosttest.bat`, goldens, no trailer).

As of 28.09.2026. Done: 51 of 52 tasks in the N+1 scope (63 planned plus P0.9 = 64; P8 and P10, 12
tasks, moved to N+2 on 27.09.2026); P0.5b and P4.6a were added along the way and are
not counted.

| Task | Status | Commit | Model | Note |
|---|---|---|---|---|
| P0.1 Baseline | done | `6224d6d` | Sonnet | hw 83.4 KB flash / 14.8 KB RAM; figures from the map file (no size tool in xc-dsc 3.31) |
| P0.2 Host test harness | done | `0a84aa6` | Sonnet | MinGW gcc 16.1; `tools\hosttest.bat` |
| P0.3 Trace spike | done | `f1c5ad5` | Opus | recommended the page guard; the user chose (a), see Decisions |
| P0.4 Trace harness | done | `a9218be` | Sonnet | snapshot diff in C |
| P0.5 Golden traces | done | `51e0181`, `f294834` | Sonnet | 13 scenarios; cost 169 M tokens, a card cut too large |
| P0.5b Read hook, fast `trace.bat` | done | `8089b6d`, `49dae4d` | Sonnet | race and retries gone; `trace.bat` 5 min 19 s -> ~7 s; goldens byte-identical |
| P0.6 `fncmp.py` | done | `c25c07e`, `07b6763` | Fable | `_DMA0Interrupt` 42 instructions, 0 indirect calls |
| P0.7 Smoke build | done | `e777678` | Fable | 82-99 s per run; the simulator does NOT trap a misaligned read - the fault case is a stack overflow |
| P0.8 Header cross-check | done | `3f43aa2` | Fable | ATDF check in every `trace.bat` run, 0 errors both devices; simulator: 0 disagreements |
| P0.9 ATDF reset values | done | `3adafc9` | Fable | added task; 89 golden lines changed, all explained; `PR1` write now invisible (equals reset value) |
| P1.1 Move into `src/` | done | `2c4a6f9` | Fable | fncmp: 0 functions differ in hw, sim, nano, smoke |
| P2.1 `lib/fmt` | done | `59e48d4` | Fable | |
| P2.2 `lib/stats` | done | `28a5c13` | Fable | no volatile; all callers read the completed half; `n = 0` still unguarded (as before) |
| P2.3 `lib/tri_eval` | done | `cacc592` | Fable | 0 false alarms in 2100 clean windows, 100 % detection with the fault in the middle half (95.4 % anywhere) |
| P2.4 C/Python cross-check | done | `009ba51` | Fable | 306 windows identical |
| P3.1 Reference models | done | `de0120e`, `dd7951f` | Fable | damped form and hysteresis, see Decisions |
| P3.2 `lib/iir1` | done | `dee0638` | Fable | exact against the reference |
| P3.3 `lib/goertzel_f` | done | `796408d` | Fable | max. deviation 1 LSB |
| P3.4 `lib/goertzel_i` | done | `c015270` | Fable | max. deviation 1 LSB |
| P3.5 `lib/detect` | done | `355f237` | Fable | exact counts 4/4/0/3, two channels independent |
| P3.6 `lib/wavegen` | done | `c926a69` | Fable | 0 of 1536 values differ from the reference |
| P3.7 Libraries into the builds | done | `8376a5b` | Fable | +2932 B flash, +8 B RAM; libm already linked by default, only `expf` lands (sin/cos are FPU instructions) |
| P3.8 Cycle count (optional) | open | | | |
| P4.1 Port layer | done | `2f4c2cd` | Fable | |
| P4.2 timebase, led | done | `7725dce` | Fable | nothing to move |
| P4.3 sccp | done | `702fe95` | Fable | |
| P4.4 dac | done | `c8d17f7` | Fable | |
| P4.5 adc | done | `0ccdd74` | Fable | `port_trace*` added (keeps the `BOOT_VERBOSE` gate); `adc_init(.., burst_len)` |
| P4.6a `port/wait.h` | done | `529c6a3` | Fable | added task: one bounded-wait macro instead of per-driver copies |
| P4.6 dma | done | `49497b0` | Fable | ISR unchanged, 42 instructions |
| P4.7 clock | done | `6acd0fa` | Fable | `clock_fail_hook()` returns the boot stage; strong version in `port_impl.c` |
| P4.8 Register visitor | done | `6e8d4b6` | Fable | `port/regs.h`: one visitor `(name, v, fmt)` with `REG_HEX/REG_DEC/REG_TITLE` reproduces the old dumps character for character (`regs` golden unchanged); the drivers keep no print call, only the three callers and the six dump functions change |
| P5.1 `src/drivers/uart.c` | done | `1b82b9f` | Sonnet | `_U2RXInterrupt` moved out of cli.c, 65 -> 55 instructions (the byte-counting/CR-LF/parser-feed body became `uart_rx_hook()`, a direct `rcall`, still 0 indirect calls); uart.c reports through neither `port/log.h` nor `port/wait.h` - it is what `console_puts()` writes through |
| P5.2 `cli.c` on top of `uart.c` | done | `8c77890` | Sonnet | P5.1 already finished the switch (the plan allows that order); this task is the check: `grep -E "U2\|RPCON\|RPOR\|RPINR\|IPC" src/cli/` empty, **[SMOKE]** log identical to `tests/smoke/expected.log` (110 lines) |
| P6.1 Per-module command registration | done | `e7826bf` | Sonnet | `bench_register()` (sweep, clk, pll, test - contiguous in the old order, clk/pll stay in cli.c after P6.2), `link_register()` (snap, rate, blk), `chain_register()` (chain, stream); `cli_init()` calls them in the old position, `help` order unchanged (**[SMOKE]** matches `tests/smoke/expected.log`) |
| P6.2 `src/tests/bench.c` | done | `9408731` | Sonnet | `test_*`/`matrix_*`/`sweep_*` moved verbatim, with `bench_register_sweep()`/`bench_register_test()` (split in two: `clk`/`pll` still sit between "sweep" and "test" in the registration order); `put_kv`/`put_line`/`arg_u32`/`usage`/`run_dactest` stay in cli.c, no longer `static`, called `extern` from bench.c; traces `variants`/`b2b` unchanged, fncmp 379/426 functions identical, the rest explained by the move and the linkage change |
| P6.3 `src/lib/frame.c` | done | `e52c234` | Sonnet | `frame_send()`: header (caller-built) + chunked payload with the CRC folded in + CRC tail, or the bare `CRC 0000` shape for n=0 - byte-for-byte what `cmd_blk_fn()`/`cmd_stream_grab()` build today, not yet wired in (cli.c untouched); `tests/host/test_frame_xcheck.py` cross-checks a dumped frame against `tools/protocol.py`'s `parse_grab_frame()` and a small local BIN-header parser, CRC and payload identical |
| P6.4 `src/link/gui_link.c` | done | `475462e` | Sonnet | `cmd_blk_fn`/`cmd_stream_grab`(`->gui_link_stream_grab`)/`link_register` moved, `blk`/`stream grab` rewritten over `frame_send()` - wire bytes proven identical by P6.3's host cross-check plus fncmp showing only the moved/renamed functions differ; `cli.c`'s `cmd_stream_fn()` keeps "stream on\|off\|grab" dispatch, restart-always rule untouched; `_DMA0Interrupt` 42/0, `_U2RXInterrupt` 55/0 unchanged |
| P6.5 `tools/protocol.py` | done | `88b00d6`, merge `aa1498c` | Sonnet | built in a worktree alongside P5; GUI `--selftest` 15/15 before and after; `eval_chain.py` had no copy to redirect; `gui_ui_test.py` not run (Playwright not installed in any Python on this machine) |
| P7.1 Board config as data | done (reduced) | `b708584` (ported from `ea93510`) | Sonnet | only the boot PLL dividers moved; ADC core/input, LED, UART PPS stay board.h macros (compile-time uses, hot path led_toggle, PPS unverifiable without a board run) - `grep board.h src/drivers/` still finds hits; revisit in N+2 with P8 |
| P8.1-P8.8 Drivers with instances | moved to N+2 | | | user decision 27.09.2026, see Decisions; `ROUTE_STREAM` works with today's single instances |
| P9.1 `src/app/pingpong.c` | done | `4d40f82`, merge `12d2383` | Sonnet | `pingpong_on_half()` static inline; `dma0_event` 124 instructions before and after, `_DMA0Interrupt` 42/0; `late`/`overrun` stay capture.c globals (two literal designs measured slower) |
| P9.2 Simulator hooks out of pingpong.c | done | `d868bbb`, merge `12d2383` | Sonnet | pingpong.c had none to move; [SIM] run once already: `[simtest] PASS`, 100 halves, 0 mismatches |
| P9.3 `src/meter/meter.c` | done | `37e9484` | Sonnet | `capture_process_bench/_selftest/_clkoff_probe/_oneshot/_oneshot_n/_measure_rate` moved verbatim (two like-for-like substitutions: direct `buf`/`half_len` reads became `capture_buffer()`/`capture_half_len()` calls); `process_buffer()`/`wait_for_blocks()`/`oneshot_left`/`oneshot_ticks` stay in capture.c, made non-static, reached through `capture_priv.h` (guard_check()/dma_buffer stay private, oneshot_left is also touched by `dma0_event()`); `_DMA0Interrupt` 42/0, `_U2RXInterrupt` 55/0 unchanged, `dma0_event()` 124/124 instructions (only a symbol-label change); traces 13/13, `b2b`/`variants` byte-identical, `stream_on`/`stream_on_input` needed `meter.c` added to their `.sources` (chaintest.c calls the moved functions) but their goldens are unchanged; [SMOKE] PASS |
| P9.4 `src/app/acquisition.c` | done | `911cdb9` | Sonnet | rate setters + variant matrix (capture.c) and the standing stream `chain_stream_*()` (chaintest.c, plus `chain_streaming()`/`chain_stream_state()` not named in the card but sharing their state) moved verbatim; `clkdiv_cur` (capture_priv.h) and `chaintest.c`'s `setup()`/`restore()`/`triangle_for()`/`rate_hz()`/`ksps_of()`/`wait_ticks()`/`g_trig_hz`/`s_core`/`s_pinsel`/`s_samc`/`s_test_dac` (new `chaintest_priv.h`) reached as plain externs; traces `b2b`/`clk`/`variants` needed `acquisition.c` plus `chaintest.c`'s own dependency closure and `-DHAVE_CHAINTEST` (linking one object that mixes both groups); 13/13 traces unchanged, `stream_on`/`stream_on_input` goldens byte-identical, fncmp shows only the moved/renamed symbols differ (identical instruction counts), `_DMA0Interrupt` 42/0, `_U2RXInterrupt` 55/0, [SMOKE] PASS |
| P9.4b Dependency acquisition -> chaintest inverted | done | `93c485f` | Sonnet | added task: P9.4 left `chaintest.c`'s `setup()`/`restore()`/`triangle_for()`/`rate_hz()`/`ksps_of()`/`wait_ticks()` in `chaintest.c`, reached from `acquisition.c`'s `chain_stream_on_input()` through a `chaintest_priv.h` - the application layer depending on the test layer's internals, backwards, through generic global names in the whole firmware's namespace. Inverted: the six functions moved into `acquisition.c`, renamed `acq_chain_setup()`/`acq_chain_restore()`/`acq_triangle_for()`/`acq_rate_hz()`/`acq_ksps_of()`/`acq_wait_ticks()` (bodies unchanged apart from the rename), `chaintest.c`'s stages call them now (test -> app); `s_core`/`s_pinsel`/`s_samc`/`s_test_dac` became a plain static in `acquisition.c` (both ends of that state now live in one file); the raw state still shared in both directions (`acq_trig_hz`, `acq_setup_rc_pll`/`_trig`/`_dac`/`_ok`, `acq_step_on`) and the shared constants moved to a new `acquisition_priv.h`, which replaces and deletes P9.4's `chaintest_priv.h`; `grep -rn chaintest src/app/` finds no include, only comments. Trace scenarios `b2b`/`clk`/`variants` dropped `chaintest.c`/`tri_eval.c`/`meter.c`/`-DHAVE_CHAINTEST` from their `.sources`/`.cflags` (nothing in `acquisition.c` calls into `chaintest.c` any more) but keep `dac.c` and the board file (`acq_chain_setup()`/`_restore()` call into both directly); `stream_on`/`stream_on_input` untouched, still linking `chaintest.c` (unaffected, not required by the card). Verified: hw/sim/nano/smoke builds `-Wall -Wextra` clean; `tools\trace.bat` 13/13 PASS, `git diff 911cdb9 --stat -- tests/trace/golden tests/smoke` empty; `tools\hosttest.bat` 15/15 PASS; fncmp `_DMA0Interrupt` 42/0, `_U2RXInterrupt` 55/0 unchanged; comparing the pre-task hw ELF against this one, 25 of the 26 differing functions are pure symbol renames at identical instruction counts - the one exception, `chaintest.c:_summary` (180 -> 182 instructions), is `acq_ksps_of(ladder_n[g_best])` losing its intra-translation-unit visibility now that the callee moved to `acquisition.c`: the compiler can no longer see the callee's body to know it does not need to reload `g_best` across the call, so it reloads it from memory instead of keeping it live in a register - same computed values, no behaviour change; `python tools\sim_trap.py --smoke` -> [smoke] PASS |
| P9.5 [SIM] acceptance run | done | this commit | lead | at `93c485f`: default `[simtest] PASS` (100 halves, 0 bad), 256 per half PASS, `--fault 65536` FAIL with one mismatch (half 9, index 0). The fault case had proved nothing since `afc5e00`: it waited for "measurement running", which that commit removed, so the fault landed after the check and the run said PASS; `sim_trap.py` now waits for the current marker and refuses a fault run that PASSes |
| P10.1-P10.4 Split `clock.c` | moved to N+2 | | | user decision 27.09.2026; when it is done: needs `trace_point()` between clock steps (approach (a)) |
| P11.1 Routing types | done | `4bddb2c` (worktree, beside P9.3) | Sonnet | routing.h/.c, host-only, no apply yet; pin-reachability table from a new generator, tools/gen_route_pins.py, against tools/pins128.py |
| P11.2 Host tests of the conflict rules | done | `0a5ecca`, merge (see log) | Sonnet | tests/host/test_routing.c, 45/45 checks, one test per rule; a deliberately broken SCCP check caught it (44/45, reverted) |
| P11.3 `routing_apply()` for `ROUTE_STREAM` | done | this commit | Fable | `routing_apply()` = `route_check()` (routing_add()'s rules, shared) + shape gate (STREAM sink, no table, EXT or DAC_PIN/dac 2; else NOT_YET before any driver call) + `acq_chain_setup_input()` (new in acquisition.c: the three statements chain_stream_on_input() wraps around acq_chain_setup(), as a function; the fixed order is acq_chain_setup()'s body, mapped step by step in routing.c's comment) + restore/`ROUTE_ERR_SETUP` on refusal; `route_t` gained `samc` (the rate stays outside a route, A1); `ROUTE_STREAM`/`ROUTE_B2B` defined in acquisition.c from board.h's macros (both boards); pin table per DEVICE macro (`pins64.py` added to gen_route_pins.py; Nano: AD5AN3 = RA8 reachable); routing.c in every build. New golden `route_stream`: W/C lines equal `stream_on`'s except six IEC2/DMACON/DMA0CH down/up lines its extra snapshot point exposes - without that point byte-identical (checked, not committed); `slp=0` in both (clock_dac_hz() reads COSC the model never switches). `stream on` NOT routed through it yet (P11.4). Verified: hw/sim/nano/smoke clean; trace 14/14, 13 existing goldens unchanged; hosttest 15/15 (test_routing 93 checks); fncmp `_DMA0Interrupt` 42/0, `_U2RXInterrupt` 55/0, 427 functions identical, 5 differ only in BUILD_ID string hashes or a relabelled RAM address (`_cmd_parser_feed_char`: the same constant now falls inside `route_table`), 14 new (none dropped by the linker); [SMOKE] PASS |
| P11.4 `stream on` through the routing | done | this commit | Fable | `chain_stream_on()` = `routing_apply(&ROUTE_STREAM)` + the old tail, `chain_stream_on_input()` builds the route from its arguments (`ROUTE_SRC_DAC_PIN`/dac 2 with `test_signal`, `ROUTE_SRC_EXT` without - the old `s_test_dac` flag verbatim; cli.c passes `false` for every custom form), both through one static `stream_on_route()`; the inline setup copy is gone (`acq_chain_setup_input()` has one caller). `routing_clear()` in `chain_stream_off()` and on the post-apply failure path (on/off/on host test). **The central proof: the `stream_on`, `stream_on_input` and `route_stream` goldens are byte-identical** (trace 14/14, `git diff --stat` on `tests/trace/golden` empty). Stopped once before editing: the routing would have refused PINSEL 6 (the internal 15/16 VDD reference, the GUI's "6 = internal ref"); lead decision: 6 reachable everywhere like 7 (`ROUTE_PINSEL_VREF`), and per the ATDF core 5's AD5AN5 "Touch ADC Input"/AD5AN8 "VDDCORE" too (`route_int_mask[]`); 9..15 unnamed, refused; package pins a core does not bring out refused as P11.2 intended (see Decisions). routing.c added to the five `.sources` that link acquisition.c. Verified: hw/sim/nano/smoke clean; hosttest all PASS (test_routing 161 checks); fncmp `_DMA0Interrupt` 42/0, `_U2RXInterrupt` 55/0; [SMOKE] PASS; `adc_gui.py --selftest` PASS |
| P11.5 `route list` | done | this commit | Sonnet | new console command `route list` (one parser slot: 27 commands + help = 28 of 32, 4 free - dispatch inside `cmd_route_fn()` like `stream on\|off\|grab`, so a later sub-command costs no more slots), sourced from `routing.c`'s new `routing_visit(route_visit_t visit)` - the active route(s) (`src`/`core`/`pinsel`/`dac`/`samc`/`sink`, or "route: none - ..." with nothing recorded) then the resource table (DMA/SCCP/DAC outputs/UREF used vs. total, RAM used vs. budget), one field per callback exactly like `port/regs.h`'s register-dump visitor - routing.c stays print-free, `cli.c`'s `route_print()` turns every call into a "key: value" line through `put_kv()`/the new `put_kv_str()`. `tests/host/test_routing.c`: `test_routing_visit_empty()`/`test_routing_visit_active_route()` check the exact call sequence through a capturing `route_visit_t`, 251/251 checks (was 161). [SMOKE]: `route list` added to `smoke_script` (main.c) after `status`; `sim_trap.py --smoke --update-expected` diff is exactly the new `route: ...`/resource-table block plus the new `help` line for `route` - nothing else changed; `--smoke` PASS against the new `expected.log`. Verified: hw/sim/nano/smoke clean; `trace.bat` 14/14, `git diff 6fdfadb --stat -- tests/trace/golden` empty (cli.c is not linked into the trace harness); `hosttest.bat` 15/15; fncmp `_DMA0Interrupt` 42/0, `_U2RXInterrupt` 55/0; `adc_gui.py --selftest` PASS |
| P12.1 `CLAUDE.md` close-out | done | `4be594a` | Sonnet | consistency pass, not a rewrite: the module tables and "who may touch what" already carried port/, routing.c, acquisition/meter/pingpong and the bench_client section (1a1464e) card by card; grepped for the stale phrasings the card named ("until P11.4", "not yet", "host-only so far", "26 commands", "27 in use", "13/13", "13 scenarios", "P11.3 wires it") and found none - the parser slot count already read 28 of 32. Added what was missing rather than stale: `tools\trace.bat`/`tools\hosttest.bat` as an explicit "Build and verify" step (14 scenarios, `tools\fncmp.py`'s 42/0 and 55/0 ISR counts), and one sentence atop "Open questions" that N+1 has not run on silicon and the first run goes through phase BR. "What the board has settled"/"Open questions" left as the silicon facts they are |
| P12.2 README/FIRMWARE-STRUCTURE/REFACTORING-PROPOSAL | done | `70d377c` | Sonnet | `README.md`: the `src/` folder list, the parser slot count (28/32), the `status` row, the whole `Files` table rebuilt on the current tree (port layer, `pingpong`/`meter`/`acquisition` split, `lib/` incl. the unused N+4 building blocks, `routing.c`/`route list`, `tests/host`/`tests/trace`, `[SMOKE]`/`[SIM]`), one pointer line to `board_run/README.md` (BR.7 writes the rest). `docs/FIRMWARE-STRUCTURE.md`: kept as the pre-N+1 analysis, a status note added pointing at the V1..V10 list and at `CLAUDE.md`. `docs/REFACTORING-PROPOSAL.md`: each of V1..V10 marked done/done reduced/deferred to N+2 with its P-task and commit |
| P12.3 `docs/HARDWARE-LOG.md` entry | done | this commit | Sonnet | dated entry "N+1 restructured, not run on silicon": what changed by phase, what was verified without a board (trace 14/14, hosttest 17/17, [SMOKE], [SIM] P9.5 incl. the repaired `--fault` case, fncmp 42/55), the one console behaviour change (`stream on` custom-pinsel refusal, P11.4), the open points only silicon answers, phase BR (A = `b41af3b` vs. B, BR.9's pass criterion), and predictions for R2/R4 on B |
| P12.4 [SIM] close-out | done | this commit | lead | at `ba75ff8` (firmware final: BR.6 included): [SIM] default PASS (0 mismatches), 256 per half PASS, `--fault 65536` FAIL with one mismatch at index 0 (written at 27 s); `_test_mplabx.bat` builds the EV74H48A configuration through MPLAB X's generator (all 34 objects incl. routing/acquisition/meter/pingpong; the generator picked xc-dsc 3.21 and rewrote `languageToolchainVersion`, restored to 3.31; stale `Makefile-*.mk`, `.X/build`, `dist` deleted); `adc_gui.py --selftest` PASS |
| BR.1 `tools/board_run.py` | done | `175b20c` | Sonnet | phase BR added 27.09.2026 (not counted in the 52); blocks R0..R7 over `protocol.Target`, log `<ms> <TX|RX|EV> <block> <text>`, RUNNER_VERSION 1; `sim_trap.py` cannot serve as a transport (the simulated UART takes no bytes at run time) |
| BR.2 `tools/eval_board.py` + `expected.json` | done | `0fa9607` | Sonnet | one evaluator: `board_run.py`'s summary calls it; 7 expectations tagged run19/prediction, all citing HARDWARE-LOG 25.09.2026 |
| BR.3 Python environment | done | `1a6bbaa` | Sonnet | `requirements-board.txt`; `gui_setup.bat/.sh` install both and run all three self-tests; no offline path (decision 5) |
| BR.4 `board_run/` hex files + README, runner finds them | done | `df224aa` | Sonnet | A = b41af3b, 242938 B, SHA-256 7068aa09...; a second clean build differs only in 4 `__TIME__` records; `*.hex binary` in `.gitattributes` (autocrlf had rewritten the hex); jumpers and the R5 ground marked "(to confirm on the board)"; runner default R5 input corrected to core 3 / pinsel 5 (was the touch pad) |
| BR.5 Merge of the worktree | done | this commit | lead | merged after P11.5; the stand-in's `route list` reply brought to P11.5's real format; hosttest 17/17 |
| BR.6 Firmware additions for the board run | done | this commit | Sonnet | `status` gained 11 fields, no new parser slot: `stack_size`/`stack_used_max`/`stack_free_pct` (`diag_stack_paint()` paints the free stack - current W15 to SPLIM minus 64 bytes - once, as the first thing `main()` does; guarded by `#if defined(__XC_DSC__)`, host trace harness gets a no-op stub), `buf_addr`/`buf_align_mod4`/`buf_len`/`buf_guard_ok` (built from capture.c's existing `capture_buffer()`/`capture_half_len()`/`capture_guard_ok()`, no new accessor), `boot_stage`/`trap_seen`/`trap_vec`/`chain_mark` (the current values; RCON not repeated - the banner already decodes and clears it). `eval_board.py` gained `check_stack_criterion()` (BR.9's `>= 25%` rule and the guard-word check, B only, A always `NOT_AVAILABLE`); both self-tests extended (17/17). [SMOKE]: simulator numbers at the point `status` runs in the smoke script - `stack_size=52264 stack_used_max=29764 stack_free_pct=99` (well below SPLIM, boot_stage=8 since the smoke script runs before `boot_mark(9)`); `expected.log` diff was exactly the 11 new lines plus the three build-id lines already masked by the comparison. Verified: hw/sim/nano/smoke `-Wall -Wextra` clean; `trace.bat` 14/14, `git diff 9ac4a0a --stat -- tests/trace/golden` empty (cli.c/main.c are not linked into any scenario); `hosttest.bat` 17/17; fncmp `_DMA0Interrupt` 42/0, `_U2RXInterrupt` 55/0 unchanged; `adc_gui.py --selftest` PASS |
| BR.7 Documentation | done | this commit | Sonnet | `CLAUDE.md`: module rows for `tools/board_run.py`/`.bat`, `tools/eval_board.py`, `tests/board/expected.json`, `board_run/`, plus a "Build and verify" paragraph on the board run (the reply-format rule, "a board run goes through `board_run.py`", and that it talks to a local COM port directly - `grep -i "bench\|relay" tools/board_run.py` finds nothing, so it does not use the relay/bench_client above). `README.md`: the old one-line pointer became a "Running the board test" section pointing at `board_run/README.md`. `docs/CHAIN-TEST-PLAN.md` section 7: the manual procedure replaced by `git pull`/`gui_setup.bat`/`board_run.bat`, pointing at `board_run/README.md`. `docs/TROUBLESHOOTING.md`: new §2.6 - port busy (who holds it, from `board_run.py`'s own message), no reply (timeout, reset, the runner carries on), pip behind a proxy (`HTTPS_PROXY`, `gui_setup.bat`'s own wording). Plan: a Decisions row (28.09., BR decision 4 superseded for remote runs by bench_client) and the P12.1 row's commit hash filled in |
| BR.8 B image for the first board run | done | this commit | Sonnet | B = dead53c (P12 close-out HEAD), 262492 B, SHA-256 1e1a27d414a2b7af0d1ba4ecc488fe4b4d7fca10e2826b8127324f659a8eac9b; built from two independent clean worktree checkouts, differing only in 4 `__TIME__` records (git revision/dirty flag identical, banner has no "+local changes"); `SHA256SUMS.txt`/`README.md` updated (A kept), HARDWARE-LOG's 2026-09-27 entry gained both SHA-256 under phase BR |
| BR.9 Board run and loop back | open | | | needs the colleague and the EV74H48A |
| DBG.1 `mem` command | open | | | after BR.9 (a firmware change invalidates B); one parser slot, address check before any access |
| DBG.2 `tools/sym.py` | open | | | name -> address from the ELF/map and the pack; refuses a map that does not match the banner's revision |
| DBG.3 Documentation, board-run integration | open | | | the `help` change is a reply-format change (BR rule) |
| SG.0 Datasheet check | open | | | window check on a RAM source, `DACxDAT` upper-half write, DAC rate limit, SCCP2 event and clock |
| SG.1 `dma.c` channel 1 | open | | | shared `DMALOW`/`DMAHIGH`, no `DMACON.ON` toggle while channel 1 runs; ISR 42/0 |
| SG.2 `sccp.c` SCCP2 | open | | | playback clock, trace scenario |
| SG.3 `src/siggen/` | open | | | no registers; callers that touch a DAC stop it first; host test |
| SG.4 `siggen` command | open | | | one parser slot; `help` change = BR rule, [SMOKE] |
| SG.5 Routing claim | open | | | DMA 1, SCCP 2, DAC output, RAM; conflict with `ROUTE_STREAM` on DAC2 |
| SG.6 GUI card | open | | | `tools/wavegen_model.py`, loop overlay and match chip |
| SG.7 GUI tests | open | | | selftest + Playwright |
| SG.8 Board run | open | | | block R8; fallback TMR2, then timer ISR ("A2 not met") |
| SG.9 Documentation | open | | | CLAUDE.md, architecture, DESIGN 4.2 corrected |

### Decisions taken during the work

| Date | Decision | Where recorded |
|---|---|---|
| 26.09. | Register trace: snapshot diff in C (approach (a)), not the page-guard trace the spike recommended; `console_*` stubbed; ISR-driven waits left out; goldens compare writes, console, stubs, `fail()`, no reads | `tests/trace/README.md` |
| 27.09. | Hybrid: a page-guarded read hook serves only the polled registers; the model thread and all retries removed | `tests/trace/README.md` |
| 27.09. | [SMOKE] runs without asking, one run at a time; the ~7-minute [SIM] run stays on request | `CLAUDE.md` |
| 27.09. | Trace starts from the ATDF reset values (P0.9) | `tests/trace/README.md` |
| 27.09. | Remaining cards run mixed: Fable for the hard ones (P11.3/P11.4), Sonnet for the mechanical ones - the Fable weekly budget stood at 34 % | this table, column Model |
| 27.09. | N+1 shortened: P8 (drivers with instances) and P10 (split `clock.c`) move to N+2; independent cards run in parallel worktrees. N+1 = P0-P7, P9, P11, P12. Reason: about 20 h of agent time down to 6-8 h; P8.1 (the DMA ISR, the only timing risk) leaves the board run after N+1 | this section, "After N+1" |
| 27.09. | The simulator, including the ~7-minute [SIM] acceptance run, runs without asking; one run at a time | `CLAUDE.md` |
| 27.09. | Goertzel: damped textbook form `q0 = s + 2D·cos·q1 − D²·q2` (the literal reading of the design is undamped) | `docs/DESIGN-MULTICHANNEL.md` 4.3 |
| 27.09. | Detector counts once per pulse: re-arm only below threshold × hysteresis | `docs/DESIGN-MULTICHANNEL.md` 4.3 |
| 27.09. | P9.4b added: P9.4 made `src/app/acquisition.c` depend on `src/tests/chaintest.c` internals (generic global names `setup()`/`restore()`, three trace scenarios linking the whole chain test); inverted before P11.3 builds on the chain setup | Status row P9.4b |
| 27.09. | Phase BR (board run through a host-side runner) added from `board-run-task.md`; host-side cards in a worktree now, firmware (BR.6) after P11 and before P12 | section BR |
| 27.09. | BR decision 1 (user): A = `b41af3b`, the parent of P0.1, built from a clean checkout. Run 19's firmware (`fbfd883` + local changes, committed only with `c3bc644`) cannot be rebuilt; A vs run 19's values shows the post-run-19 changes, B vs A the restructuring | section BR |
| 27.09. | BR decision 2: the run takes place after P12. Reason: B must carry `route list` (P11.5) and the BR.6 status fields, and one board run should cover N+1 as a whole | section BR |
| 27.09. | BR decision 3 (user): EV74H48A only; the EV17P63A gets its own first run later | section BR |
| 27.09. | BR decision 4: programming by hand (MPLAB X / IPE), not `ipecmd`. Reason: one dependency fewer at the colleague's; the runner checks the SHA-256 of the file named at the prompt, so a wrong file is caught anyway | section BR |
| 27.09. | BR decision 5: no offline installation; `gui_setup.bat` installs from the internet as today, a proxy through `HTTPS_PROXY`. Reason: follows from decision 8 as changed - a committed wheelhouse would put tens of MB of binaries (numpy, matplotlib) into the repository. (First settled as "package with wheelhouse", withdrawn the same day with decision 8.) | BR.3 |
| 27.09. | BR decision 6: `tests/board/expected.json`, not YAML. Reason: stdlib only, no PyYAML in the colleague's environment | BR.2 |
| 27.09. | BR decision 7: budget <= 15 min per run, <= 30 min for A + B, one timeout per block; the runner logs each block's duration, and R4's grab count is the knob if the first run overruns | section BR |
| 27.09. | BR decision 8 (user), changed the same day: no package. The colleague does `git pull`, `tools\gui_setup.bat`, `tools\board_run.bat`, nothing else. The hex files are committed prebuilt in `board_run/` (A now in BR.4, B in BR.8), built from clean worktree checkouts, with `SHA256SUMS.txt` and a README; the runner finds and checks them and records HEAD and a dirty tree | BR.4, BR.8 |
| 27.09. | BR decision 9: no signal generator assumed. R5 runs on the default custom input without a signal and judges only the data path (CRC, counters, frame shape); SNR/THD are judged only if the colleague answers the prompt with a connected signal. Reason: availability unknown, and the block must not fail for lack of a generator | BR.1, BR.2 |
| 27.09. | BR: the P3 libraries' cycle count on silicon is not in the first board run. Reason: keep the new firmware in B minimal; P3.8 stays optional | BR.6 |
| 27.09. | BR pass criterion adopted from `board-run-task.md` section 10, the stack margin fixed at >= 25 % of the stack unused. Reason: the port layer and visitor callbacks added depth that no host tool measures; a quarter leaves room for interrupt nesting not reached in the run | BR.9 |
| 27.09. | BR: `sim_trap.py` cannot serve as a transport for the runner - the simulator's UART is write-only to a file, nothing feeds bytes in at run time (BR.1's look); the runner is tested against its stand-in only | BR.1 |
| 27.09. | `stream on` custom input through the routing: PINSEL 6/7 (the internal 15/16 VDD reference and UREF) always reachable, and the internal channels the ATDF names on core 5 (AD5AN5 "Touch ADC Input", AD5AN8 "VDDCORE") likewise; PINSEL 9..15 (unnamed in the ATDF) and package pins a core does not bring out are refused - intended by P11.2's rule "a pin the core cannot reach", the only console behaviour change of N+1 (same "set-up failed" line as any other refusal; the GUI already snapped its PINSEL field to the core's pins plus 6/7, its hint narrowed) | P11.4: `routing.c`'s `route_int_mask[]`, `acquisition.h` |
| 28.09. | BR decision 4 superseded for remote runs: with bench_client (`CLAUDE.md`, `1a1464e`) the lead can flash through `ipecmd` over the relay; runs by the colleague stay by hand | `CLAUDE.md` "Remote board access" |
| 29.09. | SG added (user request): N+3's signal generator brought forward before N+2, with narrow channel-1/SCCP2 functions that P8 generalises later; one DMA window over table and ADC buffer (the window is global); `TRMODE = 1`, not DESIGN 4.2's Repeated Continuous; parameters set one per line (64-character console line) | section SG |
| 28.09. | DBG added after BR.9: a `mem rd/wr` console command (one parser slot, `diag.c`) and `tools/sym.py`, so an agent can read and change memory over the relay without a rebuild; deliberately after the first board run, since any firmware change invalidates the B image | this plan, section DBG |

### Handover to the next lead session (27.09.2026, 18:30)

The lead session that ran P0 to P9.2 was cleared to save tokens. To continue: read
`CLAUDE.md`, this "Status" section, and the rules below; then hand out the next card.

**Next cards, in this order** (model as decided 27.09.: Sonnet for mechanical cards,
Fable only for P11.3/P11.4):

| Card | Model | Note |
|---|---|---|
| P9.3 `src/meter/meter.c` | Sonnet | traces `b2b`, `variants` unchanged |
| P9.4 `src/app/acquisition.c` (incl. `chain_stream_*` out of chaintest.c) | Sonnet | traces `stream_on`, `stream_on_input` unchanged |
| P9.5 [SIM] acceptance run | Sonnet (or the lead itself) | default, 256 per half, `--fault`; ~7 min each, one at a time |
| P11.1 + P11.2 routing types + host tests of all conflict rules | Sonnet | host-only |
| P11.3 `routing_apply()` for `ROUTE_STREAM` | Fable | |
| P11.4 `stream on` through the routing | Fable | the central proof: `stream_on`/`stream_on_input` traces identical |
| P11.5 `route list` | Sonnet | one parser slot (27 + help = 28 of 32); [SMOKE] with `route list` added to the smoke script and `expected.log` updated on purpose |
| P12.1-P12.4 close-out | Sonnet | P12.3: HARDWARE-LOG entry "N+1 restructured, not run on silicon" + the board-run checklist; P12.4 [SIM] |
| BR.1-BR.4 board-run tools (worktree, beside P9.5-P11) | Sonnet | see section BR; BR.6 after P11, before P12 |

**How the lead checks every card** (do not trust the agent's report; it once named
the wrong branch): `git status` clean; `git log -N --format=%B | grep -ic co-authored`
= 0; `tools\trace.bat` 13/13 PASS; `tools\hosttest.bat` all PASS; `git diff <before>
--stat -- tests/trace/golden tests/smoke` empty unless the card says why;
`python tools\fncmp.py --count _DMA0Interrupt build\adc_dma_40msps.elf` = 42 and
`--indirect` 0; `_U2RXInterrupt` 55/0; the card's row in this table updated.

**Brief building blocks that proved necessary** in every agent brief: read CLAUDE.md,
this Status section and the task; builds via
`MSYS_NO_PATHCONV=1 cmd /c "cd /d C:\work\Claas\ADC\tools & C:\work\Claas\ADC\tools\build.bat nano" < /dev/null`;
never write files with backslashes through a Bash heredoc or sed (use Write/Edit);
`console_*/fail()` inside a C comment closes it; file lists for a new .c = build.bat
(all variants), tools/Makefile SRC+HDR, configurations.xml (never touch
`languageToolchainVersion`, never `git checkout` it; delete stale
`nbproject/Makefile-*.mk` before `_test_mplabx.bat`, delete `.X/build` and `dist`
after), tools/setup.py SOURCES, trace `.sources`; stop after two failures of the same
command; keep context small (grep/sed -n, never cat cli.c/capture.c whole); explicit,
exhaustive stop conditions; no attribution trailer; update the Status row.

**Parallel work:** only cards whose files stay disjoint for their whole run go into a
worktree (`isolation: worktree`). P7 beside P6 cost an extra card because P6 moved the
code P7 changed; every card touching build file lists conflicts there (union merge by
hand is easy). The remaining cards are largely sequential (P9.3 -> P9.4 -> P11); P11.1 +
P11.2 (host-only, new files) could run in a worktree beside P9.3/P9.4.

**Usage at the time of handover** (`/usage`, 27.09. afternoon): weekly 26 %, Fable
34 %, reset Friday 02.10. Agent tokens so far about 400 M (mostly cache reads); a
card of 30-60 calls costs 2-6 M, a card of 300+ calls 150 M+ - keep cards small.

### Open points found along the way

- MPS506 ATDF gives `CLK1CON` reset value 0x28180, the MPS512's 0x101 (the simulator
  confirms 0x101); probably an ATDF error. A smoke run for the MPS506 or the Nano board
  would settle it. No effect on N+1: `clock_init()` writes the whole word.
- diag.h's `WAIT_WHILE` has no user left after P4.7; `WAIT_LIMIT` is still used by
  `capture.c` and `main.c`.
- The trace cannot see a write of a register's reset value (approach (a)), e.g. `PR1`.
- A clean simulator run does not prove the absence of misaligned accesses; only
  silicon traps them.
- P7.1 moved only the boot PLL dividers into `board_cfg`; ADC core/input, LED
  port/polarity and the console's PPS/TRIS pins stay `board.h` macros (compile-time
  uses, `led_toggle()`'s hot path, and a path no golden trace exercises without a
  board run) - `grep board.h src/drivers/` still finds hits; revisit in N+2 with P8.
- The DAC slope path of `stream on` is covered by no golden (found in P11.3, recorded
  P11.4): in the trace harness `acq_triangle_for()` refuses the triangle because
  `clock_dac_hz()` reads `CLK7CON.COSC`, which the hardware model never switches, so
  `slp=0` in both `stream_on` and `route_stream` and no `DAC2SLP*` write appears in
  either. `tests/trace/scenarios/stream_on.c`'s header paragraph on the PLL1DIV/VCO1DIV
  preset ("or it reads 0 and refuses the triangle") is therefore stale - the preset is
  necessary but not sufficient. Only the `dac` scenario traces the triangle registers.
  A model rule that lets CLK7CON's COSC follow NOSC on OSWEN would close it (a golden
  change, on purpose, in a card of its own).
- PINSEL 9..15 are refused by `route_pin_reachable()` since P11.4 because the pack's ATDF
  (dsPIC33AK-MP_DFP 1.4.260, both devices) names no channel for them - only the
  value-group's bare `AD_AN9..15`. If a datasheet revision names one, add it to
  `route_int_mask[]` in routing.c.

## Rules for every task

1. **One task = one commit.** The commit message states what changed, why, and how it
   was verified. It carries no attribution trailer.
2. **All three builds stay `-Wall -Wextra` clean:** `tools\build.bat`,
   `tools\build.bat sim`, `tools\build.bat nano`.
3. **Register trace unchanged** (from P0 on): `tools\trace.bat` reproduces every golden
   trace, unless the task says a trace changes and why.
4. **Host tests green** (from P0 on): `tools\hosttest.bat`.
5. **No behaviour change on the console.** Any task that touches output says how that
   was checked.
6. **The simulator is used at two levels.**
   - **[SMOKE]**: the smoke build (P0.7) boots, prints the banner, runs `help` and a few
     commands, and checks for traps. It is meant to take under a minute and runs after
     every task that touches the boot or console path. It may run without asking, as
     long as the P0.7 measurement confirms that it stays short.
   - **[SIM]**: the full acceptance run (~7 min, ping-pong stream) **only runs when the
     user asks**. It is collected at P9 and P12.
   The simulator compiles with the real xc-dsc, device header and linker script. It
   catches what the host cannot: dsPIC-only traps (misaligned access, stack overflow,
   address errors) and RAM layout. It models no PLL, ADC or DMA, and any pending
   interrupt aborts the run. It says nothing about driver register behaviour; the
   register trace does.
7. After any change to the file list: update `tools/build.bat`, `tools/Makefile`,
   `nbproject/configurations.xml` (file list only; never `languageToolchainVersion`, and
   never `git checkout` the whole file), and delete `adc_dma_40msps.X/build` and `dist`.
8. `cmd_parser.c/.h` stays untouched except for the known `CMD_PARSER_MAX_COMMANDS` line.

Legend: **Verify** = the evidence that the task preserves behaviour. **Done** = the
condition for committing.

---

## P0: Test infrastructure

Nothing in the firmware changes in this phase. It builds the instruments that every
later task relies on.

### P0.1 Baseline

- Build all three variants and record the memory usage (flash, RAM) of each in
  `tests/baseline.md`.
- Record `help` output and the boot banner from a simulator run that already exists in
  `docs/logs/`, if any; otherwise capture them with the first [SMOKE] run (P0.7).
- **Done:** baseline file committed.

### P0.2 Host test harness

- `tests/host/` with a minimal assert header (`check.h`: `CHECK`, `CHECK_EQ`, a summary
  line, exit code), no external framework.
- `tools/hosttest.bat`: builds each `tests/host/test_*.c` with the installed MinGW gcc
  (`-std=c11 -Wall -Wextra -Werror`), runs them, and prints PASS/FAIL per test.
- First test: `test_crc16.c` (check value 0x29B1 for "123456789", as in
  `crc16_selfcheck()`).
- **Verify:** the test passes; a deliberately broken CRC makes it fail.
- **Done:** `tools\hosttest.bat` prints `1/1 PASS`.

### P0.3 Spike: how to trace register writes (timebox: half a day)

The question is how to make a driver compiled on the host record what it writes to the
SFRs. There are two candidates:

| | (a) Snapshot diff in C | (b) Proxy objects in C++ |
|---|---|---|
| How | Fake `xc.h` declares every SFR as a plain variable. After each call and at every `trace_point()`, the harness diffs all SFRs against the last snapshot | Each SFR and bit field is a C++ object whose `operator=` logs the write |
| Records order? | only between trace points | every write, in order |
| Drivers compile unchanged? | yes, as C | only if the driver code is valid C++ |
| Busy-wait loops on status bits | preset the bits; a self-clearing bit (`DIVSWEN`, …) hangs | a read hook can answer "ready" |

- List every polling loop in the drivers (`grep -n "while"` in `clock.c`, `adc.c`,
  `dma.c`, `dac.c`, `sccp.c`, `cli.c`), with the bit it waits on.
- Generate the fake `xc.h` from the device header in the DFP
  (`…/xc16/support/dsPIC33A/h/p33AK512MPS512.h`) with a script
  (`tools/gen_fake_sfr.py`), not by hand.
- Try both approaches on `timebase.c` and `dma.c`.
- **Done:** a short decision written into `tests/trace/README.md`, covering the chosen
  approach, how polling loops are satisfied, and what the trace cannot see.

### P0.4 Register-trace harness

- `tests/trace/`: the generated fake `xc.h`, the recorder, a `main` per scenario, and
  `tools/trace.bat`.
- The harness also captures everything the drivers print (`console_*` is stubbed into
  the trace). Output changes therefore show up as trace changes.
- Timer and clock readings (`timebase_ticks()`) are stubbed to advance deterministically.
- **Done:** a trace of `timebase_init()` matches the four writes in `timebase.c`.

### P0.5 Golden traces of today's state

One scenario per entry point, recorded from the current code and committed as
`tests/trace/golden/*.trace`:

| Scenario | Entry points |
|---|---|
| `boot` | `console_early_init`, `clock_init`, `timebase_init`, `led_init`, `adc_init`, `capture_init`, `cli_init` |
| `stream_on` | `chain_stream_on(1000)` (1 MSPS), `chain_stream_off()` |
| `stream_on_input` | `chain_stream_on_input()` with a non-default core and pin |
| `b2b` | `capture_set_pll(5,5)`, `capture_start()`, `capture_stop()` |
| `variants` | `capture_select_variant()` for every variant in the table |
| `dac` | the DAC2 triangle as `chain all` sets it, and `dac2` off |
| `sccp` | `sccp1_start()` for each clock/mode/event combination the code uses |
| `clk` | `capture_set_clkdiv()`, `clock_adc_set_rate()` for three rates |
| `fail` | the clock-fail path (`_CLKFInterrupt` called directly) |
| `regs` | `regs_dump()`, the output text included |
| `nano` | `boot` with `-DBOARD=2` |

- **Verify:** recording twice gives identical files, so the traces are deterministic.
- **Done:** all golden traces committed and `tools\trace.bat` passes.

### P0.6 Disassembly comparison

- `tools/fncmp.py`: disassembles two ELF files per function with
  `xc-dsc-objdump -d`, normalises absolute addresses, and reports the functions that
  differ.
- This is used for pure moves (P1) and to prove that an ISR still has no indirect call
  (P8).
- **Done:** comparing an ELF with itself reports no difference; changing one constant
  reports exactly one function.

---

### P0.7 Simulator smoke build **[SMOKE]**

- `tools\build.bat smoke`: the simulator build with `-DSIM_SMOKE=1`. In `main.c`, the
  simulator path skips the ping-pong stream and instead feeds a fixed command script
  to the parser (`help`, `version`, `status`, and later `route list`), then stops with a
  final marker line `[smoke] DONE`.
- `tools/sim_trap.py --smoke`: runs it, fails on any trap, a missing `[smoke] DONE` or
  a timeout, and saves the console output as `build/smoke.log`.
- `tests/smoke/expected.log`: today's output, committed. Later tasks diff against it;
  lines that change on purpose (a new command in `help`) are updated in the same commit.
- **Measure** the wall-clock time. If it is over a minute, find out why (the `__delay32`
  scaling, UART speed) before relying on it. If it cannot be made short, [SMOKE] also
  runs only on request.
- **Done:** the smoke run passes on today's code, its duration is recorded in
  `tests/baseline.md`, and a deliberate trap (a misaligned 32-bit read behind a
  `SIM_SMOKE_FAULT` switch) is reported as a failure.

### P0.8 Cross-check: host fake header against the simulator

The register trace is only as good as the generated fake `xc.h`. This task checks it
once against the real toolchain:

- After boot in the simulator, read the SFRs that the `boot` trace writes, through MDB
  (`sim_trap.py --dump-sfr <list>`).
- Compare them with the end state of the host `boot` trace.
- First find out whether the simulator stores SFR writes at all for peripherals it does
  not model. The P0.3 spike checks this on one register. If it does not, this task
  reduces to comparing addresses and bit positions of the fake header against the
  ATDF, done by `gen_fake_sfr.py` itself.
- **Done:** every register in the `boot` trace agrees in address and final value, or
  the reduced check passes and the limitation is written into `tests/trace/README.md`.

## P1: Directory structure (moves only)

### P1.1 Move files into `src/` subfolders

Files move whole; splits come later. Includes stay `"name.h"` by adding `-I` for each
folder.

| Folder | Files (today) |
|---|---|
| `src/drivers/` | `adc`, `dma`, `sccp`, `dac`, `clock`, `timebase`, `led` |
| `src/app/` | `main.c`, `capture.c/.h`, `config_bits.c`, `board.h` |
| `src/cli/` | `cli.c`, `console.h`, `cmd_parser.c/.h` |
| `src/tests/` | `chaintest.c/.h`, `dactest.c/.h` |
| `src/lib/` | `crc16.c/.h` |
| `src/diag/` | `diag.c/.h` |
| `src/sim/` | `sim.h`, `sim_dma.c` |

- Use `git mv` so that history follows the files.
- Update `build.bat`, `tools/Makefile`, `configurations.xml` (logical folders mirror
  `src/`), `tools/version.bat` if it writes into the root, and the P0 harness paths.
- **Verify:** `fncmp.py` reports no function difference for all three builds; the
  traces are unchanged; `tools\_test_mplabx.bat` builds; **[SMOKE]** passes.
- **Done:** all of the above, plus the paths in `CLAUDE.md` and `README.md` updated.

---

## P2: Hardware-free libraries out of mixed modules (V1)

Each task moves code into `src/lib/` and adds a host test.

| Task | From → To | Host test |
|---|---|---|
| **P2.1** | `u32_to_str`, `u32_to_hex`, `copy_str` (cli.c) → `lib/fmt.c` | limits (0, 0xFFFFFFFF), widths, buffer end |
| **P2.2** | `half_stats`, `half_mean` (cli.c, capture.c) → `lib/stats.c`, with a `const uint16_t *` interface without `volatile` | min/max/mean against hand-computed arrays |
| **P2.3** | `fit_line`, `tri_eval`, `tri_t` (chaintest.c) → `lib/tri_eval.c` | synthetic triangle windows (clean, with DNL, noise, one lost sample, one repeated sample); expected counts as in `CLAUDE.md` (no false alarm in clean windows, 96-100 % detection) |
| **P2.4** | P2.3 cross-check | `eval_chain.py` and the host-compiled `tri_eval` give identical results on the same windows (Python calls the test binary, compares) |

- **Verify, each:** host test; traces unchanged. Output formatting goes into the
  `regs` trace, so P2.1 is covered there.
- **Done, each:** as per the rules.

---

## P3: Goertzel and wavegen, included but unused

All in `src/lib/`, compiled in all three builds, called from nowhere.

### P3.1 Python reference models

- `tests/ref/goertzel_ref.py`: Goertzel in float64 with **one** damping factor
  (`damping`, default 0.995) in the feedback term, followed by the magnitude
  approximation `|re| + |im| − min/2`, the IIR1 low-pass (k = 4), and the detector
  (threshold, window, reset of state, counter). Written from the template
  `Goertzel/goertzel/firmware/src/goertzel.c`, with the second damping stage
  (`q − (q >> 8)`) deliberately left out.
- `tests/ref/wavegen_ref.py`: the formula from `tab_wave_gen.py` without the GUI:
  harmonics 2-7, envelope, min/max scaling to `out_min..out_max`.
- Both write test vectors (`tests/ref/vectors/*.csv`) that the host tests read.
- **Done:** vectors generated and committed with the generating command in a header
  line.

### P3.2 `lib/iir1.c`

- `iir1_t` per instance (low-pass and high-pass, shift `k` as parameter). Replaces the
  global `iIIR_Tap[]`.
- **Host test:** step response against the reference; two instances do not affect each
  other.

### P3.3 `lib/goertzel_f.c` (float, the default)

- `goertzel_f_t`, `goertzel_f_init(g, fs_hz, f_hz, damping, window)`,
  `goertzel_f_block(g, x, n, mag_out /* may be NULL */, detect)`.
- Block length `n` and `in_shift` are parameters. No `static` state.
- **Host test:** against `goertzel_ref.py` within a relative tolerance (to be fixed in
  the test; float32 against float64).

### P3.4 `lib/goertzel_i.c` (fixed point, the alternative)

- Same interface. The damping factor is Q16 in the feedback only.
- **Host test:** against the reference with a tolerance derived from the Q16
  quantisation, written into the test with the reasoning.

### P3.5 `lib/detect.c`

- Threshold, window counter, pulse counter, `max_amplitude`, adaptive threshold
  (`max_amplitude × scale`, from the template's `main.c`), `detect_reset()`.
- **Host test:** pulse trains from `wavegen_ref.py` (N pulses, decaying) → exactly N
  detections for both Goertzel variants. Two channels with different signals → each
  counts its own pulses. This is the case the template gets wrong through its global
  state.

### P3.6 `lib/wavegen.c`

- `wavegen_cfg_t` (n, play_hz, f0_hz, harm[6], decay, amplitude, out_min, out_max) and
  `wavegen_fill()`. Option `snap` rounds f0 to a whole number of periods in the table
  and returns the frequency actually used.
- **Host test:** against `wavegen_ref.py` with `out_min = 0`, `out_max = 1023`, ±1 LSB;
  with `out_min = 205`, `out_max = 3890` (the DAC range from the ATDF), every value
  inside the range.

### P3.7 Into the builds

- Add the five files to all three builds and to the MPLAB X project.
- Record the flash growth against the P0.1 baseline. If the linker keeps the unused
  code, that is accepted for N+1 (flash is 252 KB). Turning on
  `-ffunction-sections -Wl,--gc-sections` would be its own task, with an `fncmp`
  check, and is not part of N+1.
- **Verify:** traces unchanged; `fncmp` reports no change in any existing function.

---

### P3.8 Cycle count per block (simulator, optional)

- A smoke-build variant that calls `process_buffer`, `goertzel_f_block` and
  `goertzel_i_block` on one 512-sample block, with the simulator stopwatch reading
  before and after each (MDB `stopwatch`, driven from `sim_trap.py`).
- First check whether the simulator counts FPU instructions with realistic cycle
  counts: time a loop of known float operations and compare with the instruction set
  reference. If it does not, drop this task. The float/fixed decision is then measured
  on the board in N+4.
- **Done:** cycles per block for the three functions in `tests/baseline.md`, marked
  "simulator, not silicon".

## P4: Port layer (V2)

### P4.1 `src/port/log.h`, `src/port/panic.h`, `src/app/port_impl.c`

- `port_log(s)`, `port_log_kv(key, v, hex)`, `port_panic(code)` (noreturn). The
  implementation maps to `console_puts/kv/kv_hex` and `fail()`.
- **Done:** builds; nothing uses it yet.

### P4.2 to P4.7: one driver per task

| Task | Driver | Replaces |
|---|---|---|
| P4.2 | `timebase.c`, `led.c` | `board.h` stays for now (P7) |
| P4.3 | `sccp.c` | `console_*` |
| P4.4 | `dac.c` | `console_*` |
| P4.5 | `adc.c` | `console_*`, `SAMPLES_PER_BUF_MAX` becomes a parameter of `adc_init()` |
| P4.6 | `dma.c` | `console_*`, `fail(8)` → `port_panic(8)` |
| P4.7 | `clock.c` | `console_*`, `fail(10)` → `port_panic(10)`; `capture_halt()` + `console_force_up()` in `_CLKFInterrupt` → `clock_fail_hook()` (weak, implemented in `app/`) |

- **Verify, each:** traces unchanged, output text included. `grep` shows no
  `console.h`, `diag.h` or `capture.h` include in the driver any more.

### P4.8 Register visitor for the dumps

- `typedef void (*reg_visit_t)(const char *name, uint32_t v);` and
  `xxx_regs_visit(visit)` per driver. `regs_dump()` in `diag.c` passes a visitor that
  prints exactly the old format.
- One commit per driver if the diff gets large.
- **Verify:** the `regs` trace is unchanged, character for character.

---

## P5: UART driver out of `cli.c` (V4)

### P5.1 `src/drivers/uart.c`

- `uart_init(const uart_cfg_t *)` (instance, baud, PPS pins from the board),
  `uart_write()`, `uart_flush()`, `uart_set_baud()`, `uart_reinit()`, and the receive
  callback (weak `uart_rx_hook(byte)`). `_U2RXInterrupt` moves with it.
- **Verify:** the `boot` and `fail` traces are unchanged.

### P5.2 `cli.c` on top of `uart.c`

- `console_*` keep their names and meaning, implemented over `uart_*`.
- **Verify:** `grep -E "U2|RPCON|RPOR|RPINR|IPC" src/cli/` finds nothing; traces
  unchanged. **[SMOKE]** the console works: the smoke log is identical to
  `tests/smoke/expected.log`.

---

## P6: Splitting `cli.c` (V10, V9)

| Task | Content | Verify |
|---|---|---|
| **P6.1** | per-module command registration: `bench_register()`, `chain_register()`, `link_register()` etc.; `cli_init()` calls them in the old order | `help` lists the same commands in the same order (**[SMOKE]**: the `help` block of the smoke log is unchanged) |
| **P6.2** | `src/tests/bench.c`: `test_*`, `matrix_*`, `sweep_*` out of `cli.c` | traces `variants`, `b2b` unchanged; builds |
| **P6.3** | `src/lib/frame.c`: header line, payload in chunks, CRC line; writes through `size_t (*write)(const uint8_t *, size_t)` | host test: frame built by `frame.c` is parsed by `parse_grab_frame()` from `adc_gui.py` without error, with the same CRC |
| **P6.4** | `src/link/gui_link.c`: `blk` and `stream grab` over `frame.c` | host test from P6.3 with the real header fields; GUI self-test (`adc_gui.py`, `FakeTarget`) |
| **P6.5** | `tools/protocol.py` out of `adc_gui.py` (`crc16_ccitt_false`, `parse_grab_frame`, `Target`), imported by `adc_gui.py` and `eval_chain.py` | GUI self-test; `gui_ui_test.py` |

After P6, `cli.c` contains only the basic commands and the formatting.

---

## P7: Board configuration as data (V8)

### P7.1 `board_cfg_t`

- `src/boards/ev74h48a.c`, `src/boards/ev17p63a.c` with one `const board_cfg_t` each
  (UART pins and PPS, LED port and polarity, ADC core and input, DAC route, boot PLL
  dividers). `board.h` selects one of them and keeps only the name macros.
- Drivers get their part of the config in `*_init()`. `grep board.h src/drivers/` finds
  nothing.
- **Verify:** `boot` and `nano` traces unchanged; **[SMOKE]** passes.

---

## P8: Drivers with instances (V5 + V3)

Pattern for every driver:

1. Generate the instance table (register addresses, IRQ numbers, trigger codes) from
   the ATDF with `tools/gen_instances.py`, not by hand. The script checks every address
   against the ATDF.
2. New API with a `xxx_t *` parameter. The old functions stay as thin wrappers for
   instance 0 (or 2, 5), so that callers migrate in a separate commit.
3. Each ISR vector calls a `static inline` common body with a **constant** instance
   pointer. The event goes to a weak hook with the instance index. There is no function
   pointer in the ISR.
4. Callers migrate; the wrappers are removed.

| Task | Driver | Instances | Note |
|---|---|---|---|
| **P8.1** | `dma` | DMA0..7 | `dma_event_hook(ch, status)` replaces `dma0_event()`. `fncmp`: `_DMA0Interrupt` has no indirect call, and its instruction count is recorded against the baseline |
| **P8.2** | callers of `dma` | – | `capture.c`, `sim_dma.c`, `chaintest.c` (register dumps) |
| **P8.3** | `adc` | cores 1..5 | ISR for each core (fixes the fixed `_AD5CH0Interrupt`/`AD5CH0RES`); `adc_result_hook(core, result)` replaces `adc_ch0_event()` |
| **P8.4** | callers of `adc` | – | |
| **P8.5** | `sccp` | SCCP1..8 | trigger codes from the ATDF (the lesson from the 32/34 confusion) |
| **P8.6** | callers of `sccp` | – | |
| **P8.7** | `dac` | DAC1..8 | modes DC, triangle, slope; `dac_output()` (DACOEN); data range 205..3890 as a check; UREF `INSEL` = 5 + n |
| **P8.8** | callers of `dac` | – | |

- **Verify, each:** the golden traces for the instance in use are unchanged. For the
  other instances, a new trace per instance is reviewed once for address and
  bit-pattern plausibility and committed as golden. It proves the pattern, not the
  silicon.
- `chaintest.c` reads `IPCx` directly today. With P8.2 and P8.4 it reads through the
  drivers (`xxx_irq_priority()`).

---

## P9: Splitting `capture.c` (V7)

| Task | Content | Verify |
|---|---|---|
| **P9.1** | `src/app/pingpong.c`: `pingpong_t` (buffer, half length, guard words, counters `missed`, `late`, `overrun`), `pingpong_on_half()`, `pingpong_service()`. No driver include. The buffer is passed in | host test: sequence of half events with gaps → the right `missed`/`late` |
| **P9.2** | simulator hooks out of `pingpong.c`: `sim_dma.c` checks the half itself | build sim; **[SMOKE]**; the full check follows in P9.5 |
| **P9.3** | `src/meter/meter.c`: `measure_rate`, `process_bench`, `oneshot_n`, `selftest`, `clkoff_probe` | traces `b2b`, `variants` unchanged |
| **P9.4** | `src/app/acquisition.c`: variants, rate (`set_pll`, `set_rate`, `set_clkdiv`), and `chain_stream_*` moved out of `chaintest.c` | traces `stream_on`, `stream_on_input` unchanged |
| **P9.5** | **[SIM] acceptance run** (without asking since 27.09.2026): `sim_trap.py` default and at 256 samples per half, plus the `--fault` case | `[simtest] PASS`, `PASS`, `FAIL` with one mismatch at index 0 |

---

## P10: Splitting `clock.c` (V6)

The order of the clock writes matters here. This phase needs the ordered trace from
P0.3; if the spike chose the snapshot approach, add trace points between the steps
first.

| Task | Content |
|---|---|
| **P10.1** | `src/drivers/pll.c`: feedback divider, prescaler, post dividers, lock wait |
| **P10.2** | `src/drivers/clkgen.c`: generator n (source, divider, `DIVSWEN`/`CLKRDY` sequence per Example 12-2) |
| **P10.3** | `src/drivers/clkmon.c`: clock monitor as a frequency meter |
| **P10.4** | `src/app/clock_plan.c`: which PLL feeds what (ADC, trigger CLKGEN13, DAC CLKGEN7), `clock_adc_set_rate()` with its `PLLFBDIV` search, the reasons from Table 40-24 |

- **Verify, each:** the `boot`, `clk` and `fail` traces are unchanged, **in order**.
  The datasheet citations move with the register writes.

---

## P11: Routing core

### P11.1 Types and resource model

- `src/app/routing.h`: `route_src_t` (EXT, DAC_INT, DAC_PIN, RAM_TABLE),
  `route_sink_t`, `route_t`, `route_err_t` with one code per reason, and the resource
  table (DMA 0..7, SCCP 1..8, DAC 1..8, DACOUT1/2, UREF, ADC cores 1..5, RAM budget).
- **Done:** header plus `routing.c` with `routing_add()`, `routing_clear()` and the
  checks, and no `apply` yet.

### P11.2 Host tests of all conflict rules

One test per rule, each with one case that triggers the rule and one that just passes:

- DMA channels: ADC channels + table DACs ≤ 8.
- SCCP: trigger + playback clocks ≤ 8.
- DAC outputs ≤ 2; UREF ≤ 1 DAC.
- One core in one route only.
- A pin that the core cannot reach. The table is generated from the ATDF/board.
- RAM: halves × channels + tables within the budget (runtime check; decision of
  26.09.2026).
- Everything except `ROUTE_STREAM` → `ROUTE_ERR_NOT_YET` in N+1.

### P11.3 `routing_apply()` for `ROUTE_STREAM`

- `ROUTE_STREAM` = SCCP1 → ADC core 5 (single) → DMA0 → ping-pong → CPU, DAC2 as the
  signal. `routing_apply()` runs the fixed order: DMA off, cores off, clock and trigger,
  cores on, DMA from scratch. It calls only `acquisition`/driver functions.
- `ROUTE_B2B` (back-to-back, single channel; decision of 26.09.2026) is defined as
  data. In N+1 the `test` suite keeps using its current path; switching it to
  `routing_apply(&ROUTE_B2B)` is a task for N+2.

### P11.4 `stream on` through the routing

- `chain_stream_on()` / `stream on` call `routing_apply(&ROUTE_STREAM)`.
- **Verify:** the `stream_on` and `stream_on_input` traces are **identical** to the P0.5
  golden traces. This is the central proof that N+1 does what today's firmware does.

### P11.5 `route list`

- A new console command. It prints the active route and the resource table (which DMA
  channel, SCCP and DAC is in use). It takes one parser slot (then 27 + help = 28 of
  32).
- **Verify:** host test of the output function via the visitor; **[SMOKE]** the command
  in the simulator.

---

## P12: Close-out

| Task | Content |
|---|---|
| **P12.1** | `CLAUDE.md`: module table and rules for the new structure (who may touch which register, `port/`, hooks, `routing.c`), build and test commands (`hosttest.bat`, `trace.bat`) |
| **P12.2** | `README.md`, `docs/FIRMWARE-STRUCTURE.md` brought to the new state; `docs/REFACTORING-PROPOSAL.md` marks V1..V10 as done or deferred |
| **P12.3** | `docs/HARDWARE-LOG.md`: entry "N+1 restructured, not run on silicon", with the board-run checklist below |
| **P12.4** | final **[SIM]** acceptance run (ask first), `_test_mplabx.bat`, GUI self-test |

### Checklist for the first board run after N+1

**Superseded by phase BR** (27.09.2026): the first board run after N+1 goes through
`tools/board_run.py` (A = `b41af3b` against B = N+1, blocks R0..R7), and every item
below is one of its blocks - 1 = R2, 2 = R3, 3 = R1, 4 = R3/R4, 5 = R6. The list stays as
the short form of what the run must show:

1. `chain all`: every stage as in the last run before N+1 (compare with the log in
   `docs/logs/`).
2. `test all`: the back-to-back rows as before.
3. `regs`: identical to a dump from the old firmware on the same board (bitwise
   diffable).
4. `capture_process_bench` and the stream counters: same order as before (`_DMA0Interrupt`
   is unchanged in N+1, 42 instructions; P8.1, which changes it, moved to N+2).
5. `route list` shows `ROUTE_STREAM` with DMA0, SCCP1, DAC2, core 5.

---

## BR: Board run (host-side runner, one log file back)

Source: `board-run-task.md` (27.09.2026, written outside the repository while N+1 was
under way; its content is carried over here and the file itself is not needed any more).
Decisions 1, 3 and 8 were set by the user, the rest by the lead session - all in
"Decisions taken during the work" above.

**What BR is:** the colleague runs one script next to the board. It drives the existing
console, runs the same sequence against the pre-N+1 firmware (A = `b41af3b`) and the N+1
firmware (B), and writes one archive. `eval_board.py` turns that archive into a list of
deviations; each deviation becomes a correction card or an explained deviation. N+1
counts as "run on silicon" only when the pass criterion (BR.9) holds.

**Why host-side, not a firmware variant:** A and B run in the same session on the same
board (A cannot run a sequence built into B); run A is the baseline that `docs/logs/`
lacks (only run15/run16 are there, runs 18/19 were never committed); `stream grab` is
tested through `tools/protocol.py`'s `Target.grab()`, the GUI's own code, which has never
run on a board; a missing test is a new script, not a new hex file; hangs are handled by
the host (timeout, ask for reset, read the boot banner, carry on).

**Scheduling:** BR.1-BR.5 touch only `tools/` and `tests/` and run in a separate worktree
alongside P9-P11 (like P6.5 beside P5). BR.6 (firmware) runs after P11 and before P12.
BR.7 (docs) with or after BR.6. BR.8 (the B image) after P12. BR.9 is the run itself.

**Rule for every later firmware change (from BR.1 on):** a change to the reply format of
`version`, `help`, `status`, `regs`, `chain all`'s `@` lines or the GRAB header updates
`board_run.py`/`eval_board.py` in the same commit, checked by their self-tests.

### Sequence per run (A and B identical)

| Block | Command(s) | Answers |
|---|---|---|
| R0 | `sync`, `version`, `help`, `status` | build, revision, board; which commands this firmware has |
| R1 | `regs` | register state after boot; A/B bitwise diff |
| R2 | `chain all` | against run 19 and between A and B |
| R3 | `test all` | the back-to-back suite |
| R4 | `stream on` at 1, 4, 8 MSPS, each >= 50 `stream grab`, `stream off` | the GUI path: per grab the ov/late/missed delta, CRC, triangle verdict (`eval_chain.tri_eval`/`grid_ok`); raw frame stored on FAIL. Since 28.09.2026 also R4.gui: `buf 512` while stopped, `stream on 4000`, `dac 2 on 256 3000 39`, 10 grabs (CRC and counters, no triangle verdict), `dac 2 off`, `stream off`, `buf` restored - the two commands `adc_gui.py` sends on a board that no other block did (`docs/TEST-COVERAGE.md`) |
| R5 | `stream on <ksps> <core> <pinsel>`, 10 grabs | the non-DAC path (`slp=0`) |
| R6 | `route list` (B only) | P11 on silicon; `NOT_AVAILABLE` in A |
| R7 | `status` again | end state, trap/fail codes, stack high-water mark (BR.6) |

Every block has its own timeout. On timeout: log it, ask for a reset, read the banner,
continue with the next block. Budget: <= 15 min per run, <= 30 min for A + B.

### BR.1 `tools/board_run.py` + `tools/board_run.bat` (worktree)

- Interactive: "program the OLD firmware `<file>`, press ENTER" -> run A; the same for
  NEW -> run B; then a PASS/FAIL overview and the path of the one file to send. It never
  programs the board.
- Transport: `tools/protocol.py`'s `Target` (`sync()`, `cmd()`, `grab()`), 115200 8N1.
  `--list` shows the serial ports with VID:PID and description and marks the likely one
  (PKOB4 virtual COM port on the EV74H48A); without a port argument and exactly one
  candidate it uses that one and says so; "access denied" names the usual holders (GUI,
  terminal program, MPLAB X terminal).
- Compatibility: reads `version` then `help` first and derives the command set; a
  command the firmware does not have is `NOT_AVAILABLE`, never a failure; a NAK or
  unknown reply to a command it claims to have is a finding with the raw reply.
  `RUNNER_VERSION` goes into `session.json` and the first line of each log.
- Output: `run-<date>-<time>-<board>.zip` with `A.log`, `B.log` (every byte received and
  every command sent, host timestamp in ms and direction per line), `A-grab-<n>.bin` /
  `B-grab-<n>.bin` only for failed grabs, `summary.txt`, `session.json` (runner version,
  COM port, PC time, Python/pyserial versions, the two hex file names and SHA-256, the
  R5 signal answer). The hex files are checked against a SHA-256 list (BR.4 moves this
  to `board_run/SHA256SUMS.txt`).
- `--selftest` / `--fake`: the full sequence against a stand-in target (extend
  `adc_gui.py`'s `FakeTarget` or a replay stub for `chain all`/`test all`/`regs`) with a
  timeout in the middle, a reset with a new boot banner, a NAK, and a firmware without
  `route list`; checks the zip's content.
- Timebox half a day: can `sim_trap.py`'s console path serve as a transport, to run R0/R1
  against the simulator build? Result recorded in the commit message either way.
- **Verify:** `board_run.py --selftest` PASS; run from `tools\hosttest.bat` after the
  existing Python tests; `adc_gui.py --selftest` still 15/15.

### BR.2 `tools/eval_board.py` + `tests/board/expected.json` (worktree)

- Input: the session zip (or `A.log`/`B.log`). Imports `eval_chain.py` for `chain all`.
- Completeness: every block in A and B, `@END` reached, no timeout.
- A/B diff: per `@S..` line, per `regs` register, per counter: equal / different / only
  in one.
- Expectations: `tests/board/expected.json`, entries tagged `source: run19` (HARDWARE-LOG
  25.09.2026: 8 MSPS 15 s with overrun/late/missed 0, triangle clean up to 10 MSPS,
  slope 1.000, overruns from 10 MSPS, lost triggers from 16 MSPS) or `source:
  prediction` (S4/S6 pass at 1/4/8 MSPS, S9 picks 8 or 10 MSPS, processing load at
  8 MSPS well below half), so that a missed prediction is not read as a regression.
  Every entry cites its HARDWARE-LOG date.
- Output: one line per deviation - block, A value, B value, expectation and source, the
  source files the block exercises. Refuses a log from an unknown `RUNNER_VERSION`.
- **Verify:** `eval_board.py --selftest` on synthetic logs: identical A/B, one FAIL only
  in B, one timeout, truncated without `@END`, A without the BR.6 fields - each with the
  expected verdict; run from `hosttest.bat`.

### BR.3 Python environment (worktree)

- `tools/requirements-board.txt`: `pyserial`, `numpy`, `matplotlib` (only for
  `eval_chain.py --png`). No PyYAML (decision 6).
- `tools/gui_setup.bat` and `tools/gui_setup.sh`: install both requirement files into
  `tools/.venv` from the internet as today (a proxy through `HTTPS_PROXY`, as the script
  already says); after the GUI self-test run `board_run.py --selftest` and
  `eval_board.py --selftest`; header comment and closing message name all three tools.
  No offline installation (decision 8, changed).
- **Verify:** a fresh `.venv` in a temporary copy, both self-tests PASS.

### BR.4 `board_run/` - the committed firmware images and the colleague's README (worktree)

The colleague's whole procedure is `git pull`, `tools\gui_setup.bat` (once, and after a
pull that changed a requirements file), `tools\board_run.bat`. No package, no build on
his side (decision 8, changed 27.09.2026).

- `board_run/` at the repository root, committed (not git-ignored, unlike `build/`):
  `A-EV74H48A-b41af3b.hex`, `SHA256SUMS.txt`, `README.md`. Each hex file is built by
  `tools\build.bat` from a clean `git worktree` checkout of its revision (never a working
  tree with local changes) and committed together with the revision it came from.
  `README.md` states which revision each file is and that the files are not build output
  of the current tree. B is added in BR.8, not here.
- `board_run/README.md`: the three steps, what to send back, and the hardware set-up of
  the EV74H48A (jumpers, the one USB cable on PKOB4, power), checked against the board
  user guide and how runs 11-19 were set up (HARDWARE-LOG); that R2-R4 need no external
  wiring (DAC2 -> RA8 -> core 5 / UREF on chip) and that nothing may load RA8; for R5 the
  pin (core, `pinsel`, header pin) and that without a signal R5 tests only the data path.
- `board_run.py`: finds the hex files in `board_run/` itself and names them in its
  prompts with the full path; checks them against `SHA256SUMS.txt` before the run (a
  stale or locally modified file stops the run with a clear message); prints the
  hardware set-up of the README as a checklist before run A ("confirm with ENTER");
  records `git rev-parse HEAD` and `git status --porcelain` in `session.json` and warns,
  without stopping, if the tree is dirty. Self-test extended for all four.
- **Verify:** `A-EV74H48A-b41af3b.hex` bit-identical to a second, independent clean
  build of `b41af3b` (apart from the build-ID string, if any - say which bytes);
  `board_run.py --selftest` covers a matching, a modified and a missing hex file and a
  dirty tree; `hosttest.bat` all PASS.

### BR.5 Merge of the worktree

- BR.1-BR.4 merged onto master after P11 (lead). `hosttest.bat` all PASS, including the
  two new self-tests.

### BR.6 Firmware additions for the board run (after P11, before P12)

| Addition | Where | Why |
|---|---|---|
| Stack high-water mark: paint the stack at boot, report the deepest use and the margin to SPLIM | extra field in `status` | stack depth grew with N+1 (port layer, visitor callbacks) |
| Buffer address, alignment, guard words | extra fields in `status` | placement after relinking |
| `trap_seen`, `fail_code`, `chain_mark`, RCON after a reset | check what the banner/`status` already report; add what is missing | hang analysis |

- No new parser slot (27 + `route list` + help = 28 of 32 after P11.5): extend `status`.
- `board_run.py`/`eval_board.py` read the new fields in the same commit (rule above); A
  has none of them, which the self-tests already cover.
- The optional cycle count of the P3 libraries on silicon is not in the first board run
  (decision below).
- **Verify:** all four builds `-Wall -Wextra` clean, `trace.bat`, `hosttest.bat`,
  **[SMOKE]** with `tests/smoke/expected.log` updated on purpose (the diff is the
  review); the stack paint checked in the simulator (the high-water mark after the smoke
  script is plausible and below SPLIM).

### BR.7 Documentation

- `CLAUDE.md`: module rows for `board_run.py`, `eval_board.py`, `tests/board/expected.json`,
  `board_run/` (the committed hex files); in "Build and verify" a paragraph on the board run, the
  reply-format rule above, and "a board run goes through `board_run.py`".
- `README.md`: "Running the board test" (pointer to `README-colleague.txt`);
  `docs/CHAIN-TEST-PLAN.md` section 7 replaced by the new procedure;
  `docs/TROUBLESHOOTING.md`: port busy, no reply, timeout/reset, pip behind a proxy.

### BR.8 Package for the first board run (after P12)

- `board_run/B-EV74H48A-<rev>.hex` built from a clean `git worktree` of the P12 close-out
  revision, `SHA256SUMS.txt` and `board_run/README.md` updated, committed; the two
  SHA-256 recorded in the HARDWARE-LOG entry of P12.3. B is rebuilt and recommitted only
  when a board run is prepared, never on every commit.

### BR.9 The board run and the loop back

1. The session zip is copied unchanged to `docs/logs/run<NN>-<date>/` and committed as
   it came.
2. `eval_board.py` on it; the report committed beside it.
3. Every deviation becomes a correction card here (block, A/B values, expectation,
   suspected source file, how it is verified without a board and on the next run) or an
   explained deviation in the HARDWARE-LOG entry (missed prediction, known instrument
   fault such as S0's PLL readings or SCCP1's OC mode, present in A as well) - never
   dropped silently.
4. HARDWARE-LOG: a dated entry for the run, predictions that turned out wrong included,
   and one per change made in reaction.
5. `tests/board/expected.json` updated from the confirmed values, tagged with the run
   number.
5a. `python docs/gen_architecture.py --apply-run <session zip>` (since 28.09.2026): every
   module whose board-run blocks all passed in B turns green in `docs/ARCHITECTURE.md`'s
   diagrams (`docs/test_status.json`); `docs/TEST-COVERAGE.md` updated to match.
6. The next run uses the same runner with the new B; A stays the fixed baseline.

**Pass criterion for N+1:** B complete (every block, `@END`, no timeout, no trap); every
deviation of B from A absent or explained; `chain all` S4/S6/S9 at 8 MSPS without
overrun/late/missed; every `stream grab` cycle with a clean CRC and triangle verdict; the
stack high-water mark leaves at least 25 % of the stack unused. Only then N+1 counts as
"run on silicon".

---

## DBG: memory access for remote diagnosis (after BR.9)

Added 28.09.2026 (user decision). **Why:** since the relay (`bench_client`), an agent can
drive the board's console from here - but every question about a register or variable
the firmware does not already print costs a rebuild, a flash and a run. A read/write
command answers such a question in one console line, while the chain streams, and
narrows a board-run deviation down without a second board run - the cost CLAUDE.md's
rule "a board run costs a person their afternoon" is about.

**Why after BR.9, not before:** any firmware change invalidates the committed B image
(`dead53c`, SHA-256 in `board_run/SHA256SUMS.txt`); the first board run after N+1 tests
exactly N+1. DBG goes into the next B image, so it is available for chasing the
deviations BR.9 produces.

### DBG.1 `mem` command (firmware)

- One parser slot, sub-commands dispatched inside `cmd_mem_fn()` like `stream`/`route`
  (28 of 32 slots in use before; 29 after): `mem rd <addr> [n]` (n 32-bit words, `n` <=
  16, one `addr: value` line each) and `mem wr <addr> <value> [mask]` (read-modify-write
  of the bits in `mask`, default all; the value read back is the reply).
- Lives in `src/diag/diag.c` (diagnosis, not a driver); `cli.c` only registers and
  parses.
- **Checked before any access**, never trapped on: 4-byte alignment (a misaligned
  32-bit access is an address-error trap on silicon, see [SMOKE]'s fault case 1) and
  the address against a table of mapped ranges taken from the device pack's ATDF /
  linker script for the MPS512 and the MPS506 (RAM, SFR space read/write, flash
  read-only; anything else refused with one line naming the range it is not in). The
  table cites its source like every register value in this repository.
- **Writes:** only RAM and SFR space; each one is echoed with the old and new value, so
  the console log is the record. No unlock sequences (protected registers such as the
  PLL's stay out of reach on purpose - that is `clock.c`'s job).
- **Stated in `help` and in CLAUDE.md:** `mem wr` deliberately bypasses the register
  ownership rules ("nobody outside `dma.c` touches a DMA register"); it is a diagnosis
  tool, and a write to a clock or DMA register while the chain streams can hang the
  chip (recovery: reset, as for any board-run timeout).
- Verify: host test of the address check (`tests/host/test_mem.c`: every range edge,
  misaligned, unmapped, flash write refused); [SMOKE] with `mem rd` of a known variable
  and a refused misaligned read added to the script (`expected.log` updated);
  `-Wall -Wextra` clean on hw/sim/nano/smoke; `trace.bat` unchanged; fncmp of both ISRs
  unchanged.

### DBG.2 `tools/sym.py` (host)

- Resolves a name to an address so the agent writes `mem rd dma_overrun` rather than a
  hex number: firmware variables from the build's `.map`/ELF (`xc-dsc-objdump -t`), SFR
  names (e.g. `CLK6CON`, `DMA0STAT`) from the pack's device header/ATDF. Checks that the
  ELF it reads matches the revision in the board's boot banner (the same check
  `board_run.py --remote` makes for the hex), and refuses otherwise - a stale map gives
  the wrong address silently.
- A `mem` wrapper usable over `protocol.Target` (local COM port or the bench_client
  tunnel): `python tools/sym.py rd dma_overrun`, `... wr CLK6CON 0x... --mask ...`.
- Verify: self-test against a committed small ELF/map fixture and the replay target.

### DBG.3 Documentation and board-run integration

- CLAUDE.md: module rows (`diag.c` gains `mem`), the parser slot count (29/32), the
  exception to the register-ownership rule; README command table; HARDWARE-LOG entry
  for the first board run that uses it.
- `tools/board_run.py`: `help`'s new line is a reply-format change (BR rule) -
  `board_run.py`/`eval_board.py` updated in the same commit; optionally one `mem rd`
  of a variable `status` already reports, as a cross-check in R7.
- `docs/test_status.json` / `docs/TEST-COVERAGE.md`: the new code in `diag.c` starts
  amber/never like any new code.

## TRG: trigger mode in the GUI (host only)

Added 29.09.2026 (user request). **Goal:** a "trigger" checkbox with a level next to it
in the acquisition card; with it ticked, every grab is shown from the point where the
signal first crosses that level, so a periodic signal stands still in the time plot.
Switchable at run time, without touching the stream.

**Design decision 1 - a fixed display window, not a rotation.** The request as first
stated was: search the transferred half from the front, send from the crossing to the
end, then wrap to the start of the buffer up to the sample before the crossing. That
does not give a standing picture. The half is one contiguous time window x[0..N-1];
after the wrap, x[N-1] is followed by x[0], which is N samples earlier, not one. For
any signal whose period does not divide N exactly, that seam is a visible jump at
position N-k, and it moves with the trigger index k from grab to grab. It would also
break the analysis: `chain_tri_eval()` reads the seam as a lost sample (FAIL on every
grab), and the FFT gets a discontinuity in the middle of its window (leakage, wrong
SNR/THD). Instead, the oscilloscope rule: the display shows a fixed length L (default
N/2), the trigger is searched only in x[0..N-L], and x[k..k+L) is shown - contiguous
by construction, no seam.

**Design decision 2 - in the GUI, not in the firmware.** The GUI already holds the
whole half once `parse_grab_frame()` returns. Searching there needs no firmware change,
no wire-protocol change (no new GRAB header field), no trace/[SMOKE]/goldens, no
`board_run.py`/`eval_board.py` update (the BR rule) and no board run; the triangle
evaluation and the FFT keep working on the full, unrotated window. The search itself
is not a cost argument either way (~2048 compares, about 40 us on the CPU while the
stream is halted anyway, against milliseconds of UART transfer). The firmware variant
only pays once the transfer is cut to L samples to raise the frame rate - TRG.7,
optional.

### TRG.1 Search function

- `find_trigger(samples, level, slope, hyst, search_end) -> (k, frac) | None`, pure,
  no NiceGUI (in `tools/adc_gui.py` or a small `tools/trigger.py`).
- Rising edge: armed once a sample is below `level - hyst`, fires at the first sample
  `>= level` after that; falling edge mirrored. The hysteresis keeps ADC noise (a few
  LSB) from firing several times on a slow slope.
- `frac`: the crossing linearly interpolated between x[k-1] and x[k], for an optional
  sub-sample shift of the x axis - without it the picture jitters by up to one sample
  (125 ns at 8 MSPS), visible only at high signal-to-sample-rate ratios.

### TRG.2 Controls

- In the acquisition card next to "grab interval, ms": checkbox "trigger", number
  "level" (ADC counts 0..4095, default 2048), edge rising/falling, hysteresis (LSB,
  default 16). Tooltips like the other fields.
- Values live in `state`; a change takes effect with the next grab - switching on and
  off while LIVE needs nothing else.

### TRG.3 Display

- In `one_cycle()`, only the time series changes when the trigger is on: `L = N // 2`,
  search in x[0..N-L], plot x[k..k+L). x axis in samples and time relative to the
  trigger point (t = 0 at the crossing, `frac` applied when enabled).
- A dashed horizontal line at the level (ECharts `markLine`); a chip "trig @ k" or
  "no trigger".
- **No trigger found:** "auto" behaviour like an oscilloscope - x[0..L) untriggered,
  chip "no trigger", no error.
- FFT, spectrum metrics and the triangle card are untouched: they keep receiving the
  full `samples`.

### TRG.4 Self-test

- `adc_gui.py --selftest` gains: a sine of known phase - the index found is the
  expected crossing to within one sample; two `FakeTarget` grabs at different phases
  (its sine already follows wall-clock time, so consecutive grabs differ in phase) -
  the displayed windows agree at the trigger point to within the noise; a noisy slow
  slope - exactly one crossing with the hysteresis, several without it; a level outside
  the signal - "no trigger"; the triangle verdict identical with the trigger on and off.

### TRG.5 UI test

- `tools/gui_ui_test.py`: trigger on while LIVE (chip "trig @" appears, the level line
  is drawn), off again, no server-side exception; both board profiles as before.

### TRG.6 Documentation

- README (GUI section) and the `tools/adc_gui.py` row in CLAUDE.md. No HARDWARE-LOG
  entry for the change itself (no firmware); the first use on the board is noted with
  the board run it happens in.

### TRG.7 Firmware-side search with a shortened transfer (optional, later)

- Only if the GUI's frame rate turns out too low. `stream trig <level> <edge> | off` as
  a sub-command of `stream` (no parser slot), the search in `gui_link_stream_grab()`
  before `frame_send()`, L samples sent instead of N - still one contiguous
  `frame_send()`, never a wrap - and a header field `trig=<k>` (`protocol.py`'s regex).
- Brings the full chain CLAUDE.md asks for: `-Wall -Wextra` on hw/sim/nano/smoke,
  `trace.bat`, [SMOKE] (`help` changes), `board_run.py`/`eval_board.py` in the same
  commit (GRAB header and `help` are reply formats under the BR rule), a board run and
  its HARDWARE-LOG entry.

**Effort:** TRG.1-TRG.6 about 6-9 hours, host only; TRG.7 about two to three days
including the board run.

---

## SG: signal generator in firmware and GUI (N+3, brought forward)

Added 29.09.2026 (user request). **Goal:** requirement A2 (`docs/DESIGN-MULTICHANNEL.md`
section 1): the firmware computes a table from the `tab_wave_gen.py` parameters with
`lib/wavegen` (P3.6, done, host-tested, linked but called from nowhere), and plays it
through a DMA channel into a DAC at a selectable rate, with no CPU involvement. The GUI
sets the parameters, shows the expected table and, in the loop DAC2 -> RA8 -> ADC core 5
-> chain stream, compares what comes back with what was sent. That loop is the test the
design asks for (DESIGN-MULTICHANNEL 5, "the captured table must match") and gives the
GUI a known signal other than the triangle.

This is N+3 from "After N+1" below. It comes before N+2 (P8, drivers with instances), so
the new channel and the new SCCP get narrow functions of their own in `dma.c`/`sccp.c`.
P8 folds them into the instance API later. Nothing of SG has run on silicon.

**What the code and the ATDF already say (checked 29.09.2026, pack 1.4.260):**

- **The DMA address window is global.** `DMALOW`/`DMAHIGH` exist once, not per channel,
  and `dma0_init()` sets them to exactly the ADC buffer (`dma.c`, "the window is the
  destination buffer itself"). A second channel whose table lies outside that window
  may be refused with `ADRERR` (checked "every transaction", 13.4.8.1 p829). Whether
  the check also covers a channel's RAM *source* is not settled: the board has only
  shown that an SFR source below `DMALOW` passes.
- **`dma0_init()` switches the whole controller off** (`DMACONbits.ON = 0`) and back
  on. Every `stream on` would therefore stop a running generator.
- **`DACxDAT` is 32 bits:** `DACLOW` in 15:0, `DACDAT` in 31:16 (pack header
  `p33AK512MPS512.h`). A DMA write of the table value has to land in the upper half.
  Either a 16-bit transfer to `&DACxDAT + 2`, if a halfword write to an SFR's upper
  half is allowed, or 32-bit transfers from a `uint32_t` table (twice the RAM).
- **DMA trigger:** ATDF `DMA_SEL__CHSEL` 0x19 = "SCCP2" (0x18 SCCP1, 0x0E TMR2,
  0x05 TMR1). Which SCCP2 event raises it (timer period or only IC/OC) is not given.
  Run 18 found OC mode produced no ADC events and timer mode did.
- **DESIGN-MULTICHANNEL 4.2 says "Repeated Continuous".** That is `TRMODE = 3`, the
  mode that copied a whole block per trigger and caused runs 1-18's false rates. The
  generator uses `TRMODE = 1` (Repeated One-Shot, one table entry per trigger) like the
  ADC channel. SG.9 corrects 4.2.
- **Console line: 64 characters** (`CMD_PARSER_LINE_MAX_LEN`). The design's one-liner
  `siggen 8192 50000 f0=10000 h2=0.3 decay=20 amp=0.8` fits, but not with all six
  harmonics. So the parameters are set one per line (SG.4).
- **SCCP1's clock, CLKGEN13, is PLL1 out / 2.** When anything changes PLL1 while the
  generator runs, its rate moves too. SG.0 decides the clock (SG.2).
- RAM: 64 KB (`data` 0x4000, size 0x10000), 14.8 KB used at P0.1. The stack takes the
  rest (BR.6), and BR's criterion is at least 25 % of it unused.
- Parser slots: 27 + help = 28 of 32 in use; DBG.1 takes one, SG one.

**Design decision 1 - one window over both regions.** `dma.c` sets `DMALOW`/`DMAHIGH`
to the smallest range covering the ADC buffer and the generator table. The table goes
*below* the ADC buffer (one linker-placed object, or both in one named section), so the
buffer's end is still the window's end: an ADC overrun past the buffer still hits
`DMAHIGH` and stops the channel as today (fail 8). Its guard words stay. What is lost is
the hardware fence between table and buffer, which neither channel's configuration can
cross. `dma0_init()` no longer toggles `DMACON.ON` while another channel is enabled.

**Design decision 2 - the table is computed on the target, parameters only over the
wire** (decision of 26.09.2026, DESIGN-MULTICHANNEL 4.2, unchanged). The GUI computes
the same table from the same formula for its preview and for the loop comparison.

**Design decision 3 - no interrupt for the generator.** Channel 1 runs with
`HALFEN = DONEEN = 0`, `RELOADS`/`RELOADC` set. Errors (`ADRERR`, `BUSERR`) are read
back by `siggen` status rather than counted in an ISR. `_DMA0Interrupt` stays 42/0
(fncmp).

**Design decision 4 - checked defaults, explicit override.** Output range 205..3890
(ATDF, Example 18-3's note) by default. `force` lets any `lo`/`hi` through, as `dac ...
force` does, and the GUI ticks it by default (owner's choice for the DAC, 29.09.2026).
Every refusal names the violated limit with its number.

### SG.0 Datasheet check (docs only)

- Write the answers into this section with page/table, before any code:
  - Does the DMA window check apply to a RAM source (13.4.8.1)? Decision 1 holds
    either way; the answer says whether it was needed.
  - Is a 16-bit write to `DACxDAT`'s upper half allowed, from the CPU and from the
    DMA? If not: 32-bit transfers, a `uint32_t` table, `SIGGEN_N_MAX` 4096.
  - The DAC's update-rate/settling limit (DAC chapter 18, Table 40-x). It sets
    `play_hz`'s upper bound.
  - Which SCCP2 event raises the DMA request in timer mode.
  - SCCP2's clock: CLKGEN13 (as SCCP1, moves with PLL1), or the peripheral clock
    (PLL2, independent of every ADC rate change, maximum 200 MHz, Table 40-24).
    Recommended: the peripheral clock. The generator has no reason to share the ADC's
    clock, and a rate that changes under a running `stream on` is exactly the kind of
    coupling that cost runs 5-7.
  - Which DAC drives `DACOUT1`/`DACOUT2`, and whether the DAC runs in basic (DC) mode
    with `UPDTRG = 3` (every write taken at once), as `dac_level_start()` already does.
- Output: the fixed values for SG.1-SG.3 and the answer to "16 or 32 bit". Effort:
  half a day.

### SG.1 `dma.c`: channel 1 as a transmit channel

- `dma1_tx_start(trigger, src_table, n, dst_sfr, size)` / `dma1_tx_stop()` /
  `dma1_tx_status()` / `dma1_regs_visit()`. Repeated One-Shot, source incremented,
  destination fixed, `RELOADS`/`RELOADC`, no interrupt. Datasheet page per register as
  everywhere.
- The shared window (decision 1): `dma.c` keeps both regions and writes
  `DMALOW`/`DMAHIGH` from them. `dma0_init()` leaves `DMACON.ON` alone when channel 1
  is enabled.
- `sim_dma.c`: stand-ins with the same names (a variable per register, as for channel 0).
- Checks: `_DMA0Interrupt` 42/0 unchanged (fncmp). `trace.bat` - the `stream_on`-type
  goldens change only if the window or the `DMACON` sequence changes. The task says
  which lines changed and why.

### SG.2 `sccp.c`: SCCP2 as the playback clock

- `sccp2_start(ticks, clk)` / `sccp2_stop()` / `sccp2_hz()` / `sccp2_regs_visit()`,
  timer mode, the clock from SG.0. Register layout checked against SCCP1's in the ATDF,
  not assumed.
- `siggen_actual_hz()` = clock / ticks, rounded ticks, reported.
- Trace scenario `sccp2` with golden.

### SG.3 `src/siggen/siggen.c/.h`

- Owns the table (`SIGGEN_N_MAX` from SG.0: 8192 x 2 B = 16 KB if the stack keeps at
  least 25 % unused after BR.6's measurement, otherwise 4096), a `wavegen_cfg_t` with
  the defaults of `tab_wave_gen.py`, and the running state.
- `siggen_set(param, value)`, `siggen_start(dac, n, play_hz, snap)` (`wavegen_fill()`,
  then `dac_level_start(dac, table[0])`, then `dma1_tx_start()`, then `sccp2_start()`),
  `siggen_stop()` (the reverse order), `siggen_running()`/`_dac()`/`_actual_hz()`/
  `_f0_used()`, `siggen_visit()` for the status report (print-free, like
  `routing_visit()`).
- No register of its own, no device header; it calls `dma.c`, `sccp.c`, `dac.c` and
  `lib/wavegen` only.
- Every application path that starts or stops a DAC stops the generator on that unit
  first: `cmd_dac_fn()`, `run_dactest()`, `acq_triangle_for()`, `acq_chain_restore()`,
  `dactest.c`. `grep -rn "dac_\(off\|all_off\|triangle\|level\)" src/` is the checklist.
  A driver cannot call up, so this lives in the callers.
- `tests/host/test_siggen.c`, drivers stubbed with counters (the `test_routing.c`
  pattern): the order of start/stop, refusal before any driver call for every wavegen
  error code, and `snap`'s reported f0.

### SG.4 Console command `siggen` (one parser slot)

```
siggen set <param> <value>        f0 h2..h7 decay amp lo hi   (one per line, 64-char limit)
siggen on <dac> <n> <play_hz> [snap] [force]
siggen off
siggen                            status: every parameter, dac, n, play_hz set/actual,
                                  f0 set/used, table min/max, dma1 status
```

- Fractional values (`f0`, `h2..h7`, `decay`, `amp`) through a new
  `fmt_parse_dec()` in `lib/fmt` (at most six decimals, no `strtof`, no locale).
  `tests/host/test_fmt.c` gets its cases.
- Reply lines sized to the longest one, stated in the comment (CLAUDE.md rule).
- A change to `help`, so under the BR rule: [SMOKE] with `--update-expected`, and
  `board_run.py`/`eval_board.py` in the same commit, checked by their self-tests.

### SG.5 Routing: the generator's resources

- `routing.c`'s resource table gains the generator's claim (DMA 1, SCCP 2, one DAC
  output, the table's RAM against `ROUTE_RAM_BUDGET_BYTES`). `route list` shows it.
- Conflicts, each with its own `route_err_t` and host test: the test-form
  `stream on <ksps>` (`ROUTE_STREAM`, DAC2 triangle) while the generator is on DAC2,
  and the reverse.
- The loop needs no new route shape: `stream on <ksps> 5 3` (custom form, DAC left
  alone) reads RA8 = DACOUT2 while the generator drives it.

### SG.6 GUI: signal generator card

- `tools/wavegen_model.py`: the formula, moved from `tests/ref/wavegen_ref.py` (which
  then imports it). One model for the host test, the GUI and `FakeTarget`, the
  `eval_chain.py` rule.
- A card next to the acquisition card with: DAC 1/2, n, play_hz, f0, h2..h7, decay,
  amp, lo/hi, snap (default on), force (default on, decision 4), on/off. On change it
  sends `siggen set ...` line by line, then `siggen on ...`, and shows the actual
  play_hz and the f0 used from the reply.
- The table preview, computed by `wavegen_model.py` with the firmware's reported
  values.
- "Loop" preset: generator on DAC2, acquisition on core 5 / PINSEL 3 (RA8).
- In the loop, the time plot overlays the expected signal: the table resampled at the
  frame's `ksps`, aligned by cross-correlation. A chip shows the RMS error in LSB and
  the delay. The spectrum shows f0 and the harmonics against the set factors.
- `FakeTarget` plays the table (at `play_hz`, sampled at `ksps`, plus a modelled DAC
  settling) when the input is RA8 and the generator is on.

### SG.7 GUI tests

- `adc_gui.py --selftest`: `wavegen_model.py` against `tests/ref` vectors; `siggen`
  set/on/off against `FakeTarget`, reply parsing; the loop with the fake. It must
  recover the RMS error ~0 and the right delay, and fail visibly with a wrong
  harmonic. A 64-character overflow is refused by the sender, not truncated.
- `gui_ui_test.py`: generator card on/off, loop preset, overlay drawn, both board
  profiles, no server-side exception.

### SG.8 Board run

- `board_run.py` block R8 "siggen": `siggen set f0 10000`, `h3 0.3`,
  `siggen on 2 1000 100000 snap`, `stream on 1000 5 3`, 10 grabs, then the play-rate
  ladder (10 k / 100 k / 1 M / the SG.0 limit) with the chain at 8 MSPS: `ov`/`late`
  counters per step (does a second DMA channel bring the overruns below 10 MSPS?),
  `siggen off`, `stream off`.
- `eval_board.py`: the loop match against `wavegen_model.py` (RMS error, delay, f0
  within the DAC's rounding); `tests/board/expected.json` entries `source: prediction`.
- The questions the run answers in one pass: does SCCP2 trigger the DMA at all
  (`dma1` transfer count = SCCP2 periods over the run); does the value reach
  `DACDAT`; is the RAM source inside the window accepted; the highest clean play_hz;
  whether the ADC chain's overrun threshold moves.
- **Fallback, decided now** so a failed first run does not stall the card: if SCCP2
  does not trigger the DMA, try TMR2 (0x0E) in the same B image (`siggen on ...
  trig=tmr2`, a hidden second trigger option). Only if neither works does a timer-ISR
  transport (`dac_set()` from `_T2Interrupt`, below ~200 kSps) stand in for the DMA -
  explicitly as "A2 not met", in the HARDWARE-LOG and on the console.
- A new B image, a dated HARDWARE-LOG entry, predictions that missed included.

### SG.9 Documentation

- CLAUDE.md: rows for `siggen.c`, the new `dma.c`/`sccp.c` functions, `wavegen_model.py`,
  the command in the parser-slot count; "Rules" unaffected.
- `docs/gen_architecture.py`: box `siggen` (app layer) with its arrows to
  dma/sccp/dac/wavegen, regenerate both SVGs; `docs/test_status.json` entry with its
  open gaps; `docs/TEST-COVERAGE.md`.
- DESIGN-MULTICHANNEL 4.2: `TRMODE = 1` instead of "Repeated Continuous", the shared
  window, per-line parameters. README: the command and the GUI card.

**Effort:** SG.0 half a day; SG.1-SG.5 (firmware, tests, goldens, [SMOKE],
board_run/eval_board) two and a half to three days; SG.6-SG.7 (GUI) one to one and a
half days; SG.9 half a day. Together about four and a half to five and a half working
days, plus the board run in SG.8 (and a second one if the fallback is needed).

---

## Order and dependencies

```
P0 ──► P1 ──► P2 ──► P3
              │
              └────► P4 ──► P5 ──► P6 ──► P7 ──► P8 ──► P9 ──► P10 ──► P11 ──► P12
```

- **Since 27.09.2026, P8 and P10 are not part of N+1** (moved to N+2): P9 follows P7,
  P11 follows P9. Cards that touch no shared file run in parallel git worktrees.
- P3 (the libraries) depends only on P0 and P1 and can run alongside P4 to P7.
- **[SMOKE]** runs after P1, P5.2, P6.1, P7, P9.2 and P11.5, and after any other task
  that touches `main.c`, the console or the memory layout.
- **[SIM]** (full acceptance, ~7 min; without asking since 27.09.2026, one run at a time) runs at P9.5 and P12.4.
- **BR**: BR.1-BR.4 (tools/ and tests/ only) in a worktree alongside P9-P11; BR.5 merges
  them after P11; BR.6 (firmware, [SMOKE]) after P11 and before P12; BR.7 with or after
  BR.6; BR.8 (B image) after P12; BR.9 is the board run.
- **TRG.1-TRG.6** touch `tools/` only and depend on nothing above - any time, also
  alongside BR/DBG. TRG.7 (firmware) goes into a B image like DBG, after BR.9.
- **SG**: SG.0 first (docs only). SG.1/SG.2 in parallel, then SG.3-SG.5. SG.6/SG.7 can
  start after SG.4's command syntax is fixed, against `FakeTarget`. SG.8 goes into a B
  image after DBG's, SG.9 with each card and closed at the end.
- The only real risk of changing timing lies in P8.1 (DMA ISR), now in N+2. It is checked with
  `fncmp` (no indirect call, instruction count recorded), but only a board run can
  confirm it.

## After N+1 (outline only)

| Version | Content |
|---|---|
| N+2 | P8 (drivers with instances) and P10 (split `clock.c`), moved here from N+1 on 27.09.2026; further single-channel routes (other core, pin, DAC1..8 as the source); the `test` suite on `ROUTE_B2B` |
| N+3 | signal generator `siggen/`: table → DMA → DAC, playback clock from an SCCP, using `lib/wavegen` - planned in detail as section SG (29.09.2026), brought forward before N+2 |
| N+4 | processing chain `dsp_run/` with `lib/goertzel_f` (default), `goertzel_i`, `detect`; measure the cycles per block |
| N+5 | multi-channel `acq/`, up to 5 cores, common trigger |

Before N+3, confirm in the datasheet: the DMA `CHSEL` codes for the SCCP/timer
triggers, the DAC update-rate limit, `DACxDAT` as a DMA target (bits 31:16), and which
DAC can drive `DACOUT1`/`DACOUT2`.
