/* libpic30.h - P0.3 spike stand-in. __delay32() goes into the trace and
 * advances TMR1 by cycles/16 (200 MHz CPU, 12.5 MHz Timer1), so that
 * timebase_check() returns a deterministic value. recorder.c. */
#ifndef LIBPIC30_H
#define LIBPIC30_H
void __delay32(unsigned long cycles);
#endif
