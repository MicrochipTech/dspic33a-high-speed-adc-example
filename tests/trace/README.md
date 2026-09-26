# Register trace - decision from the P0.3 spike (26.09.2026)

The question: how does a driver, compiled unchanged on the host with MinGW gcc,
record what it writes to the SFRs, so that later tasks can prove "trace unchanged" -
**in order**, because P10 splits `clock.c` and the order of the clock writes is the
thing that must not change.

## Decision: plain C, SFRs in one page-guarded array (hybrid of a and b)

- **Header.** `tools/gen_fake_sfr.py` turns the DFP's device header into a host
  `xc.h`: everything verbatim (bit-field typedefs, `_X_F_MASK`, `_F` short names,
  `#define X X`), only `__attribute__((__sfr__))` dropped. The addresses come from the
  DFP's `.gld` into a generated `sfr_syms.ld` that places every `X` and `Xbits` at
  `sfr_mem + 4*idx` - one contiguous, page-aligned array, the job the device's linker
  script does. No SFR name becomes a macro, `&X` stays a link-time constant.
- **Recording.** The recorder protects the pages of `sfr_mem` (`VirtualProtect`,
  `PAGE_NOACCESS`). Every access - by name, through a bit field, through a pointer
  (`adc.c`'s `adc_cur->CON`) - raises an access violation; a vectored exception handler
  learns address and read/write from the exception record, unprotects, single-steps the
  one instruction (trap flag), logs, and protects again. Result: **every write, in
  program order, including writes that leave the value unchanged**, and optionally every
  read. The same array still allows a snapshot diff (mode 1) for debugging.
- **Read hooks** run before a trapped read and play the hardware (below).

Why not the two candidates of the plan (all tried on `timebase.c` and `dma.c`,
`tests/trace/spike/run.sh`, output in `build/trace_spike/`):

| | (a) snapshot, plain array | (a') access function per use | (b) C++ proxies | **chosen: guarded array** |
|---|---|---|---|---|
| order | only between trace points; `dma0_init()`'s 24 writes came out as 9 lines in *index* order (IEC2 before DMACON) | per access | per access | **per access** |
| unchanged-value writes (`DMA0STAT = 0`, `T1CON = 0`) | invisible | invisible | visible | **visible** |
| drivers compile unchanged | yes (C) | **no**: 36 register names are also bit-field names (`PC`, `SPLIM`, `FSCL`...) and break as macros; `&X` in `adc.c`'s static table is no constant | `timebase.c`, `clock.c`, `sccp.c`, `dac.c`, `chaintest.c` yes; **`dma.c` no** (3x `(uint32_t)ptr` is an error in C++ on 64 bit), **`adc.c` no** (`ADCBITS()` casts a pointer to the bit type: the proxy is bound to AD3's index, not to the pointer), `capture.c` no (`_Static_assert`) | **yes, all 17 files** (only `-Wno-pointer-to-int-cast`) |
| polling loops | preset only; a bit the driver sets itself (`PLLSWEN`) never clears -> `fail(1)` | read hook | read hook | **read hook** |
| accesses through pointers | seen by the diff | not hooked | wrong register | **trapped like any other** |

Measured: `trace_c1.txt` (38 lines) vs `trace_c3.txt` (59 lines) for the same scenario;
`timebase_init()` is 2 writes in (a) and the real 5 (`T1CON=0, TMR1=0, PR1, TCKPS, ON`)
in the chosen mode. Two runs give byte-identical traces. `clock.c`'s `clock_init()` and
`clock_adc_set_div(200)` run through all their waits with the hooks
(`trace_poll.txt`, 54 trapped accesses) and end in `fail(1)` without them.

## Trace format (spike; P0.4 fixes it)

    W PLL1CON      0x00008100 -> 0x40008100      write, old -> new
    R CLK6CON x3                                 read(s), with TRACE_READS=1 only
    C [dma] DMALOW: &buf+0x0                     console output of the driver
    D __delay32(20000000)                        stubs (delay, dma0_event, capture_halt)
    F fail(8)                                    fail() - longjmp back to the scenario
    # dma0_deinit()                              the scenario's own comments

Values that are host addresses are printed symbolically: `&AD5CH0RES(0x000DA4)` for
an SFR (the device address from the gld), `&buf+0x0` for a RAM region the scenario
registered (`trace_region()`). Otherwise the truncated 64-bit host address would be
neither the target's value nor deterministic.

## Polling loops and how each is satisfied

Every SFR wait in the drivers is bounded (`--n` countdown or `WAIT_WHILE` -> `fail()`),
so none hangs - but an unanswered one burns its bound: 2 M trapped reads = 54 s
(~25 us per trapped access). The hooks must answer all of them.

| Where | Waits on | Bit is | Hook |
|---|---|---|---|
| clock.c 107, 163 | `CLK1CON.OSWEN` | self-clearing | clear on read |
| clock.c 115/138, 590, 641 | `PLLxCON.PLLSWEN` | self-clearing | clear on read |
| clock.c 117/140, 594 | `PLLxCON.FOUTSWEN` | self-clearing | clear on read |
| clock.c 119/142 | `PLLxCON.OSWEN` | self-clearing | clear on read |
| clock.c 130/150 | `PLLxCON.DIVSWEN` | self-clearing | clear on read |
| clock.c 120/143, 597, 644 | `OSCCTRL.PLLxRDY` | set by hardware (lock) | set on read |
| clock.c 175, 350, 398 | `CLK6/7/13CON.OSWEN` | self-clearing | clear on read |
| clock.c 274, 402 | `CLK6/13CON.DIVSWEN` | self-clearing | clear on read |
| clock.c 277, 315, 353, 405, 600, 647 | `CLKxCON.CLKRDY` | set by hardware | set on read |
| clock.c 493 | `CM4STAT.BUFV` | set by hardware (window done) | set on read; `CM4BUF` needs a plausible count |
| adc.c 131, 287 | `ADxCON.ADRDY` (via `adc_cur` pointer) | set by hardware after `ON` | set on read (pointer access is trapped too) |
| cli.c 152 | `U2STAT.TXBF` | hardware (FIFO full) | keep 0 |
| cli.c 164 | `U2STAT.TXMTIF` | hardware (shift register empty) | keep 1 |
| cli.c 221, 256, 280 | `U2STAT.TXBF`/`RXBE` as loop conditions | hardware | TXBF 0, RXBE 1 (no input) |
| capture.c 1130, 1154; chaintest.c 188; clock.c 519 (sim path only) | `TMR1` via `timebase_ticks()` | counts | +K ticks per read |
| capture.c 1104; chaintest.c 267, 1072 | RAM flags/counters set by ISRs, with a `TMR1` timeout | software | end by timeout only - see open points |
| capture.c 257, 802; cli.c 826, 1282 | `burst_active`, `blocks_done` (RAM, ISR) | software | never satisfied by an SFR hook - see open points |

`timebase.c`, `dma.c`, `dac.c`, `sccp.c`, `led.c` have no SFR polling loop.
`diag.c`'s `for (;;)` blink loops only run after `fail()`, which the host stubs.

## Stubs and compiler features

- `__delay32(n)`: a `D` line, and TMR1 advances by n/16 (200 MHz CPU, 12.5 MHz Timer1),
  so `timebase_check()` returns a fixed value (1250000, +1 per TMR1 read with the hook).
- `__attribute__((interrupt, no_auto_psv))`, `persistent`: `sfr_host.h` defines
  `interrupt`, `no_auto_psv`, `persistent` as `__unused__` (x86 gcc knows `interrupt`
  with another signature and refuses `void f(void)`). The words occur in no driver as
  identifiers (grep). ISRs are then ordinary functions the scenario calls directly.
- `Nop()`, `ClrWdt()`, `__builtin_nop()` -> `((void)0)`.
- `__asm__ volatile ("reset")` (cli.c): the host assembler rejects it; `sfr_host.h` defines
  an empty assembler macro `reset`, so it assembles to nothing.
- `#pragma config` (config_bits.c): warnings only (-Wunknown-pragmas); not needed on host.
- `console_*`: stubs write `C` lines (spike). `fail()` writes `F` and `longjmp`s.
- Compiler flags that are required, not taste: **`-mno-ms-bitfields`** (MinGW's default
  ms_struct layout makes 916 of 1568 bit-field typedefs larger than 4 bytes; the generated
  `sfr_layout_check()` compares all 11755 named fields with the pack's `_MASK` values -
  0 differ with the flag), `-fno-strict-aliasing` (`X` and `Xbits` alias),
  `-Wl,--disable-dynamicbase` (the placed symbols are absolute; with ASLR the first SFR
  access segfaults), `-Wno-pointer-to-int-cast` (dma.c's `(uint32_t)ptr`). The linker
  prints "stripping non-representable symbol" for the placed symbols: harmless.
- `__DATA_BASE/__DATA_LENGTH` are widened to 0x4 / 0xFFFFFFF8 in the host header, else
  every truncated host buffer address is "outside RAM" and `dma0_init()` fails(8).

## What the trace cannot see

- **Timing.** No cycles; delays only as `D` lines; TMR1 is a model.
- **Hardware side effects** beyond the hooks: no clock switches, DMA transfers,
  conversions or interrupts happen unless a hook or the scenario makes them.
- **Register semantics.** An SFR is a memory cell: write-1/0-to-clear (`DMA0STAT = ~flags`
  stores 0xFFFFFFEF), read-to-clear (`CHxRDY` on reading `CHxDATA`), read-only bits and
  `SET/CLR/INV` behaviour are not modelled; later reads see what was written.
- **Access width.** A bit-field store may be a byte store on the host and a 32-bit or
  `bset` on the dsPIC; the trace shows the register value, not the width.
- **Reads are compiler-shaped.** gcc turns some bit-field writes into one read-modify-write
  instruction (trapped as one write) and others into load + store (read, then write), so
  `R` lines depend on flags. Golden traces compare `W`, `C`, `D`, `F`; `R` is a debug aid.
- The **device reset values** (all 0 in the spike; the gld/header have none).
- Code under `#ifdef __MPLAB_DEBUGGER_SIMULATOR` (the host takes the hardware path) and the
  other `BOARD`'s code unless built with `-DBOARD=2` against the MPS506 header.
- It is **Windows/x86-64 specific** (VEH + trap flag). A Linux port would be
  `mprotect` + SIGSEGV/SIGTRAP with the trap flag in `ucontext`; not needed today.

## Simulator (question for P0.8)

`tests/trace/spike/sim_sfr_probe.py` (MDB, sim build, 58 s Run, 99 s in total, not the
acceptance run): after boot (`boot_stage = 9`, `fail_code = 0`):

| SFR | reset (before Run) | after boot | firmware wrote |
|---|---|---|---|
| `T1CON` | 0x00000000 | 0x00008010 | 0x00008010 - kept |
| `VCO1DIV` | 0x00000000 | 0x00020000 | 0x00020000 - kept |
| `CLK6CON` | 0x00000101 | 0x00829501 | 0x00029500 + OSWEN - kept, status bits the simulator's |
| `PLL1DIV` | 0x0100C812 | **0x01000000** | 0x0100C829 - **not kept** |
| `PLL2DIV` | 0x0100C812 | **0x01000000** | 0x01007D29 - **not kept** |

So the simulator stores writes to some unmodelled SFRs and not to others (both PLL
divider registers lose the feedback and post-divider fields). P0.8 can compare values
only register by register where the simulator keeps them, and needs the reduced check
(addresses and bit positions against the ATDF) for the rest. The probe needed the same
`Set uart2io...`/`oscillator` settings as `sim_trap.py`; without them MDB answered no
`Print` after `Halt` (W0101-SIM NullPointerException). Boot to stage 9 takes ~55 s of
simulation because of the UART banner.

## Open points for P0.4

1. **Console output.** `cli.c` *implements* `console_*` on UART2, and the `boot` scenario
   needs `cli_init()`. Either link `cli.c` and turn consecutive `U2TXB` writes into `C`
   lines (the real text, through the real code; hooks keep TXBF 0 / TXMTIF 1 / RXBE 1),
   or stub `console_*` and leave `cli.c` out of scenarios. Recommended: the first.
2. **Software waits** on ISR-set RAM (`blocks_done`, `burst_active`, `adc_events`) need
   events: e.g. a read hook on `DMA0CH` (polled via `dma0_enabled()` in those loops) that
   sets `DMA0STAT.HALF/DONE` and calls `_DMA0Interrupt()`. Or keep those entry points out
   of P0.5's scenarios. Recommended: keep them out first, add the event hook when a
   scenario needs it.
3. **Time model.** +1 TMR1 tick per read makes `chain run`'s ms-long loops cost minutes
   (12 500 reads per ms, 25 us each). Advance K ticks per read, K per scenario.
4. **Poll runaway guard.** Abort a scenario with `POLL <reg>` after, say, 1000 consecutive
   reads of one register without a write, instead of burning a 2 M bound (54 s).
5. **Hardware-set values in the trace.** Hooks change values silently; log them as `H`
   lines, or not (they are the harness's, not the driver's).
6. **Reset values** from the ATDF (`initval`), for scenarios whose code branches on a
   register's state at entry (`clock_init()` reads `CLK1CON.COSC`).
7. **Static buffers** (`capture.c`'s `static dma_buffer`, section `.dma_buffer`): not
   reachable by name for `trace_region()`; a host linker script symbol around the section
   would make `DMA0DST` print as `&dma_buffer+0x0`.
8. **Generated files:** `gen_fake_sfr.py` output is 86 k lines (xc.h) + 15 k (sfr_table.c);
   generate into `build/` at each run (as the spike does, ~1 s) rather than commit it.
