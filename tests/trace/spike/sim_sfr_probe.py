#!/usr/bin/env python3
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

"""sim_sfr_probe.py - P0.3 spike, step 4 (the question P0.8 depends on):
does the MPLAB X simulator keep what the firmware writes to SFRs of
peripherals it does not model (PLL, clock generators, ADC)?

Programs the existing simulator build, lets it boot for a few seconds
(clock_init() and adc_init() run in the sim build; only their waits are
compiled out), halts, and prints the registers with the values the code
writes. Reuses the Mdb driver of tools/sim_trap.py. NOT the acceptance
run: a short boot only. Run under a hard timeout:
    timeout 120 python tests/trace/spike/sim_sfr_probe.py
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import sim_trap  # noqa: E402

# register -> value the firmware writes (clock.c / timebase.c), for the report
EXPECT = {
    "PLL1DIV": "0x0100C829 (clock.c)",
    "VCO1DIV": "0x00020000 (clock.c)",
    "PLL2DIV": "0x01007D29 (clock.c)",
    "CLK6CON": "0x00029500 | status bits (clock.c)",
    "PR1":     "0xFFFFFFFF (timebase.c)",
    "T1CON":   "0x00008010 (timebase.c)",
}
RUN_SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0


def main():
    elf = os.path.join(ROOT, "build", "adc_dma_40msps_sim.elf")
    bat = sim_trap.find_mdb()
    if not bat or not os.path.exists(elf):
        sys.exit("missing mdb or %s" % elf)
    t0 = time.time()
    m = sim_trap.Mdb(bat, os.environ.get("PROBE_V") == "1")
    try:
        m.cmd("Device %s" % sim_trap.DEVICE, 3)
        # the same simulator settings as sim_trap.py (without them the
        # first attempt answered no Print after Halt, W0101-SIM NPE)
        m.cmd("Set uart2io.uartioenabled true")
        m.cmd("Set uart2io.output file")
        m.cmd("Set uart2io.outputfile %s" % os.path.join(ROOT, "build", "sim_sfr_probe.uart2.txt"))
        m.cmd("Set oscillator.frequency 8")
        m.cmd("Set oscillator.frequencyunit Mega")
        m.cmd("Hwtool SIM", 2)
        if not m.wait_for(r">|Resetting", 60):
            sys.exit("simulator did not come up")
        m.cmd('Program "%s"' % elf, 2)
        if not m.wait_for(r"Program succeeded", 60):
            sys.exit("programming failed:\n" + "\n".join(m.lines[-10:]))
        print("programmed after %.0f s; before Run:" % (time.time() - t0))
        for l in m.print_symbols(list(EXPECT)):
            print("   ", l)
        m.cmd("Run", 1)
        time.sleep(RUN_SECONDS)
        m.cmd("Halt", 5)
        print("after %.0f s Run:" % RUN_SECONDS)
        for l in m.print_symbols(["boot_stage", "fail_code"] + list(EXPECT)):
            name = l.split()[0] if l.split() else ""
            print("   ", l, ("   <- firmware writes " + EXPECT[name]) if name in EXPECT else "")
    finally:
        m.quit()
    print("total %.0f s" % (time.time() - t0))


if __name__ == "__main__":
    main()
