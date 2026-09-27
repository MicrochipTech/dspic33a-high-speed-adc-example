/*
 * dac.c - DAC1 and DAC2 in Triangle Wave mode (see dac.h)
 *
 * Register order as in datasheet Examples 18-2 and 18-3: data, slope, mode
 * bits, output enable, DAC enable, then the module-wide ON. The output
 * filter follows the mode by itself ("The Fast DAC mode is used in the
 * Triangle Wave mode", 18.4.x).
 *
 * The two units differ only in their register set, so one table drives
 * both - the same shape as the ADC core table in adc.c. DACCTRL1.ON and
 * CLKGEN7 are shared: switched on with the first unit, off with the last.
 *
 * THE UPDATE TRIGGER (added 25.09.2026). DACxCON.UPDTRG resets to 00,
 * which DS70005591D p1409 describes as: "After any write(s) to
 * DACxDAT[15:0]/DACLOW[15:0] or DACxSLPDAT[15:0], the user must set
 * DACxCON.UPDREQ bit manually." Neither this code nor Examples 18-2/18-3
 * ever did, so whether DACLOW, DACDAT and SLPDAT reached the DAC at all
 * was open - one candidate for the lower end a capture never showed and
 * for a period that came out about eight times off (ANALYSIS.md open
 * question 4). UPDTRG = 11 binary = 3 ("Any write ... sets the UPDATE bit
 * immediately") takes every write at once, which is also what the
 * CPU-stepped mode of the chain test needs; UPDREQ is additionally set
 * after the set-up writes, which costs nothing.
 *
 * THE TWO-SLOPE PERIOD (added 25.09.2026). A triangle period is two
 * slopes (dac.h, Equation 18-4); until this date dac_period_ns() returned
 * one slope and called it the period - a factor of 2 in every DAC verdict
 * that rested on it (ANALYSIS.md C.12.3).
 */

#include <xc.h>
#include <stddef.h>
#include "dac.h"
#include "clock.h"
#include "console.h"

#define DAC_UPDTRG_IMMEDIATE  3u      /* p1409: any write updates at once, binary 11 */

typedef struct {
    volatile uint32_t *CON, *DAT, *SLPCON, *SLPDAT;
    const char *pin;
} dac_unit_t;

static const dac_unit_t units[DAC_UNITS] = {
    { &DAC1CON, &DAC1DAT, &DAC1SLPCON, &DAC1SLPDAT, "RA1" },
    { &DAC2CON, &DAC2DAT, &DAC2SLPCON, &DAC2SLPDAT, "RA8" },
};

static struct {
    uint16_t low, high, slp;
    bool     on;
    bool     tri;                      /* running in Triangle Wave mode   */
} state[DAC_UNITS];

/* Bit positions are identical across the units (DACxCON / DACxSLPCON /
 * DACxDAT in the register summary), so the DAC1 bit-field types serve
 * both - the same trick adc.c uses with the AD3... types. */
#define CON(u)     (*(volatile DAC1CONBITS *)units[u].CON)
#define DAT(u)     (*(volatile DAC1DATBITS *)units[u].DAT)
#define SLPCON(u)  (*(volatile DAC1SLPCONBITS *)units[u].SLPCON)
#define SLPDAT(u)  (*(volatile DAC1SLPDATBITS *)units[u].SLPDAT)

static bool valid(uint8_t unit)
{
    return (unit >= 1u) && (unit <= DAC_UNITS);
}

static bool any_running(void)
{
    for (uint32_t i = 0; i < DAC_UNITS; i++) {
        if (state[i].on) { return true; }
    }
    return false;
}

/* Common finish: output buffer, enables, immediate update. */
static bool dac_enable(uint32_t u)
{
    CON(u).DACOEN    = 1u;             /* out to the pin DACOUTn          */
    CON(u).DACEN     = 1u;
    DACCTRL1bits.ON  = 1u;             /* all DAC/comparator modules      */
    CON(u).UPDREQ    = 1u;             /* harmless with UPDTRG = 3        */
    state[u].on = true;
    return true;
}

