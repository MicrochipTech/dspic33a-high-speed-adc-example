/*
 * dma.c
 *
 * DMA channel 0 of the ADC/DMA example: one channel, peripheral to RAM,
 * Repeated One-Shot mode - one transfer per trigger - with an interrupt at
 * each half of the block.
 * This file knows the DMA registers and nothing else - what triggers the
 * channel, where the data goes and what to do at HALF and DONE is the
 * caller's business (capture.c).
 *
 * Every register write below cites the datasheet table or page it comes
 * from (DS70005591D).
 */

#include <xc.h>
#include "dma.h"
#include "log.h"        /* port layer (src/port): port_log(), port_log_kv(), port_trace*() */
#include "regs.h"       /* port layer (src/port): reg_visit_t for the register dump (P4.8) */
#include "panic.h"      /* port layer: port_panic()                                       */

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
 * TRMODE = 01, REPEATED ONE-SHOT: "single transfers occur repeatedly as
 * long as triggers are being provided ... Each time a trigger occurs ...
 * DMAxCNT is decremented ... the channel is not disabled when DMAxCNT
 * reaches 0000h. Instead, the original value of DMAxCNT is reloaded"
 * (13.4.8.3, p832). One ADC result, one transfer; with RELOADD/RELOADC
 * the channel restarts at the buffer start after each block on its own
 * (p812, p829 step 4). HALFEN and DONEEN give one interrupt per half
 * (13.6.1.2, p848). No address is ever rewritten from software while the
 * channel runs.
 *
 * NOT TRMODE = 11 (Repeated Continuous), which this file used until
 * 25.09.2026: "a single trigger starts a sequence of back-to-back
 * transfers" (Continuous, 13.4.8.4, p833) and "multiple transfers can
 * occur with each trigger" (Repeated Continuous, 13.4.8.5, p834). Every
 * conversion made the DMA copy the result register as fast as it could,
 * about 41 M transfers/s, until the block was full: run 18's S3 counted
 * 6144 transfers for 15 conversions and a buffer of runs of ~400 equal
 * values, and S4 found 41 M transfers/s at every rate from 100 kSPS to
 * 40 MSPS. That one bit is the "40 MSPS whatever the setting", the
 * overrun storms and the "few transfers per trigger" of every earlier
 * run (docs/HARDWARE-LOG.md, 25.09.2026).
 *
 * DMAxSTAT flags are "R/C/HS" - clearable by writing 0 (legend p815,
 * Example 13-4 p835: "DMA0STATbits.DONE=0"). Writing 1 does not clear.
 * ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ *
 * The address window, shared by every channel (SG.1, 29.09.2026)
 *
 * DMALOW/DMAHIGH exist once: "All DMA channels are restricted to the
 * address range set by DMAHIGH and DMALOW" (13.4.5 "Memory Boundary",
 * p826), and the check covers every SRAM access - Figure 13-3 (p828)
 * draws the window around both the source and the destination; only
 * "the memory-mapped SFR range is always accessible by DMA" (same
 * section; DMALOW/DMAHIGH notes p809/810). So with channel 1 reading a
 * generator table from RAM, the window must cover channel 0's buffer
 * AND that table, or channel 1 would stop with ADRERR on its first
 * read. dma.c keeps both regions and writes the smallest window over
 * them; siggen.c places its table in the same ".dma_buffer" section as
 * capture.c's buffer, so the window holds those two objects and nothing
 * else (dma_window_gap() reports what lies between them).
 * What is lost against one channel alone: the hardware fence between
 * the buffer and the table (docs/IMPLEMENTATION-PLAN.md SG decision 1).
 * The buffer's guard words stay.
 * ------------------------------------------------------------------ */
static uint32_t win0_first = 0u, win0_last = 0u;   /* channel 0's buffer */
static uint32_t win1_first = 0u, win1_last = 0u;   /* channel 1's table  */

static void window_write(void)
{
    uint32_t lo = win0_first, hi = win0_last;
    if (win1_last != 0u) {
        if ((win0_last == 0u) || (win1_first < lo)) { lo = win1_first; }
        if ((win0_last == 0u) || (win1_last > hi))  { hi = win1_last; }
    }
    DMALOW  = lo;
    DMAHIGH = hi;
}

