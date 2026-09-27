@echo off
rem ---------------------------------------------------------------------
rem  board_run.bat - run tools\board_run.py, preferring tools\.venv's
rem  python if gui_setup.bat has already created one there (it installs
rem  requirements-board.txt's pyserial/numpy into it), like adc_gui.bat
rem  does for the GUI - but falling back to the python on PATH rather than
rem  refusing outright, since board_run.py needs only pyserial, numpy and
rem  the standard library, none of the GUI's own dependencies.
rem
rem  Usage:
rem    tools\board_run.bat COM5                the colleague's one command
rem    tools\board_run.bat --list               which serial ports exist
rem    tools\board_run.bat --selftest           no board, checks the tool
rem  Arguments go through unchanged.
rem ---------------------------------------------------------------------
setlocal
cd /d "%~dp0"
set PY=python
if exist ".venv\Scripts\python.exe" set PY=.venv\Scripts\python.exe
"%PY%" board_run.py %*
endlocal
