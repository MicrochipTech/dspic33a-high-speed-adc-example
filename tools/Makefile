# dsPIC33AK512MPS512 (EV74H48A) ADC/DMA demo - build without MPLAB X or MCC
#
# Usage:   make            firmware for the board  -> ../build/adc_dma_40msps.elf
#          make hex        Intel HEX for the programmer
#          make sim        simulator build (sim_dma.c instead of dma.c) -> ../build/adc_dma_40msps_sim.elf
#          make size       section sizes
#          make clean
#
# Verified working with the versions below on 2026-09-22. If your compiler
# or pack version differs, adjust XC_DSC / DFP - nothing else should need
# to change.

XC_DSC  ?= C:/Program Files/Microchip/xc-dsc/v3.31
DFP     ?= C:/Program Files/Microchip/MPLABX/v6.35/packs/Microchip/dsPIC33AK-MP_DFP/1.4.260/xc16

MCU      = 33AK512MPS512
GLD      = $(DFP)/support/dsPIC33A/gld/p$(MCU).gld

CC       = $(XC_DSC)/bin/xc-dsc-gcc.exe
OBJDUMP  = $(XC_DSC)/bin/xc-dsc-objdump.exe
BIN2HEX  = $(XC_DSC)/bin/xc-dsc-bin2hex.exe

TARGET   = adc_dma_40msps
# The sources live under ../src/<folder>/ (P1.1, 27.09.2026); every folder is
# on the include path (INC), so the sources include each other as "name.h".
SRCDIR   = ../src
INC      = -I$(SRCDIR)/drivers -I$(SRCDIR)/app -I$(SRCDIR)/cli -I$(SRCDIR)/tests -I$(SRCDIR)/lib -I$(SRCDIR)/diag -I$(SRCDIR)/sim
SRC      = $(SRCDIR)/app/main.c $(SRCDIR)/app/config_bits.c $(SRCDIR)/drivers/clock.c $(SRCDIR)/drivers/adc.c $(SRCDIR)/drivers/dma.c $(SRCDIR)/app/capture.c $(SRCDIR)/lib/crc16.c $(SRCDIR)/lib/fmt.c $(SRCDIR)/drivers/sccp.c $(SRCDIR)/drivers/led.c $(SRCDIR)/diag/diag.c $(SRCDIR)/drivers/timebase.c $(SRCDIR)/drivers/dac.c $(SRCDIR)/tests/dactest.c $(SRCDIR)/tests/chaintest.c $(SRCDIR)/cli/cli.c $(SRCDIR)/cli/cmd_parser.c
HDR      = $(SRCDIR)/app/board.h $(SRCDIR)/drivers/clock.h $(SRCDIR)/drivers/adc.h $(SRCDIR)/drivers/dma.h $(SRCDIR)/app/capture.h $(SRCDIR)/lib/crc16.h $(SRCDIR)/lib/fmt.h $(SRCDIR)/drivers/sccp.h $(SRCDIR)/drivers/led.h $(SRCDIR)/diag/diag.h $(SRCDIR)/cli/console.h $(SRCDIR)/sim/sim.h $(SRCDIR)/drivers/timebase.h $(SRCDIR)/drivers/dac.h $(SRCDIR)/tests/dactest.h $(SRCDIR)/tests/chaintest.h $(SRCDIR)/cli/cmd_parser.h
SRC_SIM  = $(subst $(SRCDIR)/drivers/dma.c,$(SRCDIR)/sim/sim_dma.c,$(SRC))
OUTDIR   = ../build

# -mdfp must point at the xc16 SUBDIRECTORY of the pack, not the pack root.
# Pointing at the root gives "does not seem to support the selected device",
# because c30_device.info lives one level down.
CFLAGS   = -mcpu=$(MCU) -mdfp="$(DFP)" -O1 -Wall -Wextra $(INC)
LDFLAGS  = -T"$(GLD)"

.PHONY: all hex sim size clean version

all: $(OUTDIR)/$(TARGET).elf

$(OUTDIR):
	mkdir -p $(OUTDIR)

# Git revision for the banner -> ../src/app/version.h, rewritten only when it changes.
version:
	sh $(dir $(lastword $(MAKEFILE_LIST)))version.sh

$(OUTDIR)/$(TARGET).elf: $(SRC) $(HDR) version | $(OUTDIR)
	"$(CC)" $(CFLAGS) $(LDFLAGS) $(SRC) -o $@

#          make nano       dsPIC33AK512MPS506 Curiosity Nano (BOARD=2) -> ../build/adc_dma_40msps_nano.elf
MCU_NANO = 33AK512MPS506
GLD_NANO = $(DFP)/support/dsPIC33A/gld/p$(MCU_NANO).gld

nano: $(OUTDIR)/$(TARGET)_nano.elf

$(OUTDIR)/$(TARGET)_nano.elf: $(SRC) $(HDR) | $(OUTDIR)
	"$(CC)" -mcpu=$(MCU_NANO) -mdfp="$(DFP)" -O1 -Wall -Wextra $(INC) -DBOARD=2 -T"$(GLD_NANO)" $(SRC) -o $@

sim: $(OUTDIR)/$(TARGET)_sim.elf

$(OUTDIR)/$(TARGET)_sim.elf: $(SRC_SIM) $(HDR) version | $(OUTDIR)
	"$(CC)" $(CFLAGS) -D__MPLAB_DEBUGGER_SIMULATOR=1 -g $(LDFLAGS) $(SRC_SIM) -o $@

hex: $(OUTDIR)/$(TARGET).elf
	"$(BIN2HEX)" $(OUTDIR)/$(TARGET).elf

size: $(OUTDIR)/$(TARGET).elf
	"$(OBJDUMP)" -h $(OUTDIR)/$(TARGET).elf

clean:
	rm -f $(OUTDIR)/$(TARGET).elf $(OUTDIR)/$(TARGET).hex $(OUTDIR)/$(TARGET)_sim.elf
