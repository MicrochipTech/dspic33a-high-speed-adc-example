@echo off
rem ---------------------------------------------------------------------
rem  tools\trace.bat [record|clean] - the register-trace harness
rem  (tests\trace\). A thin wrapper: all the work is in
rem  tools\trace_build.py (P0.5b) - an incremental build engine that
rem  compiles each firmware/harness .c file to one .o under
rem  build\trace\obj\<flavor>\ and reuses it across every scenario that
rem  needs the same file compiled the same way, only rebuilding what a
rem  change actually touches, and compiles/links scenarios in parallel.
rem  See trace_build.py's own docstring for the caching rules and why the
rem  old all-in-one-invocation-per-scenario trace.bat took ~4-5 minutes
rem  for 13 scenarios whose executables each run in under a second.
rem
rem  tools\trace.bat             build (incrementally) + run every
rem                              scenario, compare against golden traces
rem  tools\trace.bat record      (re)write every scenario's golden trace
rem                              from what it just produced
rem  tools\trace.bat clean       wipe build\trace\obj and every generated
rem                              header dir first, then run the check
rem                              above from a cold cache - use this if a
rem                              stale object is ever suspected (e.g.
rem                              after editing tools\gen_fake_sfr.py
rem                              itself, which trace_build.py's own
rem                              freshness check already accounts for, or
rem                              after a manual change under build\trace)
rem
rem  A scenario with no golden file at all is still built and run (so a
rem  broken build or a crash is still caught) and reported NEW only in
rem  record mode; in check mode a missing golden is a FAIL (every
rem  scenario has had one since P0.5). A golden file with no matching
rem  scenario source is reported as a warning, not counted.
rem
rem  After the scenarios, tools\check_fake_sfr.py (P0.8) checks the
rem  inputs of the fake header - every SFR's address and bit-field
rem  masks - against the pack's ATDF for both devices (about a second;
rem  see tests\trace\README.md "P0.8 cross-check"). A mismatch there
rem  fails this script like a failed scenario would.
rem
rem  Uses %~dp0, so it runs from any cwd.
rem ---------------------------------------------------------------------
setlocal

where python >nul 2>nul
if errorlevel 1 (
  echo trace: no python found on PATH.
  exit /b 1
)

python "%~dp0trace_build.py" %1
set RC=%errorlevel%
python "%~dp0check_fake_sfr.py"
if errorlevel 1 set RC=1
exit /b %RC%
