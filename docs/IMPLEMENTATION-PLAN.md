# Implementation plan: version N+1

As of 26.09.2026, revision `28e88fa`. Plan only; no task below has started.

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
6. **The simulator acceptance run (~7 min) only runs when the user asks.** The tasks
   that need it are marked **[SIM]**. They are collected so that one run covers
   several tasks.
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
  `docs/logs/`, if any; otherwise mark it as "to capture at the first [SIM] run".
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
  traces are unchanged; `tools\_test_mplabx.bat` builds.
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

## P4: Port layer (V2)

### P4.1 `src/port/log.h`, `src/port/panic.h`, `src/app/port_impl.c`

- `port_log(s)`, `port_log_kv(key, v, hex)`, `port_panic(code)` (noreturn). The
  implementation maps to `console_puts/kv/kv_hex` and `fail()`.
- **Done:** builds; nothing uses it yet.

### P4.2 to P4.6: one driver per task

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
  unchanged. **[SIM]** the console works (the banner and one command in the simulator
  run).

---

## P6: Splitting `cli.c` (V10, V9)

| Task | Content | Verify |
|---|---|---|
| **P6.1** | per-module command registration: `bench_register()`, `chain_register()`, `link_register()` etc.; `cli_init()` calls them in the old order | `help` lists the same commands in the same order (host test of the registration table, or **[SIM]**) |
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
- **Verify:** `boot` and `nano` traces unchanged.

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
| **P9.2** | simulator hooks out of `pingpong.c`: `sim_dma.c` checks the half itself | build sim; **[SIM]** |
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
  channel, SCCP and DAC is in use). It takes one parser slot (then 24 + help = 25 of
  32).
- **Verify:** host test of the output function via the visitor; **[SIM]** the command
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
- [SIM] runs are collected: after P5.2 + P6.1 (console), after P9 (acceptance), and at
  P12.4. That makes three runs of about 7 minutes each, each only on request.
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