bool dac_triangle_start(uint8_t unit, uint16_t low, uint16_t high, uint16_t slpdat)
{
    if (!valid(unit)) { return false; }
    /* Note 1 of Example 18-3 (p1422): DACDAT at most 0xF32 - SLPDAT,
     * DACLOW at least 0xCD + SLPDAT, or the ends are not reached. */
    if ((slpdat == 0u) || (low < DAC_CODE_MIN + slpdat) ||
        (high > DAC_CODE_MAX - slpdat) || (high <= low)) {
        return false;
    }
    const uint32_t u = unit - 1u;
    if (!clock_dac_on()) { return false; }
    CON(u).DACEN     = 0u;
    SLPCON(u).SLOPEN = 0u;
    CON(u).UPDTRG    = DAC_UPDTRG_IMMEDIATE;
    DAT(u).DACLOW    = low;
    DAT(u).DACDAT    = high;
    SLPDAT(u).SLPDAT = slpdat;
    SLPCON(u).TWME   = 1u;             /* Triangle Wave mode              */
    SLPCON(u).SLOPEN = 1u;             /* slope generator on              */
    state[u].low = low; state[u].high = high; state[u].slp = slpdat;
    state[u].tri = true;
    return dac_enable(u);
}

bool dac_level_start(uint8_t unit, uint16_t code)
{
    if (!valid(unit)) { return false; }
    if ((code < DAC_CODE_MIN) || (code > DAC_CODE_MAX)) { return false; }
    const uint32_t u = unit - 1u;
    if (!clock_dac_on()) { return false; }
    CON(u).DACEN     = 0u;
    SLPCON(u).SLOPEN = 0u;
    SLPCON(u).TWME   = 0u;
    CON(u).UPDTRG    = DAC_UPDTRG_IMMEDIATE;
    DAT(u).DACDAT    = code;
    state[u].low = code; state[u].high = code; state[u].slp = 0u;
    state[u].tri = false;
    return dac_enable(u);
}

void dac_set(uint8_t unit, uint16_t code)
{
    /* From an interrupt at up to 100 kHz: one write, which UPDTRG = 3
     * takes at once. The caller keeps the code inside DAC_CODE_MIN..MAX. */
    if (!valid(unit)) { return; }
    DAT(unit - 1u).DACDAT = code;
}

void dac_off(uint8_t unit)
{
    if (!valid(unit)) { return; }
    const uint32_t u = unit - 1u;
    CON(u).DACEN     = 0u;
    CON(u).DACOEN    = 0u;
    SLPCON(u).SLOPEN = 0u;
    SLPCON(u).TWME   = 0u;
    state[u].on = false;
    state[u].tri = false;
    if (!any_running()) {              /* last one out turns off the light */
        DACCTRL1bits.ON = 0u;
        clock_dac_off();
    }
}

void dac_all_off(void)
{
    for (uint8_t unit = 1u; unit <= DAC_UNITS; unit++) {
        dac_off(unit);
    }
}

bool     dac_running(uint8_t unit)  { return valid(unit) && state[unit - 1u].on; }
bool     dac_triangle(uint8_t unit) { return valid(unit) && state[unit - 1u].on && state[unit - 1u].tri; }
uint16_t dac_low(uint8_t unit)      { return valid(unit) ? state[unit - 1u].low : 0u; }
uint16_t dac_high(uint8_t unit)     { return valid(unit) ? state[unit - 1u].high : 0u; }
uint16_t dac_slpdat(uint8_t unit)   { return valid(unit) ? state[unit - 1u].slp : 0u; }

const char *dac_pin_name(uint8_t unit)
{
    return valid(unit) ? units[unit - 1u].pin : "?";
}

uint8_t dac_active(void)
{
    if (state[1].on) { return 2u; }    /* DAC2 first: the boot test's own */
    if (state[0].on) { return 1u; }
    return 0u;
}

