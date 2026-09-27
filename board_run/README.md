# board_run/ - firmware images for the colleague's board run

This folder holds the firmware the colleague programs by hand for a board run
(board-run-task.md, `docs/IMPLEMENTATION-PLAN.md` section BR). **These hex files
are committed, prebuilt firmware images - they are not build output of the
current working tree.** Each one is built once, by the lead agent, from a
clean `git worktree` checkout of the revision named in its file name, and
committed together with that revision. Do not rebuild them yourself and do
not expect a hex file here to match a local build of the same source tree
(`build/` is where a local build lands, and it is git-ignored).

## What you do (the whole procedure)

```
git pull
tools\gui_setup.bat          once, and again after a pull that changed a requirements file
tools\board_run.bat COM5     COM5 = the board's console port; tools\board_run.bat --list shows the candidates
```

`board_run.bat` (`tools/board_run.py`) tells you everything else: it finds the
hex file(s) in this folder itself, checks each one against `SHA256SUMS.txt`
below, prints the hardware set-up checklist below and waits for you to
confirm it, then tells you which file to program and when to press ENTER. It
never programs the board itself - MPLAB X / IPE, or the programming tool of
your choice, does that (BR decision 4: one dependency fewer here, and the
SHA-256 check catches a wrong file regardless of how it got onto the board).

**What to send back:** the one file `board_run.bat` names at the end,
`run-<date>-<time>-<board>.zip`. It lands in `tools\` (next to
`board_run.bat`, unless you passed `--out-dir`). It contains the full session
log, a `summary.txt` (the same text printed on screen), and `session.json`
(which hex file(s), their SHA-256, your git revision and working-tree state,
the PC clock, and your answer about a signal generator on the R5 input).

(For the lead, not the colleague: the same run without anyone at the board is
`tools\board_run.bat --remote`, through the relay - see CLAUDE.md's
`tools/board_run.py` and `tools/remote.py` rows.)

## The firmware images

| File | Revision | What it is |
|---|---|---|
| `A-EV74H48A-b41af3b.hex` | `b41af3b` | "A" in the A/B board run: the parent of P0.1, the state of this repository from a clean checkout before the N+1 restructuring began (BR decision 1 - run 19's actual firmware, `fbfd883` plus local changes committed only as `c3bc644`, cannot be rebuilt, so `b41af3b` stands in for "before"). |
| `B-EV74H48A-dead53c.hex` | `dead53c` | "B" in the A/B board run, added in BR.8: the P12 close-out revision - N+1 restructured, not yet run on silicon (`docs/HARDWARE-LOG.md`'s 2026-09-27 entry). |

**Both A and B are present now** - the runner does the full A/B run.

`SHA256SUMS.txt` (sha256sum format, `sha256sum -c SHA256SUMS.txt` from inside
this folder checks it by hand) pins every file above byte for byte -
`board_run.py` refuses to proceed on a mismatch without your explicit
"continue anyway?".

**Reproducibility, checked before committing `A-EV74H48A-b41af3b.hex`:** built
twice, from two independent clean `git worktree` checkouts of `b41af3b`. The
two results differ in exactly 4 of the hex file's records (of several
thousand), each one carrying a build timestamp string embedded by the C
compiler's `__TIME__` (`CLAUDE.md`'s own smoke-test note: "date, time and
revision differ between any two builds, and even between the three lines of
one build, because `__TIME__` is per translation unit"); the git revision and
"dirty" flag baked into the banner were identical both times. Nothing else
differs - the committed file's SHA-256 above is what actually runs on the
board, whichever of the two timestamp strings a future rebuild would embed.
`B-EV74H48A-dead53c.hex` was checked the same way before committing (BR.8):
two independent clean worktree builds of `dead53c`, 4 differing `__TIME__`
records, git revision and dirty flag identical.

## Hardware set-up (EV74H48A only - BR decision 3; the Nano/EV17P63A gets its own first run later)

<!-- BOARD_RUN_CHECKLIST:BEGIN
The lines below (as a plain markdown list) are parsed verbatim by
tools/board_run.py and printed as a checklist before run A, so this section
is the one place to edit either text - do not duplicate it in board_run.py.
-->
- The dsPIC33AK512MPS512 GP DIM is plugged into the EV74H48A base board.
- One USB cable, from the PC to the board's PKOB4 connector (J24) - this is
  the only cable needed. It both programs the board and carries the console:
  the PKOB4 debugger enumerates for programming, and the on-board MCP2221A's
  own USB-UART channel appears as a second, separate COM port for the
  console (115200 8N1) - both over the one cable (`README.md`, user guide
  DS70005562D 2.1.1). `board_run.bat --list` marks the likely console port.
- The board is powered from that same USB cable; no other power connection
  is used in any run recorded in `docs/HARDWARE-LOG.md`.
- R2-R4 (the chain test's DAC2 triangle and the back-to-back suite) need NO
  external wiring: DAC2 drives RA8 (AD5AN3) and every ADC core reads it back
  through the chip's internal UREF route (`UREFCON.INSEL`) - not through the
  pin. Leave RA8 unconnected: nothing may be attached to it or load it (it
  doubles as the board's capacitive touch pad 2, DIM pin P44 - anything
  touching or wired to it changes what the DAC output looks like to the
  ADC).
- R5 (the non-DAC / custom-input path) reads `tools/boards.py`'s own default
  input for this board: core 3, `pinsel` 5 = AD3AN5 = device pin RA0 = DIM
  pin P77 = the **mikroBUS A socket, pin "AN"** (pin 1 of that socket; GND is
  available on the same socket). If you have a signal generator, connect it
  there now (frequency/amplitude within 0 to 3.3 V, grounded to a mikroBUS
  GND pin) and answer the runner's prompt afterwards. **Without a generator,
  leave it unconnected - R5 still runs and is still useful, but it then
  judges only the data path (CRC, counters, frame shape), never SNR/THD.**
<!-- BOARD_RUN_CHECKLIST:END -->

**Not confirmed on the board (to confirm on the board):**
- Jumper positions: no jumper is documented anywhere in this repository for
  the EV74H48A + MPS512 DIM combination (`README.md`, `docs/HARDWARE-LOG.md`
  describe plugging the DIM in and connecting the one USB cable, nothing
  else); the board user guide DS70005562D may still call for checking a
  default position before power-up. Verify against that guide once, on the
  board, rather than assuming "none needed" from silence in this repository.
- Whether the mikroBUS A socket's GND pin is the best ground reference for a
  signal generator on R5, versus one of the board's separate test points -
  either should work; not tried with an external generator on this board
  yet.
