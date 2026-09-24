@echo off
rem ---------------------------------------------------------------------
rem  dsPIC33AK512MPS512 (EV74H48A) ADC/DMA demo - build without MPLAB X
rem
rem    build.bat          firmware for the board -> ..\build\adc_dma_40msps.elf/.hex
rem    build.bat sim      simulator build        -> ..\build\adc_dma_40msps_sim.elf
rem
rem  The simulator build compiles sim_dma.c instead of dma.c, defines
rem  __MPLAB_DEBUGGER_SIMULATOR (as MPLAB X does for a Simulator
rem  configuration) and keeps debug symbols for tools\sim_trap.py.
rem
rem  Verified with the versions below on 2026-09-22. Adjust the two paths
rem  if your installation differs; nothing else needs to change.
rem ---------------------------------------------------------------------

setlocal

rem Git revision for the banner -> ..ersion.h (writes "unknown" without git)
call "%~dp0version.bat"

set XC_DSC=C:\Program Files\Microchip\xc-dsc\v3.31
set DFP=C:\Program Files\Microchip\MPLABX\v6.35\packs\Microchip\dsPIC33AK-MP_DFP\1.4.260\xc16

set MCU=33AK512MPS512
set TARGET=adc_dma_40msps
set DMA=..\dma.c
set EXTRA=
set OUT=..\build\%TARGET%

if /i "%1"=="sim" (
  set DMA=..\sim_dma.c
  set EXTRA=-D__MPLAB_DEBUGGER_SIMULATOR=1 -g
  set OUT=..\build\%TARGET%_sim
)
rem  build.bat nano   the dsPIC33AK512MPS506 Curiosity Nano (EV17P63A): other
rem                   device, other pins (BOARD in board.h) -> ..\build\adc_dma_40msps_nano.elf/.hex
if /i "%1"=="nano" (
  set MCU=33AK512MPS506
  set EXTRA=-DBOARD=2
  set OUT=..\build\%TARGET%_nano
)
set SOURCES=..\main.c ..\config_bits.c ..\clock.c ..\adc.c %DMA% ..\capture.c ..\led.c ..\diag.c ..\timebase.c ..\sccp.c ..\dac.c ..\dactest.c ..\cli.c ..\cmd_parser.c

if not exist ..\build mkdir ..\build

rem NOTE: -mdfp must point at the xc16 SUBDIRECTORY of the pack, not the
rem pack root. The root gives "does not seem to support the selected
rem device" because c30_device.info lives one level down.

"%XC_DSC%\bin\xc-dsc-gcc.exe" ^
  -mcpu=%MCU% ^
  -mdfp="%DFP%" ^
  -O1 -Wall -Wextra %EXTRA% ^
  -T"%DFP%\support\dsPIC33A\gld\p%MCU%.gld" ^
  %SOURCES% -o %OUT%.elf

if errorlevel 1 (
  echo.
  echo BUILD FAILED
  exit /b 1
)

echo.
echo Build OK: %OUT%.elf
if /i "%1"=="sim" goto :done
rem NOTE: bin2hex needs -mdfp too. Without it the HEX is still written, but
rem it prints "Could not open resource file ... c30_device.info / Please
rem specify the location of a DFP" and looks like a failed build.
"%XC_DSC%\bin\xc-dsc-bin2hex.exe" -mdfp="%DFP%" %OUT%.elf
if exist %OUT%.hex echo HEX written: %OUT%.hex

:done
endlocal
