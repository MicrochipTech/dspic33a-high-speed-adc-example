/*
 * dma_tx.h - DMA channel 1 as a transmit channel, the signal generator's
 * transport (dma.c, SG.1, 29.09.2026)
 *
 * A header of its own, without <xc.h>, so that src/siggen/siggen.c - which
 * knows no register - and its host test (tests/host/test_siggen.c) build
 * with a host gcc; dma.h includes it for everything else.
 */
#ifndef DMA_TX_H
#define DMA_TX_H

#include <stdbool.h>
#include <stdint.h>
#include "regs.h"       /* reg_visit_t (src/port, P4.8) */

/* DMAxCH SIZE codes (p812). */
#define DMA_SIZE_16   1u
#define DMA_SIZE_32   2u
/* DMA_SEL CHSEL codes channel 1 can be triggered by (Table 13-2 p797,
 * ATDF value-group DMA_SEL__CHSEL). SCCP2 is its IC/OC event (CCP2IF). */
#define DMA_TRIG_TMR1    0x05u
#define DMA_TRIG_TMR2    0x0Eu
#define DMA_TRIG_SCCP2   0x19u

/* Channel 1 plays `n` entries of `size` (DMA_SIZE_16/32) from the RAM
 * table `src` into the fixed SFR `dst_sfr`, one per `trigger`, cyclically
 * (Repeated One-Shot, source and count reloaded at the end of the table),
 * no interrupt. Widens the shared DMALOW/DMAHIGH window to cover the
 * table as well as channel 0's buffer. False (nothing changed) for
 * n < 2, a bad size, a table outside RAM or a misaligned address. */
bool     dma1_tx_start(uint32_t trigger, const volatile void *src, uint32_t n,
                       volatile void *dst_sfr, uint32_t size);
/* Channel 1 disabled, its flags cleared, the window back to channel 0's
 * buffer alone. */
void     dma1_tx_stop(void);
uint32_t dma1_tx_status(void);       /* DMA1STAT (dma.h's DMA0_* masks apply) */
uint32_t dma1_tx_remaining(void);    /* DMA1CNT: transfers left in block */
bool     dma1_tx_enabled(void);      /* false after an address/bus fault */
void     dma1_regs_visit(reg_visit_t visit);

/* Bytes between channel 0's buffer and channel 1's table inside the
 * shared window - 0 when they touch or when only one is set. Anything
 * else lying there is inside the window too (SG decision 1). */
uint32_t dma_window_gap(void);

#endif /* DMA_TX_H */
