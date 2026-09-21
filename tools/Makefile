# dsPIC33AK512MPS512 ADC/DMA demo - build without MPLAB X or MCC
#
# Usage:   make            build the ELF
#          make hex        build an Intel HEX for the programmer
#          make size       section sizes
#          make clean
#
# Verified working with the versions below on 2026-09-21. If your compiler
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
SRC      = ../$(TARGET).c

# -mdfp must point at the xc16 SUBDIRECTORY of the pack, not the pack root.
# Pointing at the root gives "does not seem to support the selected device",
# because c30_device.info lives one level down.
CFLAGS   = -mcpu=$(MCU) -mdfp="$(DFP)" -O1 -Wall -Wextra
LDFLAGS  = -T"$(GLD)"

.PHONY: all hex size clean

all: $(TARGET).elf

$(TARGET).elf: $(SRC)
	"$(CC)" $(CFLAGS) $(LDFLAGS) $(SRC) -o $@

hex: $(TARGET).elf
	"$(BIN2HEX)" $(TARGET).elf

size: $(TARGET).elf
	"$(OBJDUMP)" -h $(TARGET).elf

clean:
	rm -f $(TARGET).elf $(TARGET).hex
