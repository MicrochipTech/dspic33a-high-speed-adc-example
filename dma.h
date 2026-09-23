/*
 * dma.h - DMA channel 0 of the ADC/DMA example (dma.c)
 */
#ifndef DMA_H
#define DMA_H

#include <stdint.h>
#include <stdbool.h>
#include <xc.h>

/* Status flags, as dma0_status() returns them and dma0_clear() takes
 * them: the DMA0STAT bit masks of the device header. */
#define DMA0_OVERRUN   _DMA0STAT_OVERRUN_MASK   /* triggered while busy   */
#define DMA0_ADRERR    _DMA0STAT_ADRERR_MASK    /* outside DMALOW..HIGH   */
#define DMA0_BRERR     _DMA0STAT_BRERR_MASK     /* bus read error         */
#define DMA0_BWERR     _DMA0STAT_BWERR_MASK     /* bus write error        */
#define DMA0_HALF      _DMA0STAT_HALF_MASK      /* first half complete    */
#define DMA0_DONE      _DMA0STAT_DONE_MASK      /* block complete         */

/* Channel 0: 16-bit transfers from `src` (fixed) into the buffer `dst`
 * of `dst_bytes` bytes (incremented, reloaded after each block), started
 * by `trigger` (a DMA_SEL CHSEL code), HALF/DONE interrupts enabled. The
 * block is the whole buffer (dst_bytes / 2 transactions) and the DMA's
 * address window is exactly the buffer, so the channel cannot write
 * anywhere else. Pass the buffer object and its sizeof, nothing derived.
 * Nothing transfers until the trigger fires. */
void dma0_init(uint32_t trigger, const volatile void *src,
               volatile void *dst, uint32_t dst_bytes);

/* False once the channel switched itself off (address fault). */
bool dma0_enabled(void);
/* Interrupt masked, channel disabled. Nothing restarts after this. */
void dma0_halt(void);
/* Clear the given status flags (DMA0_* above). */
void dma0_clear(uint32_t flags);

/* Called from the DMA0 interrupt with a snapshot of DMA0STAT. Implemented
 * by the owner of the channel (capture.c), which clears the flags it has
 * acted on with dma0_clear(). */
void dma0_event(uint32_t status);

/* The channel's registers as "name: 0x........" lines (part of regs_dump()). */
void dma0_regs_dump(void);

#endif /* DMA_H */
