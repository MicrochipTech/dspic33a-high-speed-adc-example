/* libpic30.h - P0.4 register-trace harness stand-in. __delay32() is
 * implemented in stubs.c: a `D` trace line, and it advances the TMR1
 * model by cycles/16 (200 MHz CPU, 12.5 MHz Timer1), so timebase_check()
 * returns a deterministic value without actually waiting. */
#ifndef LIBPIC30_H
#define LIBPIC30_H
void __delay32(unsigned long cycles);
#endif
