/*
 * config_bits.c
 *
 * Configuration bits of the ADC/DMA example. Kept in a file of their own
 * so that a pack-version problem (see below) is found here and nowhere
 * else.
 */

#include <xc.h>

/* ------------------------------------------------------------------ *
 * Configuration bits
 *
 * FICD_NOBTSWP is written as a NUMBER on purpose, because the accepted
 * symbolic names DEPEND ON THE PACK VERSION:
 *
 *   dsPIC33AK-MP_DFP 1.3.185 : ON            / OFF
 *   dsPIC33AK-MP_DFP 1.4.260 : BTSWP_ENABLED / BTSWP_DISABLED
 *
 * Same bit, same meaning, different spelling - so a file written against
 * one pack fails to compile against the other, with a message that makes
 * it look as if the value itself were invalid:
 *
 *   error: unknown value for configuration setting 'FICD_NOBTSWP'
 *
 * That is also the most likely reason an MCC-generated config_bits.c
 * suddenly stops compiling: MCC generated it against a different pack
 * than the one the build uses.
 *
 * The numeric form is accepted by every pack version. Verified: 0x0 built
 * against packs 1.3.185 and 1.4.260, and BTSWP_ENABLED built against
 * 1.4.260, all produce a bit-identical HEX file.
 *
 * FICD, mask 0x8000, value 0x0 = BOOTSWP instruction enabled.
 * (From the ATDF value-group FICD_NOBTSWP of both packs.)
 * ------------------------------------------------------------------ */
#pragma config FICD_NOBTSWP = 0x0   /* BOOTSWP enabled - see above */

/* Watchdog left under software control, so it stays off unless the
 * application turns it on via WDTCON.ON. Valid values are SW and HW
 * (ATDF value-group FWDT_WDTEN) - there is no "OFF". */
#pragma config FWDT_WDTEN = SW
