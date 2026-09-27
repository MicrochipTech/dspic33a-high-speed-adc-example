/*
 * config_bits.c
 *
 * Configuration bits of the ADC/DMA example, dsPIC33AK512MPS512.
 *
 * Every configuration word of the device is set here explicitly, so the
 * programmed state does not depend on what the programmer does with words
 * the project leaves out. The list is what MPLAB X "Generate Source Code
 * from Hex" produced from a build of this project (23.09.2026), gone over
 * bit by bit against the pack's ATDF; every value is the erased default of
 * its word except the two marked "set on purpose". Each register says why
 * its value is right for this example.
 *
 * Two names DEPEND ON THE PACK VERSION and are written as NUMBERS, because
 * a file written against one pack fails to compile against the other, with
 * a message that makes it look as if the value itself were invalid:
 *
 *   error: unknown value for configuration setting 'FICD_NOBTSWP'
 *
 *   FICD_NOBTSWP   1.3.185: ON / OFF        1.4.260: BTSWP_ENABLED / BTSWP_DISABLED
 *   FWDT_RCLKSEL   1.3.185: BFRC256         1.4.260: BFRC244   (same bits, 0x3)
 *
 * Same bit, same meaning, different spelling. That is also the most likely
 * reason an MCC-generated config_bits.c suddenly stops compiling: MCC
 * generated it against a different pack than the one the build uses. The
 * numeric form is accepted by every pack version. Verified: this file
 * builds against 1.3.185 and 1.4.260 and both produce the same HEX. All
 * other names below are identical in both packs (ATDF value-groups
 * compared).
 *
 * Not set here: FBOOT (boot mode / partition word). MPLAB X does not list
 * it, it is not part of the normal image, and the erased state is what we
 * want: single partition.
 */

#include <xc.h>

/* ---- FCP: code protection ----------------------------------------------
 * None. This is an example that gets reprogrammed and read back all the
 * time; CP = ON would also lock the debugger out of the flash. */
#pragma config FCP_CP = OFF             /* memory protection disabled          */
#pragma config FCP_CRC = OFF            /* no CRC check of the image at boot   */
#pragma config FCP_WPUCA = OFF          /* user config areas writable          */

/* ---- FICD: debug interface ---------------------------------------------
 * JTAG off: the PKOB4 on the board debugs over ICSP (PGC/PGD), and JTAG
 * on would take four port pins away for nothing.
 * NOBTSWP: bit 15 of FICD, mask 0x8000; 0x0 = BOOTSWP instruction enabled
 * (set on purpose, the erased value is 1). Irrelevant in single-partition
 * mode, kept for the pack-name story above (value-group FICD_NOBTSWP). */
#pragma config FICD_JTAGEN = OFF        /* JTAG disabled                       */
#pragma config FICD_NOBTSWP = 0x0       /* BOOTSWP enabled - see above         */

/* ---- FDEVOPT: device options -------------------------------------------
 * The I2C and SPI2 pin choices do not matter, nothing here uses them.
 * BISTDIS = OFF (bit = 1) means the RAM self-test at start-up is DISABLED.
 * Keep it that way: diag.c keeps boot_stage and the last trap in
 * persistent RAM so they survive a reset, and a RAM BIST on every reset
 * would wipe exactly that. */
#pragma config FDEVOPT_ALTI2C1 = OFF    /* primary I2C1 pins                   */
#pragma config FDEVOPT_ALTI2C2 = OFF    /* primary I2C2 pins                   */
#if !defined(__dsPIC33AK512MPS506__) && !defined(__dsPIC33AK512MPS505__)
#pragma config FDEVOPT_ALTI2C3 = OFF    /* primary I2C3 pins - the 64-pin parts have no I2C3 and no such bit (pack ATDF) */
#endif
#pragma config FDEVOPT_BISTDIS = OFF    /* start-up RAM test disabled          */
#pragma config FDEVOPT_SPI2PIN = OFF    /* SPI2 pins via PPS                   */

/* ---- FWDT: watchdog ----------------------------------------------------
 * WDTEN = SW (set on purpose, the erased value is HW = always on): the
 * watchdog is off unless the application sets WDTCON.ON, and nothing here
 * does. That matters: the main loop stalls on purpose in fail() and in
 * the trap handler to blink a code, and a hardware watchdog would turn
 * every reported stop into a silent reboot. The remaining bits only apply
 * once the WDT is switched on; they are the erased values (longest
 * period, no window, reset on timeout). Valid values for WDTEN are SW and
 * HW - there is no "OFF".
 * RCLKSEL: bits 7:6, mask 0xC0; 0x3 = BFRC / 244 (1.4.260) alias
 * BFRC / 256 (1.3.185), the same 32.78 kHz clock, name differs per pack. */
#pragma config FWDT_WINDIS = ON         /* non-window mode                     */
#pragma config FWDT_SWDTMPS = PS2147483648 /* sleep-mode postscaler, max       */
#pragma config FWDT_RCLKSEL = 0x3       /* BFRC/244 ~ 32.78 kHz - see above    */
#pragma config FWDT_RWDTPS = PS2147483648  /* run-mode postscaler, max         */
#pragma config FWDT_WDTWIN = WIN25      /* window 25 % (unused, WINDIS)        */
#pragma config FWDT_WDTEN = SW          /* WDT under software control = off    */
#pragma config FWDT_WDTRSTEN = ON       /* WDT event would reset               */
#pragma config FWDT_WDTNVMSTL = ON      /* WDT stalls during NVM operations    */

