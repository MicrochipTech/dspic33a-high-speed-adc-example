@echo off
rem Internal test helper: build the MPLAB X project from the command line.
rem The customer does not need this - they press Build in the IDE.
cd /d %~dp0..\adc_dma_40msps.X
for /f "delims=" %%D in ('dir /b /ad /o-n "C:\Program Files\Microchip\MPLABX\v*" 2^>nul') do (
    if not defined GEN if exist "C:\Program Files\Microchip\MPLABX\%%D\mplab_platform\bin\prjMakefilesGenerator.bat" set "GEN=C:\Program Files\Microchip\MPLABX\%%D\mplab_platform\bin\prjMakefilesGenerator.bat"
    if not defined MK if exist "C:\Program Files\Microchip\MPLABX\%%D\gnuBins\GnuWin32\bin\make.exe" set "MK=C:\Program Files\Microchip\MPLABX\%%D\gnuBins\GnuWin32\bin\make.exe"
)
if not exist nbproject\Makefile-impl.mk call "%GEN%" .
"%MK%" -f Makefile CONF=EV74H48A_Curiosity_Platform_MPS512 build