uint32_t dma_window_gap(void)
{
    if ((win0_last == 0u) || (win1_last == 0u)) { return 0u; }
    if (win1_first > win0_last) { return win1_first - win0_last - 1u; }
    if (win0_first > win1_last) { return win0_first - win1_last - 1u; }
    return 0u;
}

/* DMACON.ON = 0 "resets all state machines, resulting in immediate
 * termination of all active DMA operation(s)" (p807) - every channel,
 * not one. While channel 1 plays a table, channel 0's set-up and
 * tear-down leave the module on and work through CHEN alone. */
static bool ch1_busy(void)
{
    return DMA1CHbits.CHEN != 0u;
}

void dma0_init(uint32_t trigger, const volatile void *src,
               volatile void *dst, uint32_t dst_bytes)
{
    if (!ch1_busy()) { DMACONbits.ON = 0; }
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
        port_log_kv("[dma] unusable buffer (outside RAM, odd size, misaligned or > 64K transactions), first", first, true);
        port_log_kv("[dma] unusable buffer, last", last, true);
        port_panic(8u);
    }
    win0_first = first;
    win0_last  = last;
    window_write();

    DMA0SEL = trigger;                      /* e.g. ADCn Done CH0       */
    DMA0SRC = (uint32_t)src;                /* peripheral result        */
    DMA0DST = (uint32_t)dst;                /* RAM destination          */
    DMA0CNT = count;                        /* transactions per block   */
    DMA0STAT = 0u;                          /* clear any stale flags    */

    DMA0CH = 0u;
    DMA0CHbits.SIZE    = 1u;          /* 16-bit transfers               */
    DMA0CHbits.SAMODE  = 0u;          /* source address unchanged       */
    DMA0CHbits.DAMODE  = 1u;          /* destination incremented        */
    DMA0CHbits.TRMODE  = 1u;          /* repeated one-shot: 1 per trigger (p832) */
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
    port_trace("[dma] channel 0 armed, IRQ on; address window = the buffer:\r\n");
    port_trace_kv("[dma] DMALOW", DMALOW, true);
    port_trace_kv("[dma] DMAHIGH", DMAHIGH, true);
}

uint32_t dma0_remaining(void)
{
    return DMA0CNT;                   /* transactions left in the block */
}

bool dma0_enabled(void)
{
    return DMA0CHbits.CHEN != 0u;
}