/* ---- FPR0..7: flash region protection ----------------------------------
 * All eight regions disabled (RDIS = ON). With RDIS set, the permission,
 * CRC, type and partition bits of the same word are not evaluated, and
 * START/END = 0x7FF is the erased value. No region of this example needs
 * to be protected, executable-only or immutable. */
#pragma config FPR0CTRL_RDIS = ON       /* region 0 protection disabled        */
#pragma config FPR0CTRL_ERAO = ON
#pragma config FPR0CTRL_EX = ON
#pragma config FPR0CTRL_RD = ON
#pragma config FPR0CTRL_WR = ON
#pragma config FPR0CTRL_CRC = ON
#pragma config FPR0CTRL_RTYPE = FIRMWARE
#pragma config FPR0CTRL_PSEL = BOTH
#pragma config FPR0ST_START = 0x7FF
#pragma config FPR0END_END = 0x7FF

#pragma config FPR1CTRL_RDIS = ON       /* region 1 protection disabled        */
#pragma config FPR1CTRL_ERAO = ON
#pragma config FPR1CTRL_EX = ON
#pragma config FPR1CTRL_RD = ON
#pragma config FPR1CTRL_WR = ON
#pragma config FPR1CTRL_CRC = ON
#pragma config FPR1CTRL_RTYPE = FIRMWARE
#pragma config FPR1CTRL_PSEL = BOTH
#pragma config FPR1ST_START = 0x7FF
#pragma config FPR1END_END = 0x7FF

#pragma config FPR2CTRL_RDIS = ON       /* region 2 protection disabled        */
#pragma config FPR2CTRL_ERAO = ON
#pragma config FPR2CTRL_EX = ON
#pragma config FPR2CTRL_RD = ON
#pragma config FPR2CTRL_WR = ON
#pragma config FPR2CTRL_CRC = ON
#pragma config FPR2CTRL_RTYPE = FIRMWARE
#pragma config FPR2CTRL_PSEL = BOTH
#pragma config FPR2ST_START = 0x7FF
#pragma config FPR2END_END = 0x7FF

#pragma config FPR3CTRL_RDIS = ON       /* region 3 protection disabled        */
#pragma config FPR3CTRL_ERAO = ON
#pragma config FPR3CTRL_EX = ON
#pragma config FPR3CTRL_RD = ON
#pragma config FPR3CTRL_WR = ON
#pragma config FPR3CTRL_CRC = ON
#pragma config FPR3CTRL_RTYPE = FIRMWARE
#pragma config FPR3CTRL_PSEL = BOTH
#pragma config FPR3ST_START = 0x7FF
#pragma config FPR3END_END = 0x7FF

#pragma config FPR4CTRL_RDIS = ON       /* region 4 protection disabled        */
#pragma config FPR4CTRL_ERAO = ON
#pragma config FPR4CTRL_EX = ON
#pragma config FPR4CTRL_RD = ON
#pragma config FPR4CTRL_WR = ON
#pragma config FPR4CTRL_CRC = ON
#pragma config FPR4CTRL_RTYPE = FIRMWARE
#pragma config FPR4CTRL_PSEL = BOTH
#pragma config FPR4ST_START = 0x7FF
#pragma config FPR4END_END = 0x7FF

#pragma config FPR5CTRL_RDIS = ON       /* region 5 protection disabled        */
#pragma config FPR5CTRL_ERAO = ON
#pragma config FPR5CTRL_EX = ON
#pragma config FPR5CTRL_RD = ON
#pragma config FPR5CTRL_WR = ON
#pragma config FPR5CTRL_CRC = ON
#pragma config FPR5CTRL_RTYPE = FIRMWARE
#pragma config FPR5CTRL_PSEL = BOTH
#pragma config FPR5ST_START = 0x7FF
#pragma config FPR5END_END = 0x7FF

#pragma config FPR6CTRL_RDIS = ON       /* region 6 protection disabled        */
#pragma config FPR6CTRL_ERAO = ON
#pragma config FPR6CTRL_EX = ON
#pragma config FPR6CTRL_RD = ON
#pragma config FPR6CTRL_WR = ON
#pragma config FPR6CTRL_CRC = ON
#pragma config FPR6CTRL_RTYPE = FIRMWARE
#pragma config FPR6CTRL_PSEL = BOTH
#pragma config FPR6ST_START = 0x7FF
#pragma config FPR6END_END = 0x7FF

#pragma config FPR7CTRL_RDIS = ON       /* region 7 protection disabled        */
#pragma config FPR7CTRL_ERAO = ON
#pragma config FPR7CTRL_EX = ON
#pragma config FPR7CTRL_RD = ON
#pragma config FPR7CTRL_WR = ON
#pragma config FPR7CTRL_CRC = ON
#pragma config FPR7CTRL_RTYPE = FIRMWARE
#pragma config FPR7CTRL_PSEL = BOTH
#pragma config FPR7ST_START = 0x7FF
#pragma config FPR7END_END = 0x7FF

/* ---- FIRT, FSECDBG, FPED: security features ----------------------------
 * All off. FPED_ICSPPED = OFF is the one that must never change by
 * accident: ON disables programming and erasing over ICSP, and the PKOB4
 * on the board could then no longer reprogram the part. */
#pragma config FIRT_IRT = OFF           /* no immutable root of trust          */
#pragma config FSECDBG_SECDBG = OFF     /* plain debug, no secure debug        */
#pragma config FPED_ICSPPED = OFF       /* ICSP may program and erase          */

/* ---- FEPUCB, FWPUCB: user configuration page protection ----------------
 * All ones = nothing protected, the config words stay erasable and
 * writable by the programmer. */
#pragma config FEPUCB_EPUCB = 0xFFFFFFFF
#pragma config FWPUCB_WPUCB = 0xFFFFFFFF
