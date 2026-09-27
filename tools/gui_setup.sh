#!/bin/sh
# gui_setup.sh - the same as gui_setup.bat for Linux / macOS: a private
# Python environment in tools/.venv with the dependencies of adc_gui.py,
# board_run.py and eval_board.py (requirements-gui.txt + requirements-board.txt;
# matplotlib in the latter is only for eval_chain.py --png).
#
# Afterwards: tools/.venv/bin/python tools/adc_gui.py --fake
#             tools/.venv/bin/python tools/board_run.py --selftest
set -e
cd "$(dirname "$0")"
command -v python3 >/dev/null 2>&1 || { echo "python3 not found"; exit 1; }
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip --quiet
.venv/bin/python -m pip install -r requirements-gui.txt -r requirements-board.txt
echo "Checking the GUI with its built-in self-test ..."
.venv/bin/python adc_gui.py --selftest
echo "Checking board_run.py with its built-in self-test ..."
.venv/bin/python board_run.py --selftest
echo "Checking eval_board.py with its built-in self-test ..."
.venv/bin/python eval_board.py --selftest
echo "Done. Start with: tools/.venv/bin/python tools/adc_gui.py --fake   (or --port /dev/ttyACM0)"
echo "Or run a board session with: tools/.venv/bin/python tools/board_run.py <port>"
