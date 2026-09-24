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
 */

#include <xc.h>
#include <stddef.h>
#include "dac.h"
#include "clock.h"
#include "console.h"

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

bool dac_triangle_start(uint8_t unit, uint16_t low, uint16_t high, uint16_t slpdat)
{
    if (!valid(unit)) { return false; }
    const uint32_t u = unit - 1u;
    if (!clock_dac_on()) { return false; }
    CON(u).DACEN     = 0u;
    SLPCON(u).SLOPEN = 0u;
    DAT(u).DACLOW    = low;
    DAT(u).DACDAT    = high;
    SLPDAT(u).SLPDAT = slpdat;
    SLPCON(u).TWME   = 1u;             /* Triangle Wave mode              */
    SLPCON(u).SLOPEN = 1u;             /* slope generator on              */
    CON(u).DACOEN    = 1u;             /* out to the pin DACOUTn          */
    CON(u).DACEN     = 1u;
    DACCTRL1bits.ON  = 1u;             /* all DAC/comparator modules      */
    state[u].low = low; state[u].high = high; state[u].slp = slpdat;
    state[u].on = true;
    return true;
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

bool     dac_running(uint8_t unit) { return valid(unit) && state[unit - 1u].on; }
uint16_t dac_low(uint8_t unit)     { return valid(unit) ? state[unit - 1u].low : 0u; }
uint16_t dac_high(uint8_t unit)    { return valid(unit) ? state[unit - 1u].high : 0u; }
uint16_t dac_slpdat(uint8_t unit)  { return valid(unit) ? state[unit - 1u].slp : 0u; }

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

uint32_t dac_period_ns(uint8_t unit)
{
    /* Two slopes of (high - low) * 16 DAC clocks / SLPDAT each. */
    if (!valid(unit)) { return 0u; }
    const uint32_t u = unit - 1u;
    if ((state[u].slp == 0u) || (state[u].high <= state[u].low)) { return 0u; }
    const uint64_t clocks = (uint64_t)(state[u].high - state[u].low) * 32u;
    return (uint32_t)((clocks * 1000000000ull) /
                      ((uint64_t)clock_dac_hz() * state[u].slp));
}

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
}
