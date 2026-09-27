/*
 * detect.c - pulse detector on a Goertzel magnitude stream (see detect.h)
 */

#include "detect.h"

static int32_t rearm_level(int32_t threshold, uint32_t hyst_q16)
{
    /* threshold is a magnitude, so non-negative in practice; int64 keeps
     * the product safe for any int32 threshold */
    return (int32_t)(((int64_t)threshold * (int64_t)hyst_q16) >> 16);
}

void detect_init(detect_t *d, int32_t threshold, uint32_t hyst_q16, uint32_t window)
{
    d->threshold = threshold;
    d->hyst_q16 = hyst_q16;
    d->rearm = rearm_level(threshold, hyst_q16);
    d->window = (window == 0u) ? 1u : window;
    detect_reset(d);
}

void detect_set_threshold(detect_t *d, int32_t threshold)
{
    d->threshold = threshold;
    d->rearm = rearm_level(threshold, d->hyst_q16);
}

void detect_reset(detect_t *d)
{
    d->win_cnt = 0u;
    d->counter = 0u;
    d->max_amplitude = 0;
    d->armed = true;
}

bool detect_sample(detect_t *d, int32_t mag)
{
    if (++d->win_cnt < d->window) { return false; }
    d->win_cnt = 0u;

    if (d->armed) {
        if (mag > d->threshold) {
            d->counter++;
            d->armed = false;
            return true;
        }
    } else if (mag < d->rearm) {
        d->armed = true;
    }
    return false;
}

uint32_t detect_block(detect_t *d, const int32_t *mag, uint32_t n)
{
    uint32_t fired = 0u;
    for (uint32_t i = 0; i < n; i++) {
        if (detect_sample(d, mag[i])) { fired++; }
    }
    return fired;
}

void detect_amplitude(detect_t *d, const uint16_t *x, uint32_t n, uint8_t in_shift)
{
    int32_t mx = d->max_amplitude;
    for (uint32_t i = 0; i < n; i++) {
        const int32_t s = (int32_t)x[i] >> in_shift;       /* template line 44 */
        if (s > mx) { mx = s; }                             /* line 47 */
    }
    d->max_amplitude = mx;
}

int32_t detect_adapt(detect_t *d, uint32_t scale_q16)
{
    /* main.c 206: (max_amplitude * (int32_t)(SCALE * 65536)) >> 16 */
    const int32_t t = (int32_t)(((int64_t)d->max_amplitude * (int64_t)scale_q16) >> 16);
    detect_set_threshold(d, t);
    d->max_amplitude = 0;                                   /* main.c 208 */
    d->counter = 0u;                                        /* main.c 209 */
    return t;
}
