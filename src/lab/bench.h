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
 * bench.h - the back-to-back test suite: "sweep" and "test" (bench.c)
 *
 * P6.2: test_*, matrix_* and sweep_* moved here out of cli.c verbatim,
 * together with their static state and the two commands that drive them.
 *
 * Two registration functions, not one: in the console's command list
 * "sweep" and "test" were never adjacent - cli.c's own "clk" and "pll"
 * commands sit between them - and cli_init() must still call
 * cmd_register() in exactly the old sequence, so that "help" lists the
 * same commands in the same order. bench_register_sweep() is called
 * where "sweep" used to be registered, bench_register_test() where
 * "test" used to be; cli_init() registers "clk" and "pll" itself, in
 * between, exactly as before.
 */
#ifndef BENCH_H
#define BENCH_H

void bench_register_sweep(void);
void bench_register_test(void);

#endif /* BENCH_H */
