/*
 * dac.c - DAC2 as the test signal (see dac.h)
 *
 * Register order as in datasheet Examples 18-2 and 18-3: data, slope,
 * mode bits, output enable, DAC enable, then the module-wide ON. The
 * output filter follows the mode by itself ("The Fast DAC mode is used in
 * the Triangle Wave mode", 18.4.x).
 *
 * THE UPDATE TRIGGER (added 25.09.2026). DACxCON.UPDTRG resets to 00,
 * which DS70005591D p1409 describes as: "After any write(s) to
 * DACxDAT[15:0]/DACLOW[15:0] or DACxSLPDAT[15:0], the user must set
 * DACxCON.UPDREQ bit manually." Neither this code nor Examples 18-2/18-3
 * ever did, so whether DACLOW, DACDAT and SLPDAT reached the DAC at all
 * is open - one candidate for the lower end the captures never showed
 * and for the period that came out about eight times off (ANALYSIS.md
 * open question 4). UPDTRG = 11 ("Any write ... sets the UPDATE bit
 * immediately") takes every write at once, which is also what the
 * CPU-stepped mode of the chain test needs; UPDREQ is additionally set
 * after the set-up writes, which costs nothing.
 */

#include <xc.h>
#include "dac.h"
#include "clock.h"
#include "console.h"

#define DAC_UPDTRG_IMMEDIATE  3u      /* p1409: any write updates at once */

static uint16_t dac_low  = 0;
static uint16_t dac_high = 0;
static uint16_t dac_slp  = 0;
static bool     dac_on   = false;
static bool     dac_tri  = false;     /* triangle (true) or static level  */

/* Common start: clock, update mode, output buffer, enables. */
static bool dac2_enable(void)
{
    DAC2CONbits.DACOEN = 1u;           /* to the pin DACOUT2 = RA8        */
    DAC2CONbits.DACEN  = 1u;
    DACCTRL1bits.ON    = 1u;           /* all DAC/comparator modules      */
    DAC2CONbits.UPDREQ = 1u;           /* harmless with UPDTRG = 11       */
    dac_on = true;
    return true;
}

bool dac2_triangle_start(uint16_t low, uint16_t high, uint16_t slpdat)
{
    /* Note 1 of Example 18-3 (p1422): DACDAT at most 0xF32 - SLPDAT,
     * DACLOW at least 0xCD + SLPDAT, or the ends are not reached. */
    if ((slpdat == 0u) || (low < DAC_CODE_MIN + slpdat) ||
        (high > DAC_CODE_MAX - slpdat) || (high <= low)) {
        return false;
    }
    if (!clock_dac_on()) { return false; }
    DAC2CONbits.DACEN     = 0u;
    DAC2SLPCONbits.SLOPEN = 0u;
    DAC2CONbits.UPDTRG    = DAC_UPDTRG_IMMEDIATE;
    DAC2DATbits.DACLOW    = low;
    DAC2DATbits.DACDAT    = high;
    DAC2SLPDATbits.SLPDAT = slpdat;
    DAC2SLPCONbits.TWME   = 1u;        /* Triangle Wave mode              */
    DAC2SLPCONbits.SLOPEN = 1u;        /* slope generator on              */
    dac_low = low; dac_high = high; dac_slp = slpdat; dac_tri = true;
    return dac2_enable();
}

bool dac2_level_start(uint16_t code)
{
    if ((code < DAC_CODE_MIN) || (code > DAC_CODE_MAX)) { return false; }
    if (!clock_dac_on()) { return false; }
    DAC2CONbits.DACEN     = 0u;
    DAC2SLPCONbits.SLOPEN = 0u;
    DAC2SLPCONbits.TWME   = 0u;
    DAC2CONbits.UPDTRG    = DAC_UPDTRG_IMMEDIATE;
    DAC2DATbits.DACDAT    = code;
    dac_low = code; dac_high = code; dac_slp = 0u; dac_tri = false;
    return dac2_enable();
}

void dac2_set(uint16_t code)
{
    /* From an interrupt at up to 100 kHz: one write, which UPDTRG = 11
     * takes at once. The caller keeps the code inside DAC_CODE_MIN..MAX. */
    DAC2DATbits.DACDAT = code;
}

