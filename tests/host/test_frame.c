/*
 * test_frame.c - host-side test for lib/frame.c (frame_send()), the binary
 * frame "blk" and "stream grab" both send.
 *
 * Built by tools\hosttest.bat with the installed MinGW gcc, against the
 * real src/lib/frame.c, src/lib/crc16.c and src/lib/fmt.c - not a
 * reimplementation. The two header builders below (build_bin_header(),
 * build_grab_header()) mirror cli.c's cmd_blk_fn()/cmd_stream_grab() and
 * what src/link/gui_link.c builds after P6.4 - same copy_str()/u32_to_str()
 * calls, same field order, same "\r\n" terminator - so a frame built here
 * is the same shape a colleague reading cli.c/gui_link.c would expect.
 *
 * Two jobs:
 *
 * 1. Without arguments: checks against a memory-buffer write() sink -
 *    header + payload + CRC tail bytes for a clean transfer, the bare
 *    "CRC 0000\r\n" shape for n == 0, the chunk boundary at FRAME_CHUNK
 *    (32 samples/64 bytes per write() call, a shorter last chunk), and
 *    that `aborted` is polled only between chunks (an abort flagged after
 *    the first chunk stops the transfer with exactly that chunk sent, not
 *    zero and not the whole window) - the placement cli.c had before P6.3.
 *
 * 2. `test_frame --dump-blk <path>` / `--dump-grab <path>`: writes one
 *    complete frame's raw bytes (header, binary payload, CRC line) to a
 *    file, binary, no translation - what tests/host/test_frame_xcheck.py
 *    (P6.3) drives to prove tools/protocol.py can parse what frame.c
 *    produces, with the same CRC and the payload round-tripping exactly.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "frame.h"
#include "crc16.h"
#include "fmt.h"
#include "check.h"

/* ------------------------------------------------------------------ *
 * Header builders - the exact shape cli.c's cmd_blk_fn()/cmd_stream_grab()
 * use today (docs/PLAN-BINARY-TRANSFER.md), reproduced here so this test
 * does not need to link cli.c (which pulls in the hardware drivers).
 * ------------------------------------------------------------------ */
static size_t build_bin_header(char *out, uint32_t n, uint32_t p1, uint32_t p2,
                                uint32_t samc, uint32_t pinsel)
{
    char *p = copy_str(out, "BIN n=");
    p = u32_to_str(p, n);
    p = copy_str(p, " p1=");   p = u32_to_str(p, p1);
    p = copy_str(p, " p2=");   p = u32_to_str(p, p2);
    p = copy_str(p, " samc="); p = u32_to_str(p, samc);
    p = copy_str(p, " in=");   p = u32_to_str(p, pinsel);
    p = copy_str(p, "\r\n");
    return (size_t)(p - out);
}

static size_t build_grab_header(char *out, uint32_t n, uint32_t from, uint32_t ksps,
                                 uint32_t ov, uint32_t late, uint32_t missed,
                                 uint32_t halves, uint32_t xfer, uint32_t slp,
                                 uint32_t dachz)
{
    char *p = copy_str(out, "GRAB n=");
    p = u32_to_str(p, n);
    p = copy_str(p, " from=");   p = u32_to_str(p, from);
    p = copy_str(p, " ksps=");   p = u32_to_str(p, ksps);
    p = copy_str(p, " ov=");     p = u32_to_str(p, ov);
    p = copy_str(p, " late=");   p = u32_to_str(p, late);
    p = copy_str(p, " missed="); p = u32_to_str(p, missed);
    p = copy_str(p, " halves="); p = u32_to_str(p, halves);
    p = copy_str(p, " xfer=");   p = u32_to_str(p, xfer);
    p = copy_str(p, " slp=");    p = u32_to_str(p, slp);
    p = copy_str(p, " dachz="); p = u32_to_str(p, dachz);
    p = copy_str(p, "\r\n");
    return (size_t)(p - out);
}

