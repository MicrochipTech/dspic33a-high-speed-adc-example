/*******************************************************************************
 * Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries.
 *
 * Subject to your compliance with these terms, you may use Microchip software
 * and any derivatives exclusively with Microchip products. It is your
 * responsibility to comply with third party license terms applicable to your
 * use of third party software (including open source software) that may
 * accompany Microchip software.
 *
 * THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
 * EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
 * WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
 * PARTICULAR PURPOSE.
 *
 * IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
 * INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
 * WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
 * BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
 * FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
 * ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
 * THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
 ******************************************************************************/

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
