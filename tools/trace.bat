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
rem  build\trace\NAME.trace, compared against tests\trace\golden\NAME.trace
rem  if that file exists.
rem
rem  A scenario with no golden trace yet is still built and run (so a
rem  broken build or a crash is still caught), reported NEW rather than
rem  PASS/FAIL. tools\trace.bat record (re)writes every scenario's golden
rem  trace from what it just produced - use it once a task says a trace
rem  is expected to change, review the diff, then commit the new golden
rem  file alongside the code change that caused it.
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

set CFLAGS=-std=c11 -O1 -Wall -Wextra -Werror -mno-ms-bitfields -fno-strict-aliasing -Wno-pointer-to-int-cast
set LDFLAGS=-Wl,--disable-dynamicbase
set INC=-I"%HARNESS%" -I"%GEN%" -I"%ROOT%"

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
  gcc %CFLAGS% %INC% "%%F" "%HARNESS%\recorder.c" "%HARNESS%\stubs.c" "%HARNESS%\hwmodel.c" "%GEN%\sfr_table.c" "%GEN%\sfr_syms.ld" !SRC! %LDFLAGS% -o "%OUTDIR%\!NAME!.exe" 2>"%OUTDIR%\!NAME!.build.log"
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
      echo !NAME!: NEW - no golden trace yet ^(tools\trace.bat record^)
      set /a PASSED+=1
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

echo.
if !PASSED! EQU !TOTAL! (
  echo !PASSED!/!TOTAL! PASS
  exit /b 0
) else (
  echo !PASSED!/!TOTAL! FAIL
  exit /b 1
)
