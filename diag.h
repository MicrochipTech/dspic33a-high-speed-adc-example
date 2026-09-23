/*
 * diag.h - stop codes, trap handler, boot record, register dump (diag.c)
 */
#ifndef DIAG_H
#define DIAG_H

#include <stdint.h>

/* Bound for every hardware wait loop, in loop iterations. A step that
 * needs longer than this has failed; fail() then reports which one. */
#ifdef __MPLAB_DEBUGGER_SIMULATOR
#define WAIT_LIMIT        20000u     /* simulator runs at ~1/80 real time */
#else
#define WAIT_LIMIT        2000000u
#endif

/* != 0: stopped, see fail() */
extern volatile uint32_t fail_code;

/* Start-up progress and the last trap, in persistent RAM so both survive
 * the reset that an unhandled trap would otherwise hide. boot_mark() is
 * called at each step of main(); _DefaultInterrupt() prints the lot and
 * blinks code 9. trap_seen != 0 at start-up means the previous run hit a
 * trap - main() reports that before doing anything else. */
extern volatile uint32_t boot_stage;
extern volatile uint32_t trap_seen;
extern volatile uint32_t trap_vec;
extern volatile uint32_t trap_stage;
void boot_mark(uint32_t stage);

/* Stop with a blink code (never returns). Codes: table in diag.c. */
void fail(uint32_t code);

/* Print RCON, the reset-cause register, decoded, then clear it so the
 * next boot shows its own cause. Called once, right after the console
 * is up. A board that "just restarts" is told apart here: POR/BOR
 * (supply), WDTO, SWR (the reset command), EXTR (MCLR), CM (config
 * mismatch), BUCKR/VREGxR (the internal regulators gave up). */
void diag_report_reset(void);


/* Wait until a condition becomes false, or give up with a code.
 *
 * Simulator build: no waiting at all. The MPLAB X simulator has no PLL,
 * no ADC conversion and no DMA transfer, so every one of these conditions
 * would time out and end the run in fail(1) before anything of interest
 * had executed. The register writes stay exactly as on hardware; only the
 * waits go - the same pattern MCC's clock.c uses. */
#ifdef __MPLAB_DEBUGGER_SIMULATOR
#define WAIT_WHILE(cond, code)  do { (void)(cond); } while (0)
#else
#define WAIT_WHILE(cond, code)                              \
    do {                                                    \
        uint32_t n_ = WAIT_LIMIT;                           \
        while (cond) {                                      \
            if (--n_ == 0u) { fail(code); }                 \
        }                                                   \
    } while (0)
#endif

/* Clock, ADC, DMA, interrupt and UART registers as "name: 0x........"
 * lines on the console. Printed by fail() and by the "regs" command. */
void regs_dump(void);

#endif /* DIAG_H */
