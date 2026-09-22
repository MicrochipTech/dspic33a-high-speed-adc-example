@echo off
rem ---------------------------------------------------------------------
rem  dsPIC33AK512MPS506 ADC/DMA demo - build without MPLAB X or MCC
rem
rem  Just run:  build.bat
rem
rem  Verified with the versions below on 2026-09-21. Adjust the two paths
rem  if your installation differs; nothing else needs to change.
rem ---------------------------------------------------------------------

setlocal

set XC_DSC=C:\Program Files\Microchip\xc-dsc\v3.31
set DFP=C:\Program Files\Microchip\MPLABX\v6.35\packs\Microchip\dsPIC33AK-MP_DFP\1.4.260\xc16

set MCU=33AK512MPS506
set TARGET=adc_dma_40msps

rem NOTE: -mdfp must point at the xc16 SUBDIRECTORY of the pack, not the
rem pack root. The root gives "does not seem to support the selected
rem device" because c30_device.info lives one level down.

"%XC_DSC%\bin\xc-dsc-gcc.exe" ^
  -mcpu=%MCU% ^
  -mdfp="%DFP%" ^
  -O1 -Wall -Wextra ^
  -T"%DFP%\support\dsPIC33A\gld\p%MCU%.gld" ^
  "..\%TARGET%.c" -o %TARGET%.elf

if errorlevel 1 (
  echo.
  echo BUILD FAILED
  exit /b 1
)

echo.
echo Build OK: %TARGET%.elf
"%XC_DSC%\bin\xc-dsc-bin2hex.exe" %TARGET%.elf
if exist %TARGET%.hex echo HEX written: %TARGET%.hex

endlocal