/* One slope, Equation 18-4 (p1420) solved for T_SLOPE with T_DAC =
 * 2 / F_DAC: T = (DACDAT - DACLOW) * 16 / SLPDAT * 2 / F_DAC, i.e.
 * (high - low) * 32 / (SLPDAT * F_DAC). SLPDAT is 12.4 fixed point
 * (p1414): the value moves SLPDAT/16 codes per DAC step. */
static uint64_t slope_clocks_x(uint32_t u, uint32_t scale)
{
    return (uint64_t)(state[u].high - state[u].low) * 32u * scale / state[u].slp;
}

uint32_t dac_period_ns(uint8_t unit)
{
    /* Two slopes - see the file header note on the 25.09.2026 fix. */
    if (!valid(unit)) { return 0u; }
    const uint32_t u = unit - 1u;
    const uint32_t f = clock_dac_hz();
    if (!state[u].tri || (state[u].slp == 0u) || (f == 0u)) { return 0u; }
    return (uint32_t)(2u * slope_clocks_x(u, 1000000000u) / f);
}

uint32_t dac_slope_samples_x1000(uint8_t unit, uint32_t sample_hz)
{
    /* Samples per slope at the given sample rate, times 1000:
     * T_slope * rate * 1000 = (high - low) * 32 * rate * 1000 / (SLPDAT *
     * F_DAC). 64-bit: 3700 * 32 * 40e6 * 1000 is 4.7e15. */
    if (!valid(unit)) { return 0u; }
    const uint32_t u = unit - 1u;
    const uint32_t f = clock_dac_hz();
    if (!state[u].tri || (state[u].slp == 0u) || (f == 0u)) { return 0u; }
    return (uint32_t)(slope_clocks_x(u, 1000u) * sample_hz / f);
}

/* DAC2-only names for the chain test's fixed pin route - see dac.h. */
bool     dac2_triangle_start(uint16_t low, uint16_t high, uint16_t slpdat) { return dac_triangle_start(2u, low, high, slpdat); }
bool     dac2_level_start(uint16_t code)     { return dac_level_start(2u, code); }
void     dac2_set(uint16_t code)             { dac_set(2u, code); }
void     dac2_off(void)                      { dac_off(2u); }
bool     dac2_running(void)                  { return dac_running(2u); }
bool     dac2_triangle(void)                 { return dac_triangle(2u); }
uint16_t dac2_low(void)                      { return dac_low(2u); }
uint16_t dac2_high(void)                     { return dac_high(2u); }
uint16_t dac2_slpdat(void)                   { return dac_slpdat(2u); }
uint32_t dac2_period_ns(void)                { return dac_period_ns(2u); }
uint32_t dac2_slope_samples_x1000(uint32_t sample_hz) { return dac_slope_samples_x1000(2u, sample_hz); }

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
 * that shows what the board's touch-pad network on RA8 does to the
 * signal - the internal path has neither the pin nor the network.
 *
 * UREFOUTEN additionally drives UREF onto its external pin. The internal
 * path should not need it, so it is off by default; if the ADC reads a
 * flat value the parameter lets it be tried without a rebuild. */
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
    console_kv_hex("DAC1CON", DAC1CON);
    console_kv_hex("DAC1DAT", DAC1DAT);
    console_kv_hex("DAC1SLPCON", DAC1SLPCON);
    console_kv_hex("DAC1SLPDAT", DAC1SLPDAT);
    console_kv_hex("DAC2CON", DAC2CON);
    console_kv_hex("DAC2DAT", DAC2DAT);
    console_kv_hex("DAC2SLPCON", DAC2SLPCON);
    console_kv_hex("DAC2SLPDAT", DAC2SLPDAT);
    console_kv_hex("UREFCON", UREFCON);   /* INSEL 7 = DAC2 on the internal UREF line */
    console_kv("DAC clock Hz (read back)", clock_dac_hz());
}
