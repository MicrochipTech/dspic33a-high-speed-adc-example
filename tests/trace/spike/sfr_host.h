/*******************************************************************************
 * Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries.
 *
 * Subject to your compliance with these terms, you may use Microchip software
 * and any derivatives exclusively with Microchip products. It is your
 * responsibility to comply with third party license terms applicable to your
 * use of third party software (including open source software) that may
 * accompany Microchip software.
 *
 * THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
 * EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
 * WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
 * PARTICULAR PURPOSE.
 *
 * IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
 * INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
 * WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
 * BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
 * FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
 * ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
 * THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
 ******************************************************************************/

/*
 * sfr_host.h - P0.3 spike: what the generated xc.h's SFR_REG()/SFR_BITS()
 * expand to on the host, and the XC-DSC-only compiler features stubbed.
 *
 * TRACE_MODE (set by run.sh):
 *   1  snapshot (approach a): every SFR is an element of one plain array,
 *      sfr_mem[]. Writes are found by diffing it against a shadow copy at
 *      trace points. `&X` is an address constant (static tables work).
 *   2  access function (a variant): every use of X calls sfr_at(idx),
 *      which diffs, runs a read hook and returns the element's address.
 *      `&X` is NOT a constant expression any more.
 *   3  page guard (the hybrid): like 1 in the source - a plain array -
 *      but the array's pages are protected and every access traps into
 *      a vectored exception handler (recorder.c), which logs it and
 *      runs the read hooks. Windows-specific.
 */
#ifndef SFR_HOST_H
#define SFR_HOST_H

#include <stdint.h>

#ifndef TRACE_MODE
#define TRACE_MODE 1
#endif

/* One contiguous array, page aligned and padded to whole pages so mode 3
 * can protect exactly it. SFR_COUNT comes from the generated xc.h; this
 * size is checked against it in recorder.c. */
#define SFR_MEM_WORDS 3072u
extern volatile uint32_t sfr_mem[SFR_MEM_WORDS];

#if TRACE_MODE == 2
volatile void *sfr_at(unsigned idx);
#define SFR_REG(i)      (*(volatile uint32_t *)sfr_at(i))
#define SFR_BITS(i, T)  (*(volatile T *)sfr_at(i))
#else
#define SFR_REG(i)      (sfr_mem[i])
#define SFR_BITS(i, T)  (*(volatile T *)&sfr_mem[i])
#endif

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
#define persistent  __unused__

/* cli.c's `__asm__ volatile ("reset")` is a dsPIC instruction the host
 * assembler does not know. An assembler macro of that name makes it
 * assemble to nothing (every translation unit that includes xc.h). */
__asm__(".macro reset\n.endm\n");

#endif /* SFR_HOST_H */
