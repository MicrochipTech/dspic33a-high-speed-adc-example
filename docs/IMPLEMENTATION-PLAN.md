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

As of 27.09.2026. Done: 31 of 64 tasks (63 planned plus P0.9); P0.5b and P4.6a were
added along the way and are not counted.

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
| P5.1 `src/drivers/uart.c` | done | this commit | Sonnet | `_U2RXInterrupt` moved out of cli.c, 65 -> 55 instructions (the byte-counting/CR-LF/parser-feed body became `uart_rx_hook()`, a direct `rcall`, still 0 indirect calls); uart.c reports through neither `port/log.h` nor `port/wait.h` - it is what `console_puts()` writes through |
| P5.2 `cli.c` on top of `uart.c` | done | this commit | Sonnet | P5.1 already finished the switch (the plan allows that order); this task is the check: `grep -E "U2\|RPCON\|RPOR\|RPINR\|IPC" src/cli/` empty, **[SMOKE]** log identical to `tests/smoke/expected.log` (110 lines) |
| P6.1-P6.5 Split `cli.c` | open | | | |
| P7.1 Board config as data | open | | | |
| P8.1-P8.8 Drivers with instances | open | | | P8.1 is the only timing risk |
| P9.1-P9.5 Split `capture.c` | open | | | P9.5 = [SIM], ask the user first |
| P10.1-P10.4 Split `clock.c` | open | | | needs `trace_point()` between clock steps (approach (a)) |
| P11.1-P11.5 Routing core | open | | | P11.4 is the central proof |
| P12.1-P12.4 Close-out | open | | | P12.4 = [SIM], ask the user first |

### Decisions taken during the work

| Date | Decision | Where recorded |
|---|---|---|
| 26.09. | Register trace: snapshot diff in C (approach (a)), not the page-guard trace the spike recommended; `console_*` stubbed; ISR-driven waits left out; goldens compare writes, console, stubs, `fail()`, no reads | `tests/trace/README.md` |
| 27.09. | Hybrid: a page-guarded read hook serves only the polled registers; the model thread and all retries removed | `tests/trace/README.md` |
| 27.09. | [SMOKE] runs without asking, one run at a time; the ~7-minute [SIM] run stays on request | `CLAUDE.md` |
| 27.09. | Trace starts from the ATDF reset values (P0.9) | `tests/trace/README.md` |
| 27.09. | Goertzel: damped textbook form `q0 = s + 2D·cos·q1 − D²·q2` (the literal reading of the design is undamped) | `docs/DESIGN-MULTICHANNEL.md` 4.3 |
| 27.09. | Detector counts once per pulse: re-arm only below threshold × hysteresis | `docs/DESIGN-MULTICHANNEL.md` 4.3 |

### Open points found along the way

- MPS506 ATDF gives `CLK1CON` reset value 0x28180, the MPS512's 0x101 (the simulator
  confirms 0x101); probably an ATDF error. A smoke run for the MPS506 or the Nano board
  would settle it. No effect on N+1: `clock_init()` writes the whole word.
- diag.h's `WAIT_WHILE` has no user left after P4.7; `WAIT_LIMIT` is still used by
  `capture.c` and `main.c`.
- The trace cannot see a write of a register's reset value (approach (a)), e.g. `PR1`.
- A clean simulator run does not prove the absence of misaligned accesses; only
  silicon traps them.

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
| **P9.5** | **[SIM] acceptance run**, ask the user first: `sim_trap.py` default and at 256 samples per half, plus the `--fault` case | `[simtest] PASS`, `PASS`, `FAIL` with one mismatch at index 0 |

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

To be written into the firmware as far as possible, so that one run answers everything:

1. `chain all`: every stage as in the last run before N+1 (compare with the log in
   `docs/logs/`).
2. `test all`: the back-to-back rows as before.
3. `regs`: identical to a dump from the old firmware on the same board (bitwise
   diffable).
4. `capture_process_bench` and the stream counters: same order as before (the ISR
   changes in P8.1 are the only timing risk).
5. `route list` shows `ROUTE_STREAM` with DMA0, SCCP1, DAC2, core 5.

---

## Order and dependencies

```
P0 ──► P1 ──► P2 ──► P3
              │
              └────► P4 ──► P5 ──► P6 ──► P7 ──► P8 ──► P9 ──► P10 ──► P11 ──► P12
```

- P3 (the libraries) depends only on P0 and P1 and can run alongside P4 to P7.
- **[SMOKE]** runs after P1, P5.2, P6.1, P7, P9.2 and P11.5, and after any other task
  that touches `main.c`, the console or the memory layout.
- **[SIM]** (full acceptance, ~7 min, on request only) runs at P9.5 and P12.4.
- The only real risk of changing timing lies in P8.1 (DMA ISR). It is checked with
  `fncmp` (no indirect call, instruction count recorded), but only a board run can
  confirm it.

## After N+1 (outline only)

| Version | Content |
|---|---|
| N+2 | further single-channel routes (other core, pin, DAC1..8 as the source); the `test` suite on `ROUTE_B2B` |
| N+3 | signal generator `siggen/`: table → DMA → DAC, playback clock from an SCCP, using `lib/wavegen` |
| N+4 | processing chain `dsp_run/` with `lib/goertzel_f` (default), `goertzel_i`, `detect`; measure the cycles per block |
| N+5 | multi-channel `acq/`, up to 5 cores, common trigger |

Before N+3, confirm in the datasheet: the DMA `CHSEL` codes for the SCCP/timer
triggers, the DAC update-rate limit, `DACxDAT` as a DMA target (bits 31:16), and which
DAC can drive `DACOUT1`/`DACOUT2`.
