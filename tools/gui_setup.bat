@echo off
rem ---------------------------------------------------------------------
rem  gui_setup.bat - one-time set-up for tools\adc_gui.py
rem
rem  Creates a private Python environment in tools\.venv and installs what
rem  the GUI needs (requirements-gui.txt: nicegui, pyserial, numpy) into it.
rem  Nothing is installed into the system Python. Run it again after a
rem  git pull that changed requirements-gui.txt; it only adds what is missing.
rem
rem  Needs: Python 3.10 or newer on the PATH (python --version), internet
rem  access for pip. Afterwards start the GUI with adc_gui.bat.
rem ---------------------------------------------------------------------
setlocal
cd /d "%~dp0"

python --version >nul 2>&1
if errorlevel 1 (
  echo Python was not found on the PATH. Install Python 3.10+ from python.org
  echo and tick "Add python.exe to PATH", then run this again.
  exit /b 1
)

if not exist ".venv\Scripts\python.exe" (
  echo Creating the virtual environment in %CD%\.venv ...
  python -m venv .venv
  if errorlevel 1 (
    echo Creating the virtual environment failed.
    exit /b 1
  )
)

echo Installing / updating the GUI's dependencies ...
".venv\Scripts\python.exe" -m pip install --upgrade pip --quiet
".venv\Scripts\python.exe" -m pip install -r requirements-gui.txt
if errorlevel 1 (
  echo.
  echo pip failed. No internet, or a proxy in the way? Set HTTPS_PROXY and try again.
  exit /b 1
)

echo.
echo Checking the installation with the built-in self-test ...
".venv\Scripts\python.exe" adc_gui.py --selftest
if errorlevel 1 (
  echo The self-test FAILED - see the messages above.
  exit /b 1
)

echo.
echo Done. Start the GUI with:
echo    adc_gui.bat --fake            (no board, synthetic signal)
echo    adc_gui.bat --port COM7       (the board's console port)
endlocal
