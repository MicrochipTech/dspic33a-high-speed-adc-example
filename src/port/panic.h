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
 * panic.h - the port layer's abort (V2 of docs/REFACTORING-PROPOSAL.md)
 *
 * A driver under src/drivers/ that hits a state it cannot go on from
 * calls port_panic() with its stop code and nothing else - not fail()
 * (diag.h), which is this project's implementation of it. The codes stay
 * the ones fail()'s table in diag.c lists (and docs/TROUBLESHOOTING.md),
 * so LED0's blink count and the "[FAIL] code" line do not change with the
 * indirection. src/app/port_impl.c maps it to fail(); in the trace
 * harness the same file links against the harness's fail() stub, which
 * ends the scenario step with a longjmp() as before.
 */
#ifndef PORT_PANIC_H
#define PORT_PANIC_H

#include <stdint.h>

/* Stop for good with a code from fail()'s table; never returns. */
void port_panic(uint32_t code) __attribute__((noreturn));

#endif /* PORT_PANIC_H */
