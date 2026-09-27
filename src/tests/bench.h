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
