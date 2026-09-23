@echo off
rem  adc_gui.bat - start the GUI from the environment gui_setup.bat created.
rem  Arguments go through: --fake, --port COMx, --http-port 8080, --no-browser
setlocal
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  echo No tools\.venv yet - run gui_setup.bat first.
  exit /b 1
)
".venv\Scripts\python.exe" adc_gui.py %*
endlocal
