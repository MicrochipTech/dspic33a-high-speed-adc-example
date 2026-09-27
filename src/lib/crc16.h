/*
 * crc16.h - CRC-16/CCITT-FALSE for the binary block transfer (crc16.c)
 *
 * Polynomial 0x1021, initial value 0xFFFF, no reflection of input or
 * output, no final xor. The check value of the string "123456789" is
 * 0x29B1 - that constant is what makes an implementation identifiable,
 * because the name "CRC-16 CCITT" is used for at least four different
 * parameter sets in the wild. The same check is asserted on the Python
 * side in tools/adc_gui.py, so both ends are pinned to the same variant.
 *
 * Computed over the payload bytes only, not over the header line and not
 * over the CRC line itself (docs/PLAN-BINARY-TRANSFER.md).
 */
#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>
#include <stddef.h>

#define CRC16_INIT      0xFFFFu
#define CRC16_CHECK     0x29B1u     /* of "123456789" */

/* Fold `len` bytes into a running CRC. Start with CRC16_INIT and feed the
 * payload in as many chunks as convenient - the block writer computes the
 * CRC while it sends, so that no second copy of the buffer is needed. */
uint16_t crc16_ccitt_false(uint16_t crc, const uint8_t *data, size_t len);

/* True if the implementation produces the documented check value. Cheap
 * enough to run at start-up in the simulator build, where nobody can type
 * a command to test it. */
bool crc16_selfcheck(void);

#endif /* CRC16_H */
