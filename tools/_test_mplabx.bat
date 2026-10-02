@echo off
rem Internal test helper: build the MPLAB X project from the command line.
rem The customer does not need this - they press Build in the IDE.
rem   _test_mplabx.bat              adc_dma_40msps.X, EV74H48A configuration
rem   _test_mplabx.bat core [nano]  core_example.X (tools\gen_core_project.py), EV74H48A
rem                                 or, with "nano", the EV17P63A configuration
rem The generator rewrites languageToolchainVersion in configurations.xml: restore
rem that line in adc_dma_40msps.X, re-run tools\gen_core_project.py for core_example.X.
set "PRJ=adc_dma_40msps.X"
set "CONF=EV74H48A_Curiosity_Platform_MPS512"
if /i "%1"=="core" set "PRJ=core_example.X"
if /i "%2"=="nano" set "CONF=EV17P63A_Curiosity_Nano_MPS506"
cd /d %~dp0..\%PRJ%
for /f "delims=" %%D in ('dir /b /ad /o-n "C:\Program Files\Microchip\MPLABX\v*" 2^>nul') do (
    if not defined GEN if exist "C:\Program Files\Microchip\MPLABX\%%D\mplab_platform\bin\prjMakefilesGenerator.bat" set "GEN=C:\Program Files\Microchip\MPLABX\%%D\mplab_platform\bin\prjMakefilesGenerator.bat"
    if not defined MK if exist "C:\Program Files\Microchip\MPLABX\%%D\gnuBins\GnuWin32\bin\make.exe" set "MK=C:\Program Files\Microchip\MPLABX\%%D\gnuBins\GnuWin32\bin\make.exe"
)
if not exist nbproject\Makefile-impl.mk call "%GEN%" .
"%MK%" -f Makefile CONF=%CONF% build
