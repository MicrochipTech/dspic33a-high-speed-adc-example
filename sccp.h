/*
 * sccp.h - SCCP1 as the ADC's sample-rate generator (sccp.c)
 *
 * The rate control this example was missing. SCCP1 produces one event per
 * period and the ADC converts once per event, so the sample rate is
 *
 *     rate = sccp1_hz() / ticks
 *
 * with `ticks` a 32-bit period register: fine, adjustable at run time by
 * one register write, and without touching the clock tree. At 320 MHz
 * that is 40 MSPS at ticks 8, exactly 8 MSPS at ticks 40, and anything
 * down to a few hertz.
 *
 * THREE THINGS WERE WRONG HERE UNTIL 24.09.2026, and each one alone was
 * enough for the ADC to see nothing at all (docs/HARDWARE-LOG.md runs 5
 * to 7 reported "no conversion" and were never really a test of this
 * path):
 *
 *   1. The ADC trigger code. Datasheet Table 16-4 lists 100010 = 34 as
 *      "SCCP1 trigger", and this code was changed to 34 on 23.09. on the
 *      strength of it. The device pack's ATDF says otherwise, with
 *      captions instead of numbers: 0x20 = 32 is "SCCP1 OC/IC Event",
 *      0x21 SCCP2, 0x22 = 34 SCCP3, and PTG is 0x1e = 30. So 34 selects
 *      SCCP3 - a module nothing configures - and the original 32 was
 *      right. Trust the ATDF here; it names what it selects.
 *   2. The auxiliary output. AUXOUT = 1 is "Timer Base Reset/Rollover",
 *      which is what this code used; the trigger the ADC listens for is
 *      AUXOUT = 2, "Special Event Trigger" in timer mode and "OC Event"
 *      in output-compare mode (ATDF value group CCP_CCP1CON2__AUXOUT).
 *      Microchip's knowledge base says the same in words: the special
 *      event trigger is the ADC's trigger, the rollover is not.
 *   3. The clock source. CLKSEL = 0 is the standard-speed peripheral
 *      clock, which comes from PLL2 - while the ADC runs off PLL1. From
 *      a support case: "ADC triggers go through synchronizers. If the
 *      trigger source is clocked from a different clock source than the
 *      ADC, trigger timing can be jittery. To avoid this the ADC trigger
 *      source module must be clocked from the same clock source used for
 *      ADC." CLKSEL = 1 selects Clock Generator 13, which clock.c puts
 *      on PLL1 next to CLKGEN6.
 *
 * All three are parameters here rather than fixed values, because the
 * documentation has been wrong about this module twice and the board is
 * the only authority left. The test matrix walks the combinations.
 */
#ifndef SCCP_H
#define SCCP_H

#include <stdint.h>
#include <stdbool.h>

/* ADC trigger code for this module, from the ATDF value group
 * AD_CH_CON1__TRG1SRC / __TRG2SRC: "SCCP1 OC/IC Event". */
#define SCCP1_ADC_TRIGGER   0x20u
/* What the datasheet table calls SCCP1 and the ATDF calls SCCP3 - kept
 * so the matrix can reproduce the failure that cost runs 5 to 7. */
#define SCCP3_ADC_TRIGGER   0x22u

/* Where the module gets its clock (CCP1CON1.CLKSEL). */
typedef enum {
    SCCP_CLK_PERIPHERAL = 0u,   /* standard speed peripheral clock, PLL2 */
    SCCP_CLK_GEN13      = 1u    /* Clock Generator 13, PLL1 like the ADC */
} sccp_clk_t;

/* What the module does (CCP1CON1.CCSEL/MOD). */
typedef enum {
    SCCP_MODE_TIMER = 0u,       /* 32-bit timer, period CCP1PR          */
    SCCP_MODE_OC    = 1u        /* output compare, single edge, 32-bit   */
} sccp_mode_t;

/* What comes out on the auxiliary output (CCP1CON2.AUXOUT). */
typedef enum {
    SCCP_EVENT_ROLLOVER = 1u,   /* timer base reset/rollover            */
    SCCP_EVENT_SPECIAL  = 2u    /* special event trigger / OC event      */
} sccp_event_t;

/* Start SCCP1 with a period of `ticks` of its own clock, in the given
 * combination. Safe to call again with new settings; the module is taken
 * down first. False for ticks < 2. */
bool     sccp1_start(uint32_t ticks, sccp_clk_t clk, sccp_mode_t mode, sccp_event_t ev);
void     sccp1_stop(void);

/* The clock the module actually runs on, in Hz - read from the clock
 * registers, not assumed, so a generator that did not come up shows up
 * as a wrong rate rather than as a silent factor. */
uint32_t sccp1_hz(void);
uint32_t sccp1_period(void);         /* ticks, as configured            */
/* The rate this setting should produce: sccp1_hz() / ticks, in kSPS. */
uint32_t sccp1_nominal_ksps(void);

/* CCP1CON1/CON2/PR and the counter read twice, as "name: value" lines
 * (part of regs_dump() and of the test matrix). Two equal reads of the
 * counter mean the module is not running. */
void     sccp1_regs_dump(void);

#endif /* SCCP_H */