void dma0_deinit(void)
{
    IEC2bits.DMA0IE = 0;
    DMA0CHbits.CHEN = 0;
    DMA0STAT = 0u;                          /* all flags cleared (R/C)  */
    IFS2bits.DMA0IF = 0;
    if (!ch1_busy()) { DMACONbits.ON = 0; }  /* p807: ON = 0 stops channel 1 too */
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

void dma0_regs_visit(reg_visit_t visit)
{
    visit("[regs] dma\r\n", 0u, REG_TITLE);
    visit("DMACON", DMACON, REG_HEX);
    visit("DMALOW", DMALOW, REG_HEX);
    visit("DMAHIGH", DMAHIGH, REG_HEX);
    visit("DMA0CH", DMA0CH, REG_HEX);
    visit("DMA0SEL", DMA0SEL, REG_HEX);
    visit("DMA0STAT", DMA0STAT, REG_HEX);
    visit("DMA0SRC", DMA0SRC, REG_HEX);
    visit("DMA0DST", DMA0DST, REG_HEX);
    visit("DMA0CNT", DMA0CNT, REG_HEX);
    visit("IEC2", IEC2, REG_HEX);           /* DMA0 enable,  bit 13      */
    visit("IFS2", IFS2, REG_HEX);           /* DMA0 flag,    bit 13      */
    visit("IPC9", IPC9, REG_HEX);           /* DMA0 priority             */
}

/* ------------------------------------------------------------------ *
 * DMA channel 1: a table in RAM -> one SFR, the signal generator's
 * transport (SG.1, 29.09.2026; docs/IMPLEMENTATION-PLAN.md section SG)
 *
 * One table entry per trigger: TRMODE = 01, Repeated One-Shot, as for
 * channel 0 and for the same reason (13.4.8.3, p832) - NOT the "Repeated
 * Continuous" DESIGN-MULTICHANNEL 4.2 first named, which copies a whole
 * block per trigger (runs 1-18). Source incremented (SAMODE = 01),
 * destination fixed (DAMODE = 00), and at the end of the block the
 * source address and the count reload (RELOADS/RELOADC, DMAxCH bits
 * 24/26, p811-812; CNT reloads in every repeated mode anyway, note 3), so
 * the table plays cyclically with no CPU involvement at all.
 * No interrupt (HALFEN = DONEEN = 0, SG decision 3): errors are read back
 * through dma1_tx_status(), _DMA0Interrupt stays as it is.
 * SIZE comes from the caller: 01 = one 16-bit word per transfer, 10 = 32
 * bits (DMAxCH SIZE[1:0], p812). A 16-bit transfer needs a 16-bit
 * aligned address on both sides (13.4.2, p825: "bit 0 is always 0"); the
 * SFR window rule above lets the destination be any SFR.
 * ------------------------------------------------------------------ */
bool dma1_tx_start(uint32_t trigger, const volatile void *src, uint32_t n,
                   volatile void *dst_sfr, uint32_t size)
{
    const uint32_t bytes = (size == DMA_SIZE_32) ? 4u : 2u;
    const uint32_t first = (uint32_t)src;
    const uint32_t last  = first + n * bytes - 1u;
    if ((n < 2u) || ((size != DMA_SIZE_16) && (size != DMA_SIZE_32)) ||
        (first < RAM_FIRST) || (last > RAM_LAST) || (last < first) ||
        (first % bytes != 0u) || ((uint32_t)dst_sfr % bytes != 0u)) {
        return false;
    }
    DMA1CHbits.CHEN = 0;
    win1_first = first;
    win1_last  = last;
    window_write();

    DMA1SEL  = trigger;                     /* e.g. SCCP2, 0x19 (Table 13-2 p797) */
    DMA1SRC  = first;                       /* the table                */
    DMA1DST  = (uint32_t)dst_sfr;           /* e.g. &DAC2DAT + 2        */
    DMA1CNT  = n;                           /* transfers per block      */
    DMA1STAT = 0u;                          /* stale flags cleared (R/C)*/

    DMA1CH = 0u;
    DMA1CHbits.SIZE    = size;
    DMA1CHbits.SAMODE  = 1u;          /* source incremented             */
    DMA1CHbits.DAMODE  = 0u;          /* destination unchanged          */
    DMA1CHbits.TRMODE  = 1u;          /* repeated one-shot: 1 per trigger (p832) */
    DMA1CHbits.RELOADS = 1u;          /* table start again each block   */
    DMA1CHbits.RELOADC = 1u;          /* count again each block         */
    DMA1CHbits.RETEN   = 0u;          /* as channel 0 (capture.c note 1) */

    DMACONbits.PRIORITY = 1u;         /* round robin, as dma0_init()    */
    DMACONbits.ON   = 1;
    DMA1CHbits.CHEN = 1;
    return DMA1CHbits.CHEN != 0u;
}

void dma1_tx_stop(void)
{
    DMA1CHbits.CHEN = 0;
    DMA1STAT = 0u;
    win1_first = 0u;
    win1_last  = 0u;
    if (win0_last != 0u) { window_write(); }
}

uint32_t dma1_tx_status(void)    { return DMA1STAT; }
uint32_t dma1_tx_remaining(void) { return DMA1CNT; }
bool     dma1_tx_enabled(void)   { return DMA1CHbits.CHEN != 0u; }

void dma1_regs_visit(reg_visit_t visit)
{
    visit("[regs] dma1\r\n", 0u, REG_TITLE);
    visit("DMACON", DMACON, REG_HEX);
    visit("DMALOW", DMALOW, REG_HEX);
    visit("DMAHIGH", DMAHIGH, REG_HEX);
    visit("DMA1CH", DMA1CH, REG_HEX);
    visit("DMA1SEL", DMA1SEL, REG_HEX);
    visit("DMA1STAT", DMA1STAT, REG_HEX);
    visit("DMA1SRC", DMA1SRC, REG_HEX);
    visit("DMA1DST", DMA1DST, REG_HEX);
    visit("DMA1CNT", DMA1CNT, REG_HEX);
}
