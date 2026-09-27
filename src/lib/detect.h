/*
 * detect.h - pulse detector on a Goertzel magnitude stream, one instance
 *            per channel (detect.c)
 *
 * Taken out of the template's filter loops on 27.09.2026 (P3.5):
 * Goertzel/goertzel/firmware/src/goertzel.c lines 47 (max_amplitude),
 * 78-87 (threshold, window, counter) and main.c lines 204-208 (the
 * adaptive threshold), reshaped by the user decision of 27.09.2026
 * (DESIGN-MULTICHANNEL.md 4.3):
 *
 *   - it FIRES ONCE PER PULSE: a consulted magnitude above `threshold`
 *     while armed counts one pulse and disarms; the detector re-arms only
 *     once a consulted magnitude has fallen below the re-arm level
 *     threshold x hyst (hyst in Q16, 32768 = 0.5). The template reset the
 *     resonator and the low-pass on a detection and counted again as
 *     soon as they had refilled - two or more counts per pulse. Nothing
 *     is reset on a detection here, so the detector needs no access to
 *     the Goertzel instance: it is a second pass over the magnitudes
 *     goertzel_f_block()/goertzel_i_block() write to mag_out;
 *   - `window`: only every window-th magnitude handed in is consulted
 *     (the template's WINDOW_SIZE role; window 1 consults every value).
 *     The Goertzel's own `window` already decimates its output, so with
 *     both at 1 every sample is judged, and goertzel window 4 + detect
 *     window 1 judges the same values as goertzel window 1 + detect
 *     window 4 (tests/host/test_detect.c proves it);
 *   - max_amplitude is the largest shifted input sample seen since the
 *     last reset or adapt (template line 47, over the INPUT, not the
 *     magnitude); detect_adapt(scale_q16) sets threshold =
 *     (max_amplitude x scale_q16) >> 16 and starts max_amplitude and the
 *     counter over (main.c 204-209; SIGNAL_SCALE_THRESHOLD 1.5 = 98304).
 *     The armed state is not touched by adapt;
 *   - no static state; detect_reset() clears one instance.
 *
 * Reference: class Detector in tests/ref/goertzel_ref.py; its re-arm
 * compare is threshold * hyst in float, here (threshold * hyst_q16) >> 16,
 * identical for hyst 0.5 and any threshold, and within one count
 * otherwise. The vectors' det column marks where it fires.
 */
#ifndef DETECT_H
#define DETECT_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int32_t  threshold;      /* fire above this */
    int32_t  rearm;          /* re-arm below this: (threshold * hyst) >> 16 */
    uint32_t hyst_q16;       /* the hysteresis fraction, Q16 */
    uint32_t window;         /* consult every window-th magnitude (>= 1) */
    uint32_t win_cnt;        /* magnitudes since the last consulted one */
    uint32_t counter;        /* pulses since reset/adapt */
    int32_t  max_amplitude;  /* largest shifted input sample since reset/adapt */
    bool     armed;          /* true: the next crossing counts */
} detect_t;

/* threshold, hysteresis in Q16 (32768 = 0.5, must be below 65536),
 * consult every window-th magnitude (0 counts as 1). Clears the state. */
void detect_init(detect_t *d, int32_t threshold, uint32_t hyst_q16, uint32_t window);

/* Set a new threshold, keep the hysteresis fraction; the re-arm level
 * follows. The armed state is kept. */
void detect_set_threshold(detect_t *d, int32_t threshold);

/* Clear counter, max_amplitude, window counter; re-arm. Keeps threshold,
 * hysteresis and window. */
void detect_reset(detect_t *d);

/* One magnitude (a value from mag_out). Returns true when it fires. */
bool detect_sample(detect_t *d, int32_t mag);

/* n magnitudes; returns how many of them fired. */
uint32_t detect_block(detect_t *d, const int32_t *mag, uint32_t n);

/* Track max_amplitude over a block of INPUT samples, shifted as the
 * Goertzel shifts them (template line 47). */
void detect_amplitude(detect_t *d, const uint16_t *x, uint32_t n, uint8_t in_shift);

/* Adaptive threshold (main.c 204-209): threshold = (max_amplitude x
 * scale_q16) >> 16, then max_amplitude and counter start over. Returns
 * the new threshold. */
int32_t detect_adapt(detect_t *d, uint32_t scale_q16);

#endif /* DETECT_H */
