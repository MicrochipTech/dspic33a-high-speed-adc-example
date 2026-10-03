# Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries.
#
# Subject to your compliance with these terms, you may use Microchip software
# and any derivatives exclusively with Microchip products. It is your
# responsibility to comply with third party license terms applicable to your
# use of third party software (including open source software) that may
# accompany Microchip software.
#
# THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
# EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
# WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
# PARTICULAR PURPOSE.
#
# IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
# INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
# WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
# BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
# FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
# ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
# THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.

"""Oscilloscope-style trigger for the GUI's time plot (TRG.1,
docs/IMPLEMENTATION-PLAN.md section TRG).

Pure functions, no NiceGUI: `adc_gui.py` calls them on the half a grab
has just delivered, and its --selftest checks them without a page.

Design (IMPLEMENTATION-PLAN.md, TRG decision 1): the display is a fixed
window of L samples, x[k..k+L), with the trigger searched only in
x[0..N-L] - contiguous by construction. The half is never rotated: a
wrap from x[N-1] back to x[0] would be a seam N samples wide in time,
which the triangle evaluator reads as a lost sample and the FFT as a
discontinuity.
"""

RISING = "rising"
FALLING = "falling"


def find_triggers(samples, level, slope=RISING, hyst=16, search_end=None):
    """Yield every crossing as (k, frac), in order.

    Rising: armed once a sample is below `level - hyst`, fires at the first
    sample >= `level` after that, then re-arms. Falling mirrored (armed
    above `level + hyst`, fires at the first sample <= `level`). The
    hysteresis keeps a few LSB of ADC noise on a slow slope from firing
    more than once.

    `k` is the first sample at/past the level (always >= 1: the sample
    that armed the search comes before it); `frac` in [0, 1] is where the
    level lies between x[k-1] and x[k], linearly interpolated, so the
    crossing itself is at k - 1 + frac. Only k <= `search_end` is reported
    (default: the last sample)."""
    n = len(samples)
    last = n - 1 if search_end is None else min(int(search_end), n - 1)
    rising = slope != FALLING
    arm = level - hyst if rising else level + hyst
    armed = False
    for k in range(last + 1):
        v = samples[k]
        if not armed:
            armed = v < arm if rising else v > arm
            continue
        if (v >= level) if rising else (v <= level):
            prev = samples[k - 1]
            d = float(v) - float(prev)
            frac = (float(level) - float(prev)) / d if d else 1.0
            yield k, min(max(frac, 0.0), 1.0)
            armed = False


def find_trigger(samples, level, slope=RISING, hyst=16, search_end=None):
    """The first crossing as (k, frac), or None when there is none."""
    return next(find_triggers(samples, level, slope, hyst, search_end), None)


def trigger_window(samples, level, slope=RISING, hyst=16, length=None):
    """What the time plot shows: (start, frac, found).

    `length` = L, default N // 2. The search covers x[0..N-L], so
    x[start..start+L) always lies inside the half. When nothing is found
    the oscilloscope's "auto" behaviour applies: start 0, frac 0.0,
    found False - the untriggered first L samples, no error."""
    n = len(samples)
    L = n // 2 if length is None else int(length)
    L = max(1, min(L, n))
    hit = find_trigger(samples, level, slope, hyst, search_end=n - L)
    if hit is None:
        return 0, 0.0, False
    return hit[0], hit[1], True
