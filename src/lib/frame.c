/*
 * frame.c - see frame.h. Hardware-free: only lib/crc16 and lib/fmt, the
 * output sink is a caller-supplied function pointer.
 */
#include "frame.h"
#include "crc16.h"
#include "fmt.h"

/* Four upper-case hex digits, most significant first, no prefix and no
 * terminator - cli.c's old inline loop in cmd_blk_fn()/cmd_stream_grab(),
 * moved verbatim. Writes exactly 4 bytes at `out`. */
static void hex4_upper(char *out, uint16_t v)
{
    for (int d = 12; d >= 0; d -= 4) {
        const uint32_t nib = ((uint32_t)v >> d) & 0xFu;
        *out++ = (char)((nib < 10u) ? ('0' + nib) : ('A' + (nib - 10u)));
    }
}

uint32_t frame_send(frame_write_fn write, frame_aborted_fn aborted,
                     const char *header, size_t header_len,
                     const volatile uint16_t *samples, uint32_t n)
{
    (void)write((const uint8_t *)header, header_len);

    if (n == 0u) {
        static const char zero_tail[] = "CRC 0000\r\n";
        (void)write((const uint8_t *)zero_tail, (sizeof zero_tail) - 1u);
        return 0u;
    }

    /* The payload, and the CRC computed while it goes out - no second copy
     * of the buffer anywhere. The device is little endian, so the low byte
     * of each 12-bit sample is sent first (what the client reads with
     * dtype "<u2"). */
    uint8_t  chunk[FRAME_CHUNK];
    uint16_t crc  = CRC16_INIT;
    uint32_t sent = 0u;
    for (uint32_t i = 0; i < n; ) {
        uint32_t k = 0u;
        while ((k < (FRAME_CHUNK - 1u)) && (i < n)) {
            const uint16_t v = samples[i++];
            chunk[k++] = (uint8_t)(v & 0xFFu);
            chunk[k++] = (uint8_t)((v >> 8) & 0x0Fu);   /* 12-bit result */
        }
        crc = crc16_ccitt_false(crc, chunk, k);
        sent += (uint32_t)write(chunk, k);
        if ((aborted != NULL) && aborted()) { break; }
    }

    char tail[16];      /* "\r\nCRC XXXX\r\n\0" = 13 bytes */
    char *p = copy_str(tail, "\r\nCRC ");
    hex4_upper(p, crc);
    p += 4;
    copy_str(p, "\r\n");
    (void)write((const uint8_t *)tail, 12u);

    return sent;
}
