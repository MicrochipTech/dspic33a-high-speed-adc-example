/*
 * sigproc.c - the body of the signal processing, called once per completed
 * half of the ping-pong buffer (ping and pong) while "sigproc on" is set.
 * See sigproc.h for when it is called, how much time it has, what it must
 * not do, and why the result goes back into the same half.
 */
#include "sigproc.h"

void sigproc_block(uint16_t *x, uint32_t n, const sigproc_info_t *info)
{
    (void)x;
    (void)n;
    (void)info;

    /* ---- the signal processing goes here ----------------------------
     *
     * x[0] .. x[n - 1] are the samples of the half that just completed,
     * oldest first; info->half says which half (0 = ping, 1 = pong),
     * info->seq which block it is. Write the result back into x[]: that
     * is what "stream grab" sends to the GUI.
     *
     * Ready-made building blocks, all in every build (src/lib/):
     * iir1.h (first-order low/high pass), goertzel_f.h / goertzel_i.h
     * (one frequency bin, float or Q16), detect.h (pulse detector with
     * hysteresis), stats.h (min/max/mean).
     *
     * State that must carry over from one block to the next (a filter's
     * memory, for example) goes into a static here.
     * ----------------------------------------------------------------- */
}
