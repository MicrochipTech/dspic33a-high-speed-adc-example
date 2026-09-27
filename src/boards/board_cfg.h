/*
 * board_cfg.h - board configuration as data (P7.1, docs/IMPLEMENTATION-PLAN.md;
 * V8 of docs/REFACTORING-PROPOSAL.md).
 *
 * board_cfg holds the board.h values that turned out to be genuinely safe
 * as run-time data - checked, not assumed, one at a time, and most of
 * board.h did NOT qualify. One instance is linked per build, defined in
 * src/boards/ev74h48a.c or src/boards/ev17p63a.c - see board.h and
 * tools/build.bat for how the build picks one; never both.
 *
 * What IS here: the boot sample rate as PLL1's two output dividers
 * (board.h's old ADC_PLL_POSTDIV1/ADC_PLL_POSTDIV2). capture_set_pll()
 * already takes both as plain run-time parameters at its four call sites
 * (main.c's boot, cli.c's "sweep"/"matrix" - back to the boot setting -,
 * chaintest.c's restore()); board.h's macros were only ever the DEFAULT
 * handed to it. None of those four call sites is a file-scope static
 * initializer or a #if, and none of them is linked into any P0.5 golden
 * trace scenario except stream_on/stream_on_input (through chaintest.c's
 * restore()) - tools\trace.bat 13/13 PASS with board_cfg linked there.
 *
 * What is deliberately NOT here, and stays a compile-time macro in
 * board.h instead - each one tried or checked, not assumed:
 *
 *   ADC_INSTANCE, ADC_PINSEL
 *     Tried as board_cfg data first (adc_select()/adc_init() already take
 *     both as plain parameters, so this looked like the same case as the
 *     PLL dividers) and reverted: several golden-trace scenarios
 *     (variants.c, b2b.c, clk.c, regs.c) never call adc_select() at all
 *     and rely on adc_cur's file-scope default already being this board's
 *     core - variants.c's own comment: "AD3CON.ADRDY: the board's default
 *     core (ADC_INSTANCE 3) - this scenario never switches core." Turning
 *     that default into a placeholder broke all four (tools\trace.bat
 *     7/13 FAIL, register names and values changed in the recorded
 *     trace); fixing it properly means adding an explicit adc_select() to
 *     every one of those scenario files, a change to the P0.5 test
 *     harness itself, not something P7.1 needs. Also: adc.c's
 *     `adc_cur = &adc_cores[ADC_INSTANCE - 1]` is a file-scope static
 *     initializer, and adc.h's compile-time bound check
 *     ("#if (ADC_INSTANCE < 1) || (ADC_INSTANCE > 5)") is exactly the
 *     kind of #if this header's own intro says to keep as a macro. Kept
 *     compile-time.
 *
 *   LED_TRIS / LED_LAT / LED_ACTIVE_LOW
 *     led_toggle() runs from capture_service(), called for every completed
 *     buffer half - the hot path CLAUDE.md itself names. A throwaway
 *     experiment for this task (two board.h-only variants of led.c, built
 *     with tools\build.bat and compared with tools\fncmp.py, not kept)
 *     turned LED_LAT/LED_TRIS/LED_ACTIVE_LOW into a pointer+mask+bool read
 *     from another translation unit (the shape board_cfg fields have) and
 *     found: led_toggle() 7 -> 6 instructions but two extra memory loads
 *     (the pointer, the mask) where today there are none; led_init()/
 *     led_on()/led_off() 3 -> 24 instructions each, because
 *     LED_ACTIVE_LOW's compile-time #if becomes a run-time branch. Kept
 *     compile-time.
 *
 *   CONSOLE_TX_TRIS / CONSOLE_RX_TRIS / CONSOLE_RX_RPINR / CONSOLE_RX_RP /
 *   CONSOLE_TX_RPOR / CONSOLE_TX_FN (and the four _WORD register-dump
 *   variants)
 *     The PPS remap registers pack a 7-bit field plus one reserved bit per
 *     pin at a board-specific byte lane inside a 32-bit word (RPOR28 bits
 *     23:16 on the EV74H48A, RPOR10 bits 31:24 on the Nano - board.h's own
 *     comments). That is safe today only because the compiler builds the
 *     mask and shift from the device header's bitfield layout
 *     (RPOR28bits.RP114R); a hand-written pointer+mask+shift in a board
 *     file would have to get the same width and lane right without that
 *     check. No golden trace links cli.c or uart.c at all (tests/trace/
 *     README.md, decision 1: cli.c is stubbed out of every scenario), so a
 *     mistake here would not be caught by tools\trace.bat, tools\hosttest.bat
 *     or [SMOKE]/[SIM] - only by a board run, on the one path
 *     (console_force_up(), after a trap) whose entire job is being the
 *     last, most-trusted way to get one message out. Kept compile-time.
 *
 *   ADC_SAMC, ADC_CLKDIV
 *     Already board-independent (one value for both boards in board.h's
 *     own layout, outside the per-board #if). ADC_SAMC seeds capture.c's
 *     samc_next, ADC_CLKDIV its clkdiv_cur - both file-scope static
 *     initializers, which C requires a compile-time constant expression
 *     for. Kept compile-time.
 *
 *   DAC_ADC_CORE, DAC_ADC_PINSEL, DAC_UREF_PINSEL
 *     Board-independent (both devices share the same DAC/ADC core
 *     numbering - CLAUDE.md: "every register and vector core 5 uses is
 *     identical on the MPS506"). chaintest.c seeds s_core/s_pinsel from
 *     CHAIN_CORE/CHAIN_PINSEL (== these) in a file-scope static
 *     initializer, the same constant-expression constraint as ADC_SAMC
 *     above. Kept compile-time.
 *
 *   BOARD_NAME, BOARD_INPUT_NAME, CONSOLE_PORT_NAME
 *     Plain strings, concatenated into one literal at compile time
 *     ("input: " BOARD_INPUT_NAME, diag.c/cli.c). No register, no hot
 *     path; moving them would only turn one console_puts() into several
 *     for no behaviour change. Kept compile-time.
 *
 *   BOOT_VERBOSE
 *     Gates "#if BOOT_VERBOSE" in cli.c - a compile-time choice by
 *     definition. Kept compile-time.
 */
#ifndef BOARD_CFG_H
#define BOARD_CFG_H

#include <stdint.h>

typedef struct {
    /* Sample rate at boot, as PLL1's two output dividers - board.h's old
     * ADC_PLL_POSTDIV1/ADC_PLL_POSTDIV2. See src/boards/ev74h48a.c for the
     * full reasoning (unchanged, moved verbatim). Both boards use the same
     * value today (board-independent), but capture_set_pll() takes them
     * per board file rather than reintroducing a shared macro. */
    uint8_t adc_pll_postdiv1;
    uint8_t adc_pll_postdiv2;
} board_cfg_t;

/* Exactly one of src/boards/ev74h48a.c, src/boards/ev17p63a.c is linked
 * per build - tools/build.bat picks the source file the same way it
 * already picks dma.c vs sim/sim_dma.c; the MPLAB X project excludes the
 * other one per configuration (nbproject/configurations.xml). Never both,
 * so this is a plain extern, not a weak/strong symbol pair. */
extern const board_cfg_t board_cfg;

#endif /* BOARD_CFG_H */
