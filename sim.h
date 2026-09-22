/*
 * sim.h - hooks for the MPLAB X simulator build
 *
 * The simulator has no PLL, no ADC conversion, no DMA transfer and does
 * not dispatch interrupts in this project (tools/sim_trap.py). The
 * simulator build therefore compiles sim_dma.c instead of dma.c: a
 * stand-in that produces buffer halves on request, through the same
 * dma0_event() path the DMA interrupt uses. These macros are where the
 * firmware asks for the next half and where it lets the stand-in check
 * what the consumer received. On silicon every one of them is empty.
 *
 * __MPLAB_DEBUGGER_SIMULATOR is what MPLAB X defines for a Simulator
 * configuration; build.bat sim and the "sim" project configuration set
 * it explicitly.
 */
#ifndef SIM_H
#define SIM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __MPLAB_DEBUGGER_SIMULATOR

/* Deliver one buffer half (1 MHz sine, or 3840 flat on the self-test
 * input) if a burst is running. Called wherever the code would
 * otherwise wait for the DMA. */
void sim_dma_tick(void);

/* Ping-pong check: every half handed to process_buffer() is compared
 * with the sine vector and must continue the phase of the previous
 * half. Runs over the first 100 sine halves, prints [simtest] PASS or
 * FAIL, then stops. While it runs, main() holds the status lines back,
 * because every console character costs the simulator about 0.1 s. */
void sim_check_half(const volatile uint16_t *b, uint32_t n);
bool sim_check_running(void);

#define SIM_DMA_TICK()        sim_dma_tick()
#define SIM_CHECK_HALF(b, n)  sim_check_half((b), (n))
#define SIM_CHECK_RUNNING()   sim_check_running()

#else

#define SIM_DMA_TICK()        do { } while (0)
#define SIM_CHECK_HALF(b, n)  do { (void)(b); (void)(n); } while (0)
#define SIM_CHECK_RUNNING()   false

#endif

#endif /* SIM_H */
