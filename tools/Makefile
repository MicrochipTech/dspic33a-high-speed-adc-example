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
SRC      = ../main.c ../config_bits.c ../clock.c ../adc.c ../dma.c ../capture.c ../led.c ../diag.c ../timebase.c ../cli.c ../cmd_parser.c
HDR      = ../board.h ../clock.h ../adc.h ../dma.h ../capture.h ../led.h ../diag.h ../console.h ../sim.h ../timebase.h ../cmd_parser.h
SRC_SIM  = $(subst ../dma.c,../sim_dma.c,$(SRC))
OUTDIR   = ../build

# -mdfp must point at the xc16 SUBDIRECTORY of the pack, not the pack root.
# Pointing at the root gives "does not seem to support the selected device",
# because c30_device.info lives one level down.
CFLAGS   = -mcpu=$(MCU) -mdfp="$(DFP)" -O1 -Wall -Wextra
LDFLAGS  = -T"$(GLD)"

.PHONY: all hex sim size clean

all: $(OUTDIR)/$(TARGET).elf

$(OUTDIR):
	mkdir -p $(OUTDIR)

$(OUTDIR)/$(TARGET).elf: $(SRC) $(HDR) | $(OUTDIR)
	"$(CC)" $(CFLAGS) $(LDFLAGS) $(SRC) -o $@

sim: $(OUTDIR)/$(TARGET)_sim.elf

$(OUTDIR)/$(TARGET)_sim.elf: $(SRC_SIM) $(HDR) | $(OUTDIR)
	"$(CC)" $(CFLAGS) -D__MPLAB_DEBUGGER_SIMULATOR=1 -g $(LDFLAGS) $(SRC_SIM) -o $@

hex: $(OUTDIR)/$(TARGET).elf
	"$(BIN2HEX)" $(OUTDIR)/$(TARGET).elf

size: $(OUTDIR)/$(TARGET).elf
	"$(OBJDUMP)" -h $(OUTDIR)/$(TARGET).elf

clean:
	rm -f $(OUTDIR)/$(TARGET).elf $(OUTDIR)/$(TARGET).hex $(OUTDIR)/$(TARGET)_sim.elf
