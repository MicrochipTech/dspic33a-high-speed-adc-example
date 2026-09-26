/*
 * test_crc16.c - host-side test for crc16.c (CRC-16/CCITT-FALSE)
 *
 * Built by tools\hosttest.bat with the installed MinGW gcc, against the
 * real crc16.c from the repo root - not a reimplementation.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "crc16.h"
#include "check.h"

int main(void)
{
    /* The documented check value, same as crc16_selfcheck(). */
    CHECK(crc16_selfcheck());

    static const uint8_t check_str[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    CHECK_EQ(crc16_ccitt_false(CRC16_INIT, check_str, sizeof check_str), CRC16_CHECK);

    /* Empty input leaves the running CRC at the initial value. */
    CHECK_EQ(crc16_ccitt_false(CRC16_INIT, check_str, 0), CRC16_INIT);

    /* Folding the bytes in two chunks gives the same result as one shot -
     * this is how the block writer in cli.c uses it (fold while sending,
     * not compute over one copy of the whole buffer). */
    uint16_t one_shot = crc16_ccitt_false(CRC16_INIT, check_str, sizeof check_str);
    uint16_t incremental = crc16_ccitt_false(CRC16_INIT, check_str, 4);
    incremental = crc16_ccitt_false(incremental, check_str + 4, (sizeof check_str) - 4);
    CHECK_EQ(incremental, one_shot);

    return check_summary();
}
