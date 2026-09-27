/*
 * iir1.c - first-order IIR low-pass / high-pass, one instance per filter
 *          (see iir1.h)
 *
 * The two step functions are the template's iFLT_IIR1_Lowpass() and
 * iFLT_IIR1_Highpass() (Goertzel/goertzel/firmware/src/goertzel.c, lines
 * 169-234) with the global tap array replaced by the instance and the
 * constant shift by f->k. Nothing else. The template compiles them with
 * __attribute__((optimize("-O1"))); the firmware builds at -O1 anyway.
 */

#include "iir1.h"

void iir1_init(iir1_t *f, uint8_t k)
{
    f->k = k;
    f->tap = 0;
}

void iir1_reset(iir1_t *f)
{
    f->tap = 0;
}

int32_t iir1_lp(iir1_t *f, int32_t x)
{
    int32_t tap = f->tap;
    tap = tap - (tap >> f->k) + x;
    f->tap = tap;
    return tap >> f->k;
}

int32_t iir1_hp(iir1_t *f, int32_t x)
{
    int32_t tap = f->tap;
    tap = tap - (tap >> f->k) + x;
    f->tap = tap;
    return x - (tap >> f->k);
}
