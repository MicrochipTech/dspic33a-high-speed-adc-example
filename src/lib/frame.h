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
 * frame.h - the binary frame "blk" and "stream grab" both send (frame.c)
 *
 * P6.3 of docs/IMPLEMENTATION-PLAN.md: the header line, the payload in
 * chunks with the CRC folded in as it goes, and the CRC line, moved out of
 * cli.c's cmd_blk_fn()/cmd_stream_grab() into one hardware-free, host-
 * testable function - the two commands built the identical shape by hand,
 * the only difference being the header's own fields. Framing is documented
 * in docs/PLAN-BINARY-TRANSFER.md:
 *
 *   <header, text, given by the caller, already ending in "\r\n">
 *   <2*n bytes, little endian, 12-bit sample in bits 11:0>      (n > 0)
 *   "\r\nCRC <4 hex digits, upper case>\r\n"
 *
 * or, when n == 0 (no burst to send - "blk"'s and "stream grab"'s shared
 * failure shape: no data, header already said n=0 and every other field
 * 0, the client's synchronisation on the prompt and ACK/NAK unaffected):
 *
 *   <header>
 *   "CRC 0000\r\n"
 *
 * frame_send() does not build the header text itself - "BIN n=... p1=...
 * ..." and "GRAB n=... from=... ..." share nothing but the shape, and the
 * caller already has u32_to_str()/copy_str() (lib/fmt) at hand to build
 * it. What frame.c owns is the part that was byte-for-byte identical
 * between the two commands: the chunking, the wire encoding of a sample,
 * the running CRC-16/CCITT-FALSE (lib/crc16) and the CRC line's hex
 * formatting.
 */
#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Bytes per write() call for the payload - cli.c's old BLK_CHUNK. Most
 * calls carry FRAME_CHUNK - 1 rounded down to a whole number of samples
 * (2 bytes each), i.e. 32 samples/64 bytes; the last chunk of a window may
 * be shorter. */
#define FRAME_CHUNK 64u

/* Output sink - exactly console_write_raw()'s signature (src/core/console.h),
 * so the firmware caller passes it straight through with no wrapper and a
 * host test can pass one that appends to a memory buffer. Takes as many of
 * the `len` bytes as it can and returns how many it actually took. */
typedef size_t (*frame_write_fn)(const uint8_t *data, size_t len);

/* Optional: true once the transfer should stop early (Ctrl+C on a long
 * block). Checked only between payload chunks - never mid-chunk, never
 * around the header or the CRC line - exactly where cmd_blk_fn() and
 * cmd_stream_grab() checked cmd_parser_aborted() before this task. NULL
 * means "never abort" (a host test's case: there is nothing to abort for). */
typedef bool (*frame_aborted_fn)(void);

/* Sends one frame as described above. `header`/`header_len` are written
 * through write() first, unconditionally and verbatim - the caller builds
 * them (u32_to_str()/copy_str(), lib/fmt) since their fields differ
 * between callers. `samples`/`n` are the payload; `n == 0` sends the
 * bare "CRC 0000\r\n" failure tail and nothing else. The CRC is folded
 * into the running value one chunk at a time, before that chunk is handed
 * to write() - so it always covers exactly what ends up counted in the
 * return value, whether the transfer completes or is cut short.
 *
 * Returns the number of payload bytes actually written (2*n on a clean
 * transfer, less if `aborted` cut it short) - the caller's own
 * "sent != 2*n" check is what decides whether the command reports
 * failure, exactly as cmd_blk_fn()/cmd_stream_grab() did. */
uint32_t frame_send(frame_write_fn write, frame_aborted_fn aborted,
                     const char *header, size_t header_len,
                     const volatile uint16_t *samples, uint32_t n);

#endif /* FRAME_H */
