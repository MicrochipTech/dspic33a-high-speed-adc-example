/*
 * regs.h - the port layer's register visitor (P4.8, 27.09.2026)
 *
 * A driver under src/drivers/ no longer prints its register dump. It
 * walks its registers and hands each one to a visitor the caller passes
 * in - `xxx_regs_visit(visit)` in every driver header - and what becomes
 * of them is the caller's business: here diag.c's reg_print() prints
 * exactly the lines the drivers' former xxx_regs_dump() printed (the
 * "regs" golden trace is the proof), and a foreign project can log them,
 * ship them as binary, or ignore the titles.
 *
 * Why three kinds of line and not the plan's plain (name, value): the old
 * dumps consisted of exactly three things - a title line the driver wrote
 * verbatim ("[regs] adc\r\n", or sim_dma.c's "(simulator stand-in, no
 * registers)"), a value in hex ("ADxCON: 0x00000000") and a value in
 * decimal ("core: 5", "module clock Hz: 160000000"). Two of them differ
 * only in the number's format, so a single callback with a format tag
 * reproduces all three character for character, at the cost of one
 * argument; a second callback for the title would mean a struct of two
 * pointers passed everywhere for one line per driver. The title text
 * stays with the driver because it depends on the implementation (dma.c
 * and sim_dma.c implement the same dma.h with different titles), and it
 * is passed as the whole line, "\r\n" included, so that it reaches the
 * console in one call like port_log() - the trace harness records one
 * `C` line per call.
 */
#ifndef PORT_REGS_H
#define PORT_REGS_H

#include <stdint.h>

typedef enum {
    REG_HEX,     /* "name: 0x........\r\n" - a register word            */
    REG_DEC,     /* "name: 123\r\n"        - a count, a code, a frequency */
    REG_TITLE    /* name is the complete title line, v is 0             */
} reg_fmt_t;

typedef void (*reg_visit_t)(const char *name, uint32_t v, reg_fmt_t fmt);

#endif /* PORT_REGS_H */
