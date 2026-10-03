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
 * disi.h - write the interrupt-disable threshold (DISICTL, DS70005591D
 * 10.10.1.1, p596: interrupt requests at an IPL at or below DISIIPL are held
 * off). Read it back through the DISIIPL bit field in <xc.h>.
 *
 * Why not __builtin_write_DISICTL(): with a variable argument it stops
 * xc-dsc v3.21 and v3.31 at -O0 with "internal compiler error: in
 * emit_move_insn, at expr.c:3810" (02.10.2026, the MPLAB X project builds at
 * -O0; tools\build.bat at -O1 never saw it). This is the one instruction the
 * builtin produces at -O1 ("disictl wN"), checked to be the same code there.
 */
#ifndef DISI_H
#define DISI_H

#include <stdint.h>

static inline void disi_set(uint32_t ipl)
{
    __asm__ volatile ("disictl %0" : : "r"(ipl) : "memory");
}

#endif /* DISI_H */
