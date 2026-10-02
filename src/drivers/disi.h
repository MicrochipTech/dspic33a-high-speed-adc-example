/*
 * disi.h - write the interrupt-disable threshold (DISICTL, DS70005591D
 * 10.10.1.1, p596: interrupt requests at an IPL at or below DISIIPL are held
 * off). Read it back through the DISIIPL bit field in <xc.h>.
 *
 * Why not __builtin_write_DISICTL(): with a variable argument it stops
 * xc-dsc v3.21 and v3.31 at -O0 with "internal compiler error: in
 * emit_move_insn, at expr.c:3810" (02.10.2026, the MPLAB X project builds at
 * -O0; tools\build.bat at -O1 never saw it). This is the one instruction the
 * builtin produces at -O1 ("disictl wN"), checked to be the same code there.
 */
#ifndef DISI_H
#define DISI_H

#include <stdint.h>

static inline void disi_set(uint32_t ipl)
{
    __asm__ volatile ("disictl %0" : : "r"(ipl) : "memory");
}

#endif /* DISI_H */
