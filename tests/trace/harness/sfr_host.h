/*
 * sfr_host.h - P0.4 register-trace harness: what the generated xc.h's
 * declarations need on the host, and the XC-DSC-only compiler features
 * the drivers use, stubbed so the same .c files compile with MinGW gcc.
 *
 * Decision of 26.09.2026 (tests/trace/README.md): approach (a), snapshot
 * diff in plain C. Every SFR is one element of sfr_mem[], one contiguous
 * array; `tools/gen_fake_sfr.py --style symbols` (the default) declares
 * each SFR as `extern volatile T X;`, placed there by the generated
 * sfr_syms.ld, so `&X` stays a link-time constant (adc.c's static tables
 * of register addresses compile) and no SFR name becomes a macro (36 of
 * them, e.g. PC, SPLIM, also name a bit field). SFR_REG()/SFR_BITS()
 * below exist only for `--style macros` (kept in the generator to show
 * why it was not chosen - see tests/trace/README.md); the harness itself
 * never uses that style.
 */
#ifndef SFR_HOST_H
#define SFR_HOST_H

#include <stdint.h>

/* One contiguous array. SFR_COUNT (from the generated xc.h) is checked
 * against this in recorder.c; raise it if a bigger device ever needs it. */
#define SFR_MEM_WORDS 4096u
extern volatile uint32_t sfr_mem[SFR_MEM_WORDS];

#define SFR_REG(i)      (sfr_mem[i])
#define SFR_BITS(i, T)  (*(volatile T *)&sfr_mem[i])

/* XC-DSC builtins used by xc.h / the device header / the drivers. */
#define __builtin_nop()      ((void)0)
#define __builtin_clrwdt()   ((void)0)
#define Nop()                ((void)0)
#define ClrWdt()             ((void)0)

/* __attribute__((interrupt, no_auto_psv)): on x86 gcc `interrupt` is a
 * real attribute with a different signature and fails to compile;
 * `no_auto_psv` is unknown (-Wattributes). Both become `unused`. Neither
 * word occurs as an identifier in the drivers (checked with grep). */
#define interrupt    __unused__
#define no_auto_psv  __unused__
#define persistent   __unused__

/* cli.c's `__asm__ volatile ("reset")` is a dsPIC instruction the host
 * assembler does not know (cli.c is not linked into any scenario per the
 * 26.09.2026 decision, but the macro costs nothing to keep for the day a
 * scenario does need it). An assembler macro of that name makes it
 * assemble to nothing. */
__asm__(".macro reset\n.endm\n");

#endif /* SFR_HOST_H */
