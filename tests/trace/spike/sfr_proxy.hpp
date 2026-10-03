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

/* sfr_proxy.hpp - P0.3 spike, approach (b): every SFR and every bit field
 * a C++ object whose conversion and assignment operators log the access.
 * Storage is the same sfr_mem[] as in C; the logging functions are the
 * recorder's (not linked in the spike - compile-only check). */
#ifndef SFR_PROXY_HPP
#define SFR_PROXY_HPP

#include <stdint.h>

#define __builtin_nop()      ((void)0)
#define __builtin_clrwdt()   ((void)0)
#define Nop()                ((void)0)
#define ClrWdt()             ((void)0)
#define interrupt    __unused__
#define no_auto_psv  __unused__
#define persistent   __unused__

extern volatile uint32_t sfr_mem[];
extern "C" void proxy_read(unsigned idx);
extern "C" void proxy_write(unsigned idx, uint32_t old, uint32_t now);

template <unsigned I> struct Reg {
    operator uint32_t() const { proxy_read(I); return sfr_mem[I]; }
    Reg &operator=(uint32_t v) { proxy_write(I, sfr_mem[I], v); sfr_mem[I] = v; return *this; }
    Reg &operator|=(uint32_t m) { return *this = (uint32_t)*this | m; }
    Reg &operator&=(uint32_t m) { return *this = (uint32_t)*this & m; }
    Reg &operator^=(uint32_t m) { return *this = (uint32_t)*this ^ m; }
    Reg &operator+=(uint32_t m) { return *this = (uint32_t)*this + m; }
    Reg &operator-=(uint32_t m) { return *this = (uint32_t)*this - m; }
    /* `&X` must give the device address's stand-in, not the proxy's. */
    volatile uint32_t *operator&() const { return &sfr_mem[I]; }
};

template <unsigned I, unsigned P, unsigned L> struct Field {
    static constexpr uint32_t M = (L >= 32u ? 0xFFFFFFFFu : ((1u << L) - 1u)) << P;
    operator uint32_t() const { proxy_read(I); return (sfr_mem[I] & M) >> P; }
    Field &operator=(uint32_t v) {
        uint32_t n = (sfr_mem[I] & ~M) | ((v << P) & M);
        proxy_write(I, sfr_mem[I], n);
        sfr_mem[I] = n;
        return *this;
    }
};

#endif