void dac2_off(void)
{
    DAC2CONbits.DACEN     = 0u;
    DAC2CONbits.DACOEN    = 0u;
    DAC2SLPCONbits.SLOPEN = 0u;
    DAC2SLPCONbits.TWME   = 0u;
    DACCTRL1bits.ON       = 0u;
    clock_dac_off();
    dac_on = false;
    dac_tri = false;
}

bool     dac2_running(void)  { return dac_on; }
bool     dac2_triangle(void) { return dac_on && dac_tri; }
uint16_t dac2_low(void)      { return dac_low; }
uint16_t dac2_high(void)     { return dac_high; }
uint16_t dac2_slpdat(void)   { return dac_slp; }

/* One slope, Equation 18-4 (p1420) solved for T_SLOPE with T_DAC =
 * 2 / F_DAC: T = (DACDAT - DACLOW) * 16 / SLPDAT * 2 / F_DAC, i.e.
 * (high - low) * 32 / (SLPDAT * F_DAC). SLPDAT is 12.4 fixed point
 * (p1414): the value moves SLPDAT/16 codes per DAC step. */
static uint64_t slope_clocks_x(uint32_t scale)
{
    return (uint64_t)(dac_high - dac_low) * 32u * scale / dac_slp;
}

uint32_t dac2_period_ns(void)
{
    /* Two slopes. Until 25.09.2026 this returned ONE slope and called it
     * the period (ANALYSIS.md C.12.3) - a factor of 2 in every DAC
     * verdict that rested on it. */
    const uint32_t f = clock_dac_hz();
    if (!dac2_triangle() || (f == 0u)) { return 0u; }
    return (uint32_t)(2u * slope_clocks_x(1000000000u) / f);
}

uint32_t dac2_slope_samples_x1000(uint32_t sample_hz)
{
    /* Samples per slope at the given sample rate, times 1000:
     * T_slope * rate * 1000 = (high - low) * 32 * rate * 1000 / (SLPDAT *
     * F_DAC). 64-bit: 3700 * 32 * 40e6 * 1000 is 4.7e15. */
    const uint32_t f = clock_dac_hz();
    if (!dac2_triangle() || (f == 0u)) { return 0u; }
    return (uint32_t)(slope_clocks_x(1000u) * sample_hz / f);
}

/* ------------------------------------------------------------------ *
 * The internal path from the DAC to the ADC: UREFCON
 *
 * UREFCON.INSEL selects what the chip's internal reference line UREF
 * carries (ATDF value group UREFCON_CON__INSEL): 1 AVDD/2, 2 VDD/2,
 * 3 VDDcore, 4 bandgap, 5 temperature sensor, 6..13 DAC1..DAC8, 14 AVSS,
 * 15 AVDD. And ADnAN7 is "ADC n UREF input" on EVERY core (Table 16-2).
 *
 * So the ADC can measure the DAC without a pin: route DAC2 onto UREF and
 * sample AN7. The chain test uses the pin route (DACOUT2 = RA8 = AD5AN3,
 * decided 25.09.2026, ANALYSIS.md C.11) and this one as the comparison
 * that shows what the board's touch-pad network on RA8 does to the signal.
 *
 * UREFOUTEN additionally drives UREF onto its external pin. The internal
 * path should not need it, so it is off by default. */
#define UREF_INSEL_DAC2   7u

bool uref_route_dac2(bool drive_pin)
{
    UREFCONbits.ON = 0u;
    UREFCONbits.INSEL = UREF_INSEL_DAC2;
    UREFCONbits.UREFOUTEN = drive_pin ? 1u : 0u;
    UREFCONbits.ON = 1u;
    return (UREFCONbits.INSEL == UREF_INSEL_DAC2) && (UREFCONbits.ON != 0u);
}

void uref_off(void)
{
    UREFCONbits.ON = 0u;
    UREFCONbits.UREFOUTEN = 0u;
}

uint32_t uref_insel(void) { return UREFCONbits.INSEL; }

void dac_regs_dump(void)
{
    console_puts("[regs] dac\r\n");
    console_kv_hex("DACCTRL1", DACCTRL1);
    console_kv_hex("DAC2CON", DAC2CON);
    console_kv_hex("DAC2DAT", DAC2DAT);
    console_kv_hex("DAC2SLPCON", DAC2SLPCON);
    console_kv_hex("DAC2SLPDAT", DAC2SLPDAT);
    console_kv_hex("UREFCON", UREFCON);   /* INSEL 7 = DAC2 on the internal UREF line */
    console_kv("DAC clock Hz (read back)", clock_dac_hz());
}
