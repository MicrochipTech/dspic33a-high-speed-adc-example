#!/bin/sh
# gui_setup.sh - the same as gui_setup.bat for Linux / macOS: a private
# Python environment in tools/.venv with the GUI's dependencies.
# Afterwards: tools/.venv/bin/python tools/adc_gui.py --fake
set -e
cd "$(dirname "$0")"
command -v python3 >/dev/null 2>&1 || { echo "python3 not found"; exit 1; }
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip --quiet
.venv/bin/python -m pip install -r requirements-gui.txt
.venv/bin/python adc_gui.py --selftest
echo "Done. Start with: tools/.venv/bin/python tools/adc_gui.py --fake   (or --port /dev/ttyACM0)"
