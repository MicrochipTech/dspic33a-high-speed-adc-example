/*
 * dac.c - DAC2 in Triangle Wave mode (see dac.h)
 *
 * Register order as in datasheet Examples 18-2 and 18-3: data, slope,
 * mode bits, output enable, DAC enable, then the module-wide ON. The
 * output filter follows the mode by itself ("The Fast DAC mode is used in
 * the Triangle Wave mode", 18.4.x).
 */

#include <xc.h>
#include "dac.h"
#include "clock.h"
#include "console.h"

static uint16_t dac_low  = 0;
static uint16_t dac_high = 0;
static uint16_t dac_slp  = 0;
static bool     dac_on   = false;

bool dac2_triangle_start(uint16_t low, uint16_t high, uint16_t slpdat)
{
    if (!clock_dac_on()) { return false; }
    DAC2CONbits.DACEN     = 0u;
    DAC2SLPCONbits.SLOPEN = 0u;
    DAC2DATbits.DACLOW    = low;
    DAC2DATbits.DACDAT    = high;
    DAC2SLPDATbits.SLPDAT = slpdat;
    DAC2SLPCONbits.TWME   = 1u;        /* Triangle Wave mode              */
    DAC2SLPCONbits.SLOPEN = 1u;        /* slope generator on              */
    DAC2CONbits.DACOEN    = 1u;        /* to the pin DACOUT2 = RA8        */
    DAC2CONbits.DACEN     = 1u;
    DACCTRL1bits.ON       = 1u;        /* all DAC/comparator modules      */
    dac_low = low; dac_high = high; dac_slp = slpdat; dac_on = true;
    return true;
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
}

bool     dac2_running(void) { return dac_on; }
uint16_t dac2_low(void)     { return dac_low; }
uint16_t dac2_high(void)    { return dac_high; }
uint16_t dac2_slpdat(void)  { return dac_slp; }

uint32_t dac2_period_ns(void)
{
    /* Two slopes of (high - low) * 16 DAC clocks / SLPDAT each. */
    if ((dac_slp == 0u) || (dac_high <= dac_low)) { return 0u; }
    const uint64_t clocks = (uint64_t)(dac_high - dac_low) * 32u;
    return (uint32_t)((clocks * 1000000000ull) / ((uint64_t)clock_dac_hz() * dac_slp));
}

void dac_regs_dump(void)
{
    console_puts("[regs] dac\r\n");
    console_kv_hex("DACCTRL1", DACCTRL1);
    console_kv_hex("DAC2CON", DAC2CON);
    console_kv_hex("DAC2DAT", DAC2DAT);
    console_kv_hex("DAC2SLPCON", DAC2SLPCON);
    console_kv_hex("DAC2SLPDAT", DAC2SLPDAT);
}