/* ------------------------------------------------------------------ *
 * A memory-buffer write() sink for the default (argument-less) checks.
 * ------------------------------------------------------------------ */
#define MEMBUF_MAX 8192u
static uint8_t  membuf[MEMBUF_MAX];
static uint32_t membuf_len;
static uint32_t chunk_sizes[64];
static uint32_t chunk_count;

static void membuf_reset(void)
{
    membuf_len = 0u;
    chunk_count = 0u;
}

static size_t membuf_write(const uint8_t *data, size_t len)
{
    if ((membuf_len + len) > MEMBUF_MAX) { len = MEMBUF_MAX - membuf_len; }
    for (size_t i = 0; i < len; i++) { membuf[membuf_len++] = data[i]; }
    if (chunk_count < (sizeof chunk_sizes / sizeof chunk_sizes[0])) {
        chunk_sizes[chunk_count++] = (uint32_t)len;
    }
    return len;
}

/* Aborts after `abort_after_chunks` writes to the payload/tail sink -
 * frame_send() only asks between payload chunks, matching cli.c's old
 * placement (never mid-chunk, never around the header or CRC line). */
static uint32_t abort_after_chunks;
static uint32_t writes_seen;

static bool abort_after_n(void)
{
    return writes_seen >= abort_after_chunks;
}

static size_t membuf_write_countaborts(const uint8_t *data, size_t len)
{
    const size_t sent = membuf_write(data, len);
    writes_seen++;
    return sent;
}

/* ------------------------------------------------------------------ *
 * Default run: the checks
 * ------------------------------------------------------------------ */
static void test_clean_frame(void)
{
    enum { N = 5 };
    static const uint16_t samples[N] = { 100, 200, 4095, 0, 2048 };

    char header[96];
    const size_t hlen = build_bin_header(header, N, 5, 5, 10, 3);

    membuf_reset();
    const uint32_t sent = frame_send(membuf_write, NULL, header, hlen, samples, N);

    CHECK_EQ(sent, 2u * N);
    CHECK_EQ(membuf_len, hlen + (2u * N) + 12u);
    CHECK(memcmp(membuf, header, hlen) == 0);

    /* Payload: low byte, then high nibble, little endian. */
    const uint8_t *payload = membuf + hlen;
    for (int i = 0; i < N; i++) {
        CHECK_EQ(payload[2 * i],     samples[i] & 0xFFu);
        CHECK_EQ(payload[2 * i + 1], (samples[i] >> 8) & 0x0Fu);
    }

    /* CRC tail, independently recomputed. */
    const uint16_t crc = crc16_ccitt_false(CRC16_INIT, payload, 2u * N);
    char want_tail[16];
    snprintf(want_tail, sizeof want_tail, "\r\nCRC %04X\r\n", crc);
    CHECK(memcmp(membuf + hlen + (2u * N), want_tail, 12u) == 0);
}

