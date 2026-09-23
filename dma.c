/*
 * dma.c
 *
 * DMA channel 0 of the ADC/DMA example: one channel, peripheral to RAM,
 * Repeated Continuous mode with an interrupt at each half of the block.
 * This file knows the DMA registers and nothing else - what triggers the
 * channel, where the data goes and what to do at HALF and DONE is the
 * caller's business (capture.c).
 *
 * Every register write below cites the datasheet table or page it comes
 * from (DS70005591D).
 */

#include <xc.h>
#include "dma.h"
#include "console.h"
#include "diag.h"

/* The device's data RAM, from the device header (0x4000 and 0x10000 for
 * the 64 KB parts, matching p33AK512MPS512.gld). Only a sanity check
 * now: the address window the channel gets is the buffer itself, and
 * that buffer must lie inside RAM. */
#if !defined(__DATA_BASE) || !defined(__DATA_LENGTH)
#error "__DATA_BASE / __DATA_LENGTH not provided by the device header"
#endif
#define RAM_FIRST   ((uint32_t)__DATA_BASE)
#define RAM_LAST    ((uint32_t)__DATA_BASE + (uint32_t)__DATA_LENGTH - 1u)

/* ------------------------------------------------------------------ *
 * DMA channel 0: ADCn channel 0 result -> RAM
 *
 * CHSEL is the trigger the caller passes in - "ADCn Done CH0" for the
 * ADC (ATDF value-group DMA_SEL__CHSEL, table in adc.h). SIZE = 1
 * selects 16-bit transfers; the DMA
 * supports 8, 16 and 32 bit (DS70005591D 13.4.2, p824 and p812), so a
 * 12-bit result costs 2 bytes. ADxCH0RES holds RES[11:0] in the low half
 * and RESF[11:0] in bits 31:20 (p1229), so the 16-bit read of the low
 * half is the sample.
 *
 * DMALOW / DMAHIGH MUST be set. They reset to 0, every transaction is
 * checked against them (13.4.8.1 p829, step 5), and an access above
 * DMAHIGH sets ADRERR = 10 and clears CHEN (p810, p826). With the reset
 * values the very first sample would disable the channel. Every
 * datasheet example (p832 ff.) and MCC set them - to the whole RAM.
 *
 * Here the window is the destination buffer itself, dst..dst+2*count-1,
 * nothing else: the hardware then refuses any transaction that would
 * leave the buffer, and the channel switches itself off instead of
 * writing into whatever follows (fail 8, dma_addr_err). The source is
 * outside that window on purpose - it is an SFR, far below RAM - and
 * that is fine: the board ran with the source below DMALOW from the
 * first day (window 0x4000..0x13FFF, source 0xB64), so the check does
 * not apply to the peripheral side.
 *
 * TRMODE = Repeated Continuous with RELOADD/RELOADC restarts at the
 * buffer start after each block on its own (p812, p829 step 4). HALFEN
 * and DONEEN give one interrupt per half (13.6.1.2, p848). No address
 * is ever rewritten from software while the channel runs.
 *
 * DMAxSTAT flags are "R/C/HS" - clearable by writing 0 (legend p815,
 * Example 13-4 p835: "DMA0STATbits.DONE=0"). Writing 1 does not clear.
 * ------------------------------------------------------------------ */
void dma0_init(uint32_t trigger, const volatile void *src,
               volatile void *dst, uint32_t dst_bytes)
{
    DMACONbits.ON = 0;
    DMA0CHbits.CHEN = 0;

    /* Everything about the destination comes from the buffer object the
     * caller passes (its address and its sizeof), nothing from a
     * constant: the window is exactly the buffer, the block count is
     * the buffer in 16-bit transactions. A buffer outside RAM or of odd
     * size is a bug in the caller, not something to run with. */
    const uint32_t count = dst_bytes / 2u;
    const uint32_t first = (uint32_t)dst;
    const uint32_t last  = first + dst_bytes - 1u;
    if ((first < RAM_FIRST) || (last > RAM_LAST) || (last < first) ||
        (dst_bytes % 2u != 0u) || (count > 0xFFFFu) || (first % 4u != 0u)) {
        console_kv_hex("[dma] unusable buffer (outside RAM, odd size, misaligned or > 64K transactions), first", first);
        console_kv_hex("[dma] unusable buffer, last", last);
        fail(8u);
    }
    DMALOW  = first;
    DMAHIGH = last;

    DMA0SEL = trigger;                      /* e.g. ADCn Done CH0       */
    DMA0SRC = (uint32_t)src;                /* peripheral result        */
    DMA0DST = (uint32_t)dst;                /* RAM destination          */
    DMA0CNT = count;                        /* transactions per block   */
    DMA0STAT = 0u;                          /* clear any stale flags    */

    DMA0CH = 0u;
    DMA0CHbits.SIZE    = 1u;          /* 16-bit transfers               */
    DMA0CHbits.SAMODE  = 0u;          /* source address unchanged       */
    DMA0CHbits.DAMODE  = 1u;          /* destination incremented        */
    DMA0CHbits.TRMODE  = 3u;          /* repeated continuous            */
    DMA0CHbits.RELOADD = 1u;          /* reload destination each block  */
    DMA0CHbits.RELOADC = 1u;          /* reload count each block        */
    DMA0CHbits.HALFEN  = 1u;          /* interrupt at half              */
    DMA0CHbits.DONEEN  = 1u;          /* interrupt on block complete    */
    DMA0CHbits.RETEN   = 0u;          /* errata: capture.c note 1       */

    /* Round robin arbitration. With one channel it makes no difference,
     * but it is the setting that matters once several ADC streams share
     * the single DMA data bus (DS70005591D 13.4.4, p825). */
    DMACONbits.PRIORITY = 1u;

    DMACONbits.ON   = 1;
    DMA0CHbits.CHEN = 1;

    /* Block-complete interrupt: IRQ 77, IEC2/IFS2 bit 13, IPC9 default
     * priority 4. Nothing fires until the first burst is started. */
    IFS2bits.DMA0IF = 0;
    IEC2bits.DMA0IE = 1;
    console_trace("[dma] channel 0 armed, IRQ on; address window = the buffer:\r\n");
    console_trace_kv_hex("[dma] DMALOW", DMALOW);
    console_trace_kv_hex("[dma] DMAHIGH", DMAHIGH);
}

