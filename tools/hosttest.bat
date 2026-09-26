@echo off
rem ---------------------------------------------------------------------
rem  tools\hosttest.bat - host-side unit tests, tests\host\test_*.c,
rem  built with the installed MinGW gcc. No MPLAB X / xc-dsc needed, and
rem  no external test framework: tests\host\check.h is the whole harness.
rem
rem  A test_NAME.c that needs firmware source files to link against (e.g.
rem  test_crc16.c needs ..\crc16.c) lists them, one per line, relative to
rem  the repo root, in a companion tests\host\test_NAME.sources file. A
rem  test with no such file is just compiled and linked on its own.
rem
rem  Uses %~dp0 to find the repo root, so it works from any cwd.
rem ---------------------------------------------------------------------
setlocal enabledelayedexpansion

set ROOT=%~dp0..
set TESTDIR=%ROOT%\tests\host
set OUTDIR=%ROOT%\build\host

where gcc >nul 2>nul
if errorlevel 1 (
  echo hosttest: no gcc found on PATH - install a host MinGW gcc first.
  exit /b 1
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

set TOTAL=0
set PASSED=0

for %%F in ("%TESTDIR%\test_*.c") do (
  set /a TOTAL+=1
  set "NAME=%%~nF"
  set "EXTRA="
  if exist "%TESTDIR%\!NAME!.sources" (
    for /f "usebackq delims=" %%S in ("%TESTDIR%\!NAME!.sources") do (
      set "EXTRA=!EXTRA! "%ROOT%\%%S""
    )
  )
  gcc -std=c11 -Wall -Wextra -Werror -I"%ROOT%" -I"%TESTDIR%" "%%F" !EXTRA! -o "%OUTDIR%\!NAME!.exe"
  if errorlevel 1 (
    echo !NAME!: BUILD FAILED
  ) else (
    "%OUTDIR%\!NAME!.exe"
    if errorlevel 1 (
      echo !NAME!: FAIL
    ) else (
      echo !NAME!: PASS
      set /a PASSED+=1
    )
  )
)

echo.
if !PASSED! EQU !TOTAL! (
  echo !PASSED!/!TOTAL! PASS
  exit /b 0
) else (
  echo !PASSED!/!TOTAL! FAIL
  exit /b 1
)