static void test_zero_frame(void)
{
    char header[96];
    const size_t hlen = build_grab_header(header, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

    membuf_reset();
    const uint32_t sent = frame_send(membuf_write, NULL, header, hlen, NULL, 0u);

    CHECK_EQ(sent, 0u);
    CHECK_EQ(membuf_len, hlen + 10u);   /* "CRC 0000\r\n" = 10 bytes */
    CHECK(memcmp(membuf, header, hlen) == 0);
    CHECK(memcmp(membuf + hlen, "CRC 0000\r\n", 10u) == 0);
}

static void test_chunk_boundary(void)
{
    /* 37 samples: FRAME_CHUNK (64) - 1 = 63 bytes fit 31 whole samples
     * before the loop guard trips on the 32nd - so the first payload
     * write is exactly 32 samples/64 bytes, the second the remaining
     * 5 samples/10 bytes. Same arithmetic cli.c's BLK_CHUNK loop had. */
    enum { N = 37 };
    uint16_t samples[N];
    for (int i = 0; i < N; i++) { samples[i] = (uint16_t)((i * 37 + 5) % 4096); }

    char header[16];
    const size_t hlen = build_bin_header(header, N, 1, 1, 0, 0);

    membuf_reset();
    const uint32_t sent = frame_send(membuf_write, NULL, header, hlen, samples, N);

    CHECK_EQ(sent, 2u * N);
    /* chunk_sizes[0] is the header write, then the payload writes, then
     * the tail write. */
    CHECK_EQ(chunk_count, 4u);
    CHECK_EQ(chunk_sizes[0], hlen);
    CHECK_EQ(chunk_sizes[1], 64u);
    CHECK_EQ(chunk_sizes[2], (2u * N) - 64u);
    CHECK_EQ(chunk_sizes[3], 12u);
}

static void test_abort_after_first_chunk(void)
{
    /* 100 samples = 200 payload bytes, chunked at 32 samples/64 bytes:
     * 64, 64, 64, 8. Abort after the FIRST payload write (writes_seen
     * counts the header write too, so 2 total: header + first chunk) must
     * stop before the second chunk - sent covers exactly that one 64-byte
     * chunk, not the whole window, and the CRC tail is still appended
     * (frame_send() always finishes the frame; only the payload loop can
     * be cut short, exactly where cli.c checked cmd_parser_aborted()). */
    enum { N = 100 };
    uint16_t samples[N];
    for (int i = 0; i < N; i++) { samples[i] = (uint16_t)((i * 13 + 1) % 4096); }

    char header[16];
    const size_t hlen = build_bin_header(header, N, 1, 1, 0, 0);

    membuf_reset();
    writes_seen = 0u;
    abort_after_chunks = 2u;   /* header write (1) + first payload chunk (2) */
    const uint32_t sent = frame_send(membuf_write_countaborts, abort_after_n,
                                      header, hlen, samples, N);

    CHECK_EQ(sent, 64u);
    CHECK(sent != (2u * N));
    /* header, one payload chunk, the CRC tail - frame_send() always sends
     * the tail once the payload loop ends, aborted or not. */
    CHECK_EQ(chunk_count, 3u);
    CHECK_EQ(chunk_sizes[1], 64u);
}

int main(int argc, char **argv)
{
    if ((argc == 3) && ((strcmp(argv[1], "--dump-blk") == 0) ||
                         (strcmp(argv[1], "--dump-grab") == 0))) {
        const bool is_grab = (strcmp(argv[1], "--dump-grab") == 0);
        enum { N = 100 };
        uint16_t samples[N];
        for (int i = 0; i < N; i++) { samples[i] = (uint16_t)((i * 37 + 5) % 4096); }

        char header[128];
        size_t hlen;
        if (is_grab) {
            hlen = build_grab_header(header, N, /*from=*/1024, /*ksps=*/8000,
                                      /*ov=*/3, /*late=*/1, /*missed=*/0,
                                      /*halves=*/250, /*xfer=*/2000000,
                                      /*slp=*/18, /*dachz=*/400000000);
        } else {
            hlen = build_bin_header(header, N, /*p1=*/5, /*p2=*/5,
                                     /*samc=*/10, /*pinsel=*/3);
        }

        FILE *f = fopen(argv[2], "wb");
        if (f == NULL) { fprintf(stderr, "cannot open %s for writing\n", argv[2]); return 1; }

        membuf_reset();
        (void)frame_send(membuf_write, NULL, header, hlen, samples, N);
        const size_t written = fwrite(membuf, 1, membuf_len, f);
        fclose(f);
        if (written != membuf_len) { fprintf(stderr, "short write to %s\n", argv[2]); return 1; }
        return 0;
    }

    test_clean_frame();
    test_zero_frame();
    test_chunk_boundary();
    test_abort_after_first_chunk();
    return check_summary();
}
