# P0.1 Baseline

Git revision: `b41af3b`. Date: 26.09.2026.

## Builds

All three variants build clean, `-Wall -Wextra`, no warnings or errors:

```
MSYS_NO_PATHCONV=1 cmd /c "cd /d C:\work\Claas\ADC\tools & C:\work\Claas\ADC\tools\build.bat"      < /dev/null
MSYS_NO_PATHCONV=1 cmd /c "cd /d C:\work\Claas\ADC\tools & C:\work\Claas\ADC\tools\build.bat sim"  < /dev/null
MSYS_NO_PATHCONV=1 cmd /c "cd /d C:\work\Claas\ADC\tools & C:\work\Claas\ADC\tools\build.bat nano" < /dev/null
```

(Running `build.bat` from a directory other than `tools\` fails: its source list is
relative, `..\main.c` etc., so the working directory has to be `tools\` when it runs -
plain `tools\build.bat` from Git Bash, or `cmd //c "...\build.bat"` without a `cd`
first, resolves `..\main.c` against the wrong directory and fails with
"No such file or directory". `cmd /c "cd /d <tools dir> & <full path>\build.bat ..."`
is the form that works from Git Bash.)

Compiler/linker output, tail of each build, no `warning`/`error` lines present:

```
Build OK: ..\build\adc_dma_40msps.elf
HEX written: ..\build\adc_dma_40msps.hex

Build OK: ..\build\adc_dma_40msps_sim.elf

Build OK: ..\build\adc_dma_40msps_nano.elf
HEX written: ..\build\adc_dma_40msps_nano.hex
```

## Memory usage

No `xc-dsc-size` (or any `*size*` tool) ships with this xc-dsc v3.31 install
(`C:\Program Files\Microchip\xc-dsc\v3.31\bin`, checked by directory listing and by a
recursive search for `*size*` under the install root - neither `xc-dsc-bin\bin` nor the
`elf-*` toolchain alongside it has one). The linker itself reports the memory summary
when asked for a map file (`xc-dsc-ld.exe --help` lists `-Map FILE`), so the numbers
below come from the "Total program/data memory used" lines of a linker map, produced by
re-running the exact `tools\build.bat` compiler invocation for each variant with
`-Wl,-Map=<out>.map` appended - same `-mcpu`/`-mdfp`/`-O1 -Wall -Wextra`/`-T` flags and
the same source list `build.bat` uses for that variant (`..\sim_dma.c` instead of
`..\dma.c` and `-D__MPLAB_DEBUGGER_SIMULATOR=1 -g` for `sim`; `-mcpu=33AK512MPS506
-DBOARD=2` for `nano`). Example, the hardware variant:

```
"C:\Program Files\Microchip\xc-dsc\v3.31\bin\xc-dsc-gcc.exe" ^
  -mcpu=33AK512MPS512 ^
  -mdfp="C:\Program Files\Microchip\MPLABX\v6.35\packs\Microchip\dsPIC33AK-MP_DFP\1.4.260\xc16" ^
  -O1 -Wall -Wextra ^
  -T"C:\Program Files\Microchip\MPLABX\v6.35\packs\Microchip\dsPIC33AK-MP_DFP\1.4.260\xc16\support\dsPIC33A\gld\p33AK512MPS512.gld" ^
  -Wl,-Map=..\build\hw.map ^
  ..\main.c ..\config_bits.c ..\clock.c ..\adc.c ..\dma.c ..\capture.c ..\crc16.c ..\sccp.c ..\led.c ..\diag.c ..\timebase.c ..\dac.c ..\dactest.c ..\chaintest.c ..\cli.c ..\cmd_parser.c ^
  -o ..\build\hw.elf
```

