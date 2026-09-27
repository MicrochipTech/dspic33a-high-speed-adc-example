/*
 * crc16.c - CRC-16/CCITT-FALSE (see crc16.h)
 *
 * Bitwise, without a table: a 512-byte table would save perhaps 40 us on
 * a 4 KB block that takes 360 ms to send at 115200 baud. The table is not
 * worth the RAM or the flash here.
 */

#include <stdbool.h>
#include "crc16.h"

uint16_t crc16_ccitt_false(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

bool crc16_selfcheck(void)
{
    static const uint8_t check[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    return crc16_ccitt_false(CRC16_INIT, check, sizeof check) == CRC16_CHECK;
}