bool dma0_enabled(void)
{
    return DMA0CHbits.CHEN != 0u;
}

/* Interrupt masked, channel disabled. Nothing restarts after this. */
void dma0_halt(void)
{
    IEC2bits.DMA0IE = 0;
    DMA0CHbits.CHEN = 0;
}

/* DMAxSTAT flags are "R/C/HS": a flag is cleared by writing 0 to it
 * (legend p815, Example 13-4 p835: "DMA0STATbits.DONE=0"); writing 1
 * does nothing. So the whole word is written at once, with 0 only in
 * the flags to clear and 1 everywhere else.
 *
 * NOT a bit-field write. "DMA0STATbits.DONE = 0" compiles to
 * read-modify-write: it reads the word, clears the bit, writes the word
 * back - and a flag that the hardware set between that read and that
 * write is written back as 0, i.e. cleared without ever being seen. On
 * the board this lost the DONE of a burst: the ISR was busy with
 * overrun events, DONE arrived during the write-back, the burst was
 * never restarted and the stream stopped (fail 6, DMA0STAT still
 * showing DONE). The datasheet's own example uses the bit-field form,
 * which is fine as long as only one flag can change at a time; here
 * OVERRUN, HALF and DONE arrive independently. */
void dma0_clear(uint32_t flags)
{
    DMA0STAT = ~flags;
}

/* ------------------------------------------------------------------ *
 * The interrupt: one snapshot of the status word, handed to the owner
 * of the channel (dma0_event() in capture.c). Which flags are set, what
 * they mean and what to do about them is decided there; the snapshot
 * is taken once so that a flag arriving during the handler is seen by
 * the next interrupt, not half by this one.
 *
 * The interrupt flag is cleared FIRST, before the snapshot. An event
 * that arrives while the handler runs then sets it again and the
 * handler re-enters right after returning. Cleared at the end, as the
 * first version did, that event's interrupt is wiped: its flag stays
 * set in DMA0STAT but nothing comes to read it. With the overrun
 * events of a 40 MSPS stream keeping the handler busy, that is exactly
 * what happened on the board - a DONE was lost, the burst was never
 * restarted, the stream stopped (fail 6 with DONE still set in the
 * register dump).
 * ------------------------------------------------------------------ */
void __attribute__((interrupt, no_auto_psv)) _DMA0Interrupt(void)
{
    IFS2bits.DMA0IF = 0;              /* first, see above               */
    dma0_event(DMA0STAT);
}

void dma0_regs_dump(void)
{
    console_puts("[regs] dma\r\n");
    console_kv_hex("DMACON", DMACON);
    console_kv_hex("DMALOW", DMALOW);
    console_kv_hex("DMAHIGH", DMAHIGH);
    console_kv_hex("DMA0CH", DMA0CH);
    console_kv_hex("DMA0SEL", DMA0SEL);
    console_kv_hex("DMA0STAT", DMA0STAT);
    console_kv_hex("DMA0SRC", DMA0SRC);
    console_kv_hex("DMA0DST", DMA0DST);
    console_kv_hex("DMA0CNT", DMA0CNT);
    console_kv_hex("IEC2", IEC2);           /* DMA0 enable,  bit 13      */
    console_kv_hex("IFS2", IFS2);           /* DMA0 flag,    bit 13      */
    console_kv_hex("IPC9", IPC9);           /* DMA0 priority             */
}
