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

As of 27.09.2026, 21:00. Done: 45 of 52 tasks in the N+1 scope (63 planned plus P0.9 = 64; P8 and P10, 12
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
| P11.4-P11.5 | open | | | P11.4 is the central proof |
| P12.1-P12.4 Close-out | open | | | P12.4 = [SIM], runs without asking since 27.09.2026 |
| BR.1 `tools/board_run.py` | in review | `175b20c` (worktree) | Sonnet | phase BR added 27.09.2026 (not counted in the 52); host-side, worktree beside P9/P11; selftest 10/10 |
| BR.2 `tools/eval_board.py` + `expected.json` | in progress (worktree) | | Sonnet | |
| BR.3 Python environment | in progress (worktree) | | Sonnet | |
| BR.4 `board_run/` hex files + README, runner finds them | open | | | worktree; replaces the package script (decision 8 changed) |
| BR.5 Merge of the worktree | open | | | after P11 |
| BR.6 Firmware additions for the board run | open | | | after P11, before P12; [SMOKE] + `expected.log` |
| BR.7 Documentation | open | | | with or after BR.6 |
| BR.8 B image for the first board run | open | | | after P12 |
| BR.9 Board run and loop back | open | | | needs the colleague and the EV74H48A |

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
| R4 | `stream on` at 1, 4, 8 MSPS, each >= 50 `stream grab`, `stream off` | the GUI path: per grab the ov/late/missed delta, CRC, triangle verdict (`eval_chain.tri_eval`/`grid_ok`); raw frame stored on FAIL |
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
6. The next run uses the same runner with the new B; A stays the fixed baseline.

**Pass criterion for N+1:** B complete (every block, `@END`, no timeout, no trap); every
deviation of B from A absent or explained; `chain all` S4/S6/S9 at 8 MSPS without
overrun/late/missed; every `stream grab` cycle with a clean CRC and triangle verdict; the
stack high-water mark leaves at least 25 % of the stack unused. Only then N+1 counts as
"run on silicon".

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
- The only real risk of changing timing lies in P8.1 (DMA ISR), now in N+2. It is checked with
  `fncmp` (no indirect call, instruction count recorded), but only a board run can
  confirm it.

## After N+1 (outline only)

| Version | Content |
|---|---|
| N+2 | P8 (drivers with instances) and P10 (split `clock.c`), moved here from N+1 on 27.09.2026; further single-channel routes (other core, pin, DAC1..8 as the source); the `test` suite on `ROUTE_B2B` |
| N+3 | signal generator `siggen/`: table → DMA → DAC, playback clock from an SCCP, using `lib/wavegen` |
| N+4 | processing chain `dsp_run/` with `lib/goertzel_f` (default), `goertzel_i`, `detect`; measure the cycles per block |
| N+5 | multi-channel `acq/`, up to 5 cores, common trigger |

Before N+3, confirm in the datasheet: the DMA `CHSEL` codes for the SCCP/timer
triggers, the DAC update-rate limit, `DACxDAT` as a DMA target (bits 31:16), and which
DAC can drive `DACOUT1`/`DACOUT2`.
