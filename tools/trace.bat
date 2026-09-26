@echo off
rem ---------------------------------------------------------------------
rem  tools\trace.bat [record] - the register-trace harness (tests\trace\).
rem
rem  Generates the fake xc.h/sfr_syms.ld/sfr_table.c from the device pack
rem  (tools\gen_fake_sfr.py, into build\trace\gen - cheap, about a second,
rem  regenerated every run rather than committed), builds every
rem  tests\trace\scenarios\*.c together with the harness
rem  (tests\trace\harness\*.c) and, per tests\trace\scenarios\NAME.sources,
rem  the real firmware .c files it drives - UNCHANGED, the same files the
rem  device builds use. Each scenario's stdout is its trace,
rem  build\trace\NAME.trace, compared against tests\trace\golden\NAME.trace.
rem
rem  Per-scenario overrides (both optional):
rem    NAME.mcu     a device name (e.g. "33AK512MPS506"): the fake header
rem                 is generated for THAT device, into its own
rem                 build\trace\gen_<mcu> directory, instead of the
rem                 default gen dir (33AK512MPS512) every other scenario
rem                 shares - for `nano`, built against the Curiosity
rem                 Nano's own device pack header (tools\gen_fake_sfr.py
rem                 already took --mcu since P0.3; only trace.bat needed
rem                 to learn to use a second one).
rem    NAME.cflags  extra flags appended to the compile line (e.g.
rem                 "-DBOARD=2" for `nano`).
rem
rem  A scenario with no golden trace yet is still built and run (so a
rem  broken build or a crash is still caught) and reported NEW, same as
rem  P0.4 - but only in `record` mode, where that is expected (the golden
rem  is about to be written for the first time). In the default (check)
rem  mode, from P0.5 on (golden traces now exist for every scenario), a
rem  missing golden is a FAIL, not a PASS: a scenario nobody has ever
rem  recorded a golden for is not proven unchanged, it is simply untested,
rem  and silently counting it as passing would hide exactly that. A golden
rem  file left behind with no matching scenario (a rename or removal) is
rem  reported too, after the per-scenario results.
rem
rem  tools\trace.bat record (re)writes every scenario's golden trace from
rem  what it just produced - use it once a task says a trace is expected
rem  to change, review the diff, then commit the new golden file alongside
rem  the code change that caused it.
rem
rem  Uses %~dp0, so it runs from any cwd.
rem ---------------------------------------------------------------------
setlocal enabledelayedexpansion

set ROOT=%~dp0..
set TRACEDIR=%ROOT%\tests\trace
set SCEN=%TRACEDIR%\scenarios
set HARNESS=%TRACEDIR%\harness
set GOLDEN=%TRACEDIR%\golden
set OUTDIR=%ROOT%\build\trace
set GEN=%OUTDIR%\gen

set MODE=%1

where gcc >nul 2>nul
if errorlevel 1 (
  echo trace: no gcc found on PATH - install a host MinGW gcc first.
  exit /b 1
)
where python >nul 2>nul
if errorlevel 1 (
  echo trace: no python found on PATH.
  exit /b 1
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "%GOLDEN%" mkdir "%GOLDEN%"

echo trace: generating the fake SFR header (tools\gen_fake_sfr.py)...
python "%ROOT%\tools\gen_fake_sfr.py" --out "%GEN%"
if errorlevel 1 (
  echo trace: gen_fake_sfr.py FAILED
  exit /b 1
)
echo.

set BASE_CFLAGS=-std=c11 -O1 -Wall -Wextra -Werror -mno-ms-bitfields -fno-strict-aliasing -Wno-pointer-to-int-cast
set LDFLAGS=-Wl,--disable-dynamicbase

set TOTAL=0
set PASSED=0

for %%F in ("%SCEN%\*.c") do (
  set /a TOTAL+=1
  set "NAME=%%~nF"
  set "SRC="
  if exist "%SCEN%\!NAME!.sources" (
    for /f "usebackq delims=" %%S in ("%SCEN%\!NAME!.sources") do (
      set "SRC=!SRC! "%ROOT%\%%S""
    )
  )

  rem ---- per-scenario device: default gen dir, or NAME.mcu's own -------
  set "SCEN_GEN=%GEN%"
  if exist "%SCEN%\!NAME!.mcu" (
    for /f "usebackq delims=" %%M in ("%SCEN%\!NAME!.mcu") do (
      set "SCEN_MCU=%%M"
      set "SCEN_GEN=%OUTDIR%\gen_%%M"
    )
    if not exist "!SCEN_GEN!" (
      echo trace: generating the fake SFR header for !SCEN_MCU! ^(!NAME!^)...
      python "%ROOT%\tools\gen_fake_sfr.py" --mcu "!SCEN_MCU!" --out "!SCEN_GEN!"
      if errorlevel 1 (
        echo !NAME!: gen_fake_sfr.py FAILED for --mcu !SCEN_MCU!
      )
    )
  )
  set "INC=-I"%HARNESS%" -I"!SCEN_GEN!" -I"%ROOT%""

  rem ---- per-scenario extra flags (NAME.cflags) -------------------------
  set "XFLAGS="
  if exist "%SCEN%\!NAME!.cflags" (
    for /f "usebackq delims=" %%X in ("%SCEN%\!NAME!.cflags") do (
      set "XFLAGS=!XFLAGS! %%X"
    )
  )

  gcc %BASE_CFLAGS% !XFLAGS! !INC! "%%F" "%HARNESS%\recorder.c" "%HARNESS%\stubs.c" "%HARNESS%\hwmodel.c" "!SCEN_GEN!\sfr_table.c" "!SCEN_GEN!\sfr_syms.ld" !SRC! %LDFLAGS% -o "%OUTDIR%\!NAME!.exe" 2>"%OUTDIR%\!NAME!.build.log"
  if errorlevel 1 (
    echo !NAME!: BUILD FAILED
    findstr /i "error" "%OUTDIR%\!NAME!.build.log"
  ) else (
    "%OUTDIR%\!NAME!.exe" > "%OUTDIR%\!NAME!.trace"
    if /i "%MODE%"=="record" (
      copy /y "%OUTDIR%\!NAME!.trace" "%GOLDEN%\!NAME!.trace" >nul
      echo !NAME!: RECORDED
      set /a PASSED+=1
    ) else if not exist "%GOLDEN%\!NAME!.trace" (
      echo !NAME!: FAIL - no golden trace yet ^(tools\trace.bat record^)
    ) else (
      fc /n "%GOLDEN%\!NAME!.trace" "%OUTDIR%\!NAME!.trace" >nul
      if errorlevel 1 (
        echo !NAME!: FAIL
        fc /n "%GOLDEN%\!NAME!.trace" "%OUTDIR%\!NAME!.trace"
      ) else (
        echo !NAME!: PASS
        set /a PASSED+=1
      )
    )
  )
)

rem ---- golden files with no matching scenario (reported, not counted) --
for %%G in ("%GOLDEN%\*.trace") do (
  if not exist "%SCEN%\%%~nG.c" (
    echo %%~nG: WARNING - golden trace exists but tests\trace\scenarios\%%~nG.c does not
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