(run from `tools\`, e.g. via `cmd /c "cd /d C:\work\Claas\ADC\tools & <the line above>"`
from Git Bash - for `sim`/`nano` substitute the source file and extra flags as above).
The elf/map are build artefacts (`build\` is git-ignored) and are not checked in; the
figures below are copied from the map's own summary lines.

| Variant | Program memory used | of 0x7fffc (524284 B) region | Data memory used | of 0x10000 (65536 B) region |
|---|---|---|---|---|
| hardware (`build.bat`)      | 0x145c4 (83396 B) | 15% | 0x39e2 (14818 B) | 22% |
| simulator (`build.bat sim`) | 0xf50c  (62732 B) | 11% | 0x25d6 (9686 B)  | 14% |
| nano (`build.bat nano`)     | 0x145cc (83404 B) | 15% | 0x39de (14814 B) | 22% |

"Program"/"data" memory and the percentages are the linker's own labels and
denominators (from the `.gld` linker script's memory regions), not computed here. The
simulator variant is noticeably smaller because it links `sim_dma.c` instead of
`dma.c` and carries no DMA-channel-0 register setup; hardware and nano are close, the
small difference being the different pin/BOARD constants (`board.h`) and the
`33AK512MPS506` vs. `33AK512MPS512` device header.

## Boot banner / `help` reply

No log under `docs/logs/` contains a `help` reply. Two logs contain the boot banner:
`docs/logs/run15-test-sweep.txt` and `docs/logs/run16-test-sweep.txt`. Both are older
board runs (24.09.2026, git `7a9331a+local changes` and `e52701e+local changes`
respectively) and predate this baseline's revision, so the banner text below is
representative of the format, not a byte-exact match to `b41af3b`. Copied from
`docs/logs/run16-test-sweep.txt` (the later of the two):

```
[boot] uart up on FRC, 115200 8N1

##############################################################
##   ADC/DMA TEST LOG  -  START OF RUN  (copy from here)    ##
##############################################################

[boot] adc_dma_40msps Sep 24 2026 20:01:35 git e52701e+local changes (master)
[boot] RCON: 0x00000080
[boot] reset cause: EXTR

adc_dma_40msps - ADC at 40 MSPS into RAM via DMA
board: EV74H48A, dsPIC33AK512MPS512 GP DIM
build: adc_dma_40msps Sep 24 2026 20:01:35 git e52701e+local changes (master)
type 'help' for the commands
please log this terminal from power-up and send it back
> [boot] pll1 postdiv1: 7
[boot] pll1 postdiv2: 7
[boot] adc clock Hz: 32653061
[boot] sample rate ksps (back-to-back): 4081

[boot] READY - nothing is converting, the console has the CPU
[boot] type 'help' for all commands, 'test' for the parts of a run,
[boot] 'test all' for the whole thing. Please log this terminal from
[boot] power-up and send it back.
```

The `help` reply itself is not in either log (both move straight to `test sweep`) and
will be captured by the first `[SMOKE]` run (P0.7), per the plan.

The simulator was **not** run for this baseline (`tools\sim_trap.py` requires explicit
user go-ahead, per `CLAUDE.md`/the task instructions); nothing above comes from running
it, only from the existing board logs and from the `build.bat sim` compile.

# P0.6 Disassembly comparison (`tools/fncmp.py`)

Git revision: `49dae4d` (firmware unchanged since `b41af3b`). Date: 27.09.2026.

`python tools\fncmp.py A.elf B.elf [--ignore-strings]` compares two ELFs function by
function (rules in the script's header); `--count NAME` / `--indirect NAME` answer the
P8.1 questions for one function. `--ignore-strings` is needed whenever the two ELFs
were built at different times: `BUILD_ID` carries `__DATE__`/`__TIME__`, and the four
functions that embed that line (`_main`, `_cli_init`, `_diag_report_build`,
`chaintest.c:_stage0`) differ between any two builds otherwise.

## `_DMA0Interrupt` (P8.1 compares against this)

```
python tools\fncmp.py build\adc_dma_40msps.elf build\adc_dma_40msps_nano.elf --count _DMA0Interrupt --indirect _DMA0Interrupt
adc_dma_40msps.elf: __DMA0Interrupt: 42 instructions (0 of them neop), 94 bytes
adc_dma_40msps.elf: __DMA0Interrupt: 0 indirect call(s), 0 computed jump(s)
adc_dma_40msps_nano.elf: __DMA0Interrupt: 42 instructions (0 of them neop), 94 bytes
adc_dma_40msps_nano.elf: __DMA0Interrupt: 0 indirect call(s), 0 computed jump(s)
```

The 42 instructions are: 19 saves (`push 0x8`, `push.l fsr`, `push.l fcr`, w0..w7 via
`mov.l wN, [w15++]`, `push.l f0`..`f7`), `bclr.b 0x99, #0x5` (the flag),
`mov.l 0x002318, w0` (DMA0STAT), `rcall <_dma0_event>`, the 19 restores and `retfie`. The simulator build has no `_DMA0Interrupt` (`sim_dma.c`
links instead of `dma.c`), so the count is for the hardware and nano builds only.

An indirect call in this ISA is `call wN` (the command dispatcher `_cmd_parser_write`
has two: `call w2`, `call w0`); a computed jump is `bra wN` (the compiler's switch
tables). `--indirect` exits 1 when the function has an indirect call.

## Verification of the tool (27.09.2026, hardware build)

| Check | Result |
|---|---|
| ELF against itself (hw, sim, nano) | 375/332/375 functions, 0 differ, exit 0 |
| one constant changed (`CCP1CON1bits.MOD` 1 -> 2 in `sccp.c`, reverted) | exactly 1 function: `_sccp1_start`, the diff shows `bfins.l #0, #4, #0x1` -> `#0x2` with the SFR address `0x1b00` left as it is |
| an unused global function + variable inserted at the top of `clock.c` (every later function and variable moved; reverted) | `--ignore-strings`: 0 differ, only `_fncmp_dummy` reported as "only in B" |
| the sixteen sources linked in reverse order (everything moved, code and data) | `--ignore-strings`: 375 same, 0 differ, exit 0 |
| two builds of the same source ten minutes apart | `--ignore-strings`: 0 differ; without it the four `BUILD_ID` functions |
| hw against nano | 12 functions differ (`_led_*`, `_console_*`, `_main`, `cli.c:_cmd_core_fn`, `chaintest.c:_restore`, `_diag_report_build`): the board profile |

A full comparison of two hardware ELFs takes about 3 s (three toolchain calls per ELF:
`objdump -d`, `objdump -s`, `readelf -s`; `xc-dsc-objdump` needs `-mdfp=` exactly like
`bin2hex`, otherwise "can't disassemble for architecture UNKNOWN"). `hosttest.bat`
1/1 and `trace.bat` 13/13 still pass; no firmware source changed.
