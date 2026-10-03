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

/* spike_main.c - P0.3 spike scenario: timebase.c and dma.c, compiled
 * unchanged, driven through their entry points, the SFR trace on stdout.
 * Built three times by run.sh, TRACE_MODE 1/2/3 (sfr_host.h). */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "timebase.h"
#include "dma.h"

extern jmp_buf fail_jmp;
void _DMA0Interrupt(void);            /* defined in dma.c as an ISR */

static uint16_t buf[64] __attribute__((aligned(4)));

#if TRACE_MODE != 1
/* Timer1 counts: every read of TMR1 sees one tick more. Without this a
 * `while ((timebase_ticks() - t0) < n)` loop never ends. */
static void tmr1_hook(unsigned idx) { hw_set(idx, hw_get(idx) + 1u); }
#endif

int main(void)
{
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    trace_init();
    printf("# TRACE_MODE %d, layout check: %d fields differ from the pack's masks\n",
           TRACE_MODE, sfr_layout_check());
    trace_region(buf, sizeof buf, "buf");
#if TRACE_MODE != 1
    trace_hook(trace_idx("TMR1"), tmr1_hook);
#endif

    trace_note("# timebase_init()\n");
    timebase_init();
    trace_note("# timebase_init() again - Timer1 running, no writes expected\n");
    timebase_init();
    trace_note("# timebase_check()\n");
    uint32_t t = timebase_check();
    trace_note("# -> %lu ticks\n", (unsigned long)t);
    trace_note("# timebase_ticks() twice\n");
    uint32_t a = timebase_ticks();
    uint32_t b = timebase_ticks();
    trace_note("# -> difference %lu\n", (unsigned long)(b - a));

    trace_note("# dma0_init(0x48, &AD5CH0RES, buf, sizeof buf)\n");
    if (setjmp(fail_jmp) == 0) {
        dma0_init(0x48u, &AD5CH0RES, buf, sizeof buf);
    }
    trace_note("# dma0_clear(DMA0_HALF)\n");
    dma0_clear(DMA0_HALF);
    trace_note("# _DMA0Interrupt() with HALF and DMA0IF set by the 'hardware'\n");
    hw_set(trace_idx("DMA0STAT"), DMA0_HALF);
    hw_set(trace_idx("IFS2"), hw_get(trace_idx("IFS2")) | _IFS2_DMA0IF_MASK);
    _DMA0Interrupt();
    trace_note("# dma0_deinit()\n");
    dma0_deinit();
    trace_note("# dma0_init() with an odd size -> fail(8)\n");
    if (setjmp(fail_jmp) == 0) {
        dma0_init(0x48u, &AD5CH0RES, buf, sizeof buf - 1u);
    }
    trace_flush();
    printf("# end, %lu SFR accesses trapped\n", trace_accesses());
    return 0;
}
