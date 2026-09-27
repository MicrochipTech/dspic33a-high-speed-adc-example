@echo off
rem ---------------------------------------------------------------------
rem  tools\hosttest.bat - host-side unit tests, tests\host\test_*.c,
rem  built with the installed MinGW gcc. No MPLAB X / xc-dsc needed, and
rem  no external test framework: tests\host\check.h is the whole harness.
rem
rem  A test_NAME.c that needs firmware source files to link against (e.g.
rem  test_crc16.c needs src\lib\crc16.c) lists them, one per line, relative
rem  to the repo root, in a companion tests\host\test_NAME.sources file. A
rem  test with no such file is just compiled and linked on its own. Every
rem  src\<folder> is on the include path, so "name.h" resolves as it does
rem  in the firmware build.
rem
rem  After the C tests, every tests\host\test_*.py runs with the `python`
rem  on the PATH (standard library only - no venv needed; 3.8 or newer).
rem  They run after the C tests because they may drive a test executable
rem  just built (test_tri_eval_xcheck.py drives test_tri_eval.exe --eval,
rem  P2.4). No python on the PATH counts as a failure, like no gcc.
rem
rem  Uses %~dp0 to find the repo root, so it works from any cwd.
rem ---------------------------------------------------------------------
setlocal enabledelayedexpansion

set ROOT=%~dp0..
set TESTDIR=%ROOT%\tests\host
set OUTDIR=%ROOT%\build\host
set SRC=%ROOT%\src
set INC=-I"%SRC%\drivers" -I"%SRC%\app" -I"%SRC%\cli" -I"%SRC%\tests" -I"%SRC%\lib" -I"%SRC%\diag" -I"%SRC%\sim" -I"%SRC%\port" -I"%SRC%\meter"

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
  gcc -std=c11 -Wall -Wextra -Werror %INC% -I"%TESTDIR%" "%%F" !EXTRA! -o "%OUTDIR%\!NAME!.exe"
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

for %%F in ("%TESTDIR%\test_*.py") do (
  set /a TOTAL+=1
  set "NAME=%%~nF"
  where python >nul 2>nul
  if errorlevel 1 (
    echo !NAME!: no python found on PATH - FAIL
  ) else (
    python "%%F"
    if errorlevel 1 (
      echo !NAME!: FAIL
    ) else (
      echo !NAME!: PASS
      set /a PASSED+=1
    )
  )
)

rem  tools\board_run.py (BR.1, the board-run runner) is not a tests\host\
rem  test_*.py file - it lives in tools\ because it is a real command the
rem  colleague runs, not only a test - so its --selftest is called here
rem  explicitly, after the tests above, exactly as board-run-task.md
rem  section 4/8 and the BR.1 card ask for.
set /a TOTAL+=1
if exist "%ROOT%\tools\board_run.py" (
  where python >nul 2>nul
  if errorlevel 1 (
    echo board_run.py --selftest: no python found on PATH - FAIL
  ) else (
    python "%ROOT%\tools\board_run.py" --selftest
    if errorlevel 1 (
      echo board_run.py --selftest: FAIL
    ) else (
      echo board_run.py --selftest: PASS
      set /a PASSED+=1
    )
  )
) else (
  echo board_run.py --selftest: tools\board_run.py not found - FAIL
)

rem  tools\eval_board.py (BR.2): turns a board_run.py session into deviation
rem  lines against tests\board\expected.json - same reasoning as
rem  board_run.py above, called explicitly right after it.
set /a TOTAL+=1
if exist "%ROOT%\tools\eval_board.py" (
  where python >nul 2>nul
  if errorlevel 1 (
    echo eval_board.py --selftest: no python found on PATH - FAIL
  ) else (
    python "%ROOT%\tools\eval_board.py" --selftest
    if errorlevel 1 (
      echo eval_board.py --selftest: FAIL
    ) else (
      echo eval_board.py --selftest: PASS
      set /a PASSED+=1
    )
  )
) else (
  echo eval_board.py --selftest: tools\eval_board.py not found - FAIL
)

echo.
if !PASSED! EQU !TOTAL! (
  echo !PASSED!/!TOTAL! PASS
  exit /b 0
) else (
  echo !PASSED!/!TOTAL! FAIL
  exit /b 1
)
