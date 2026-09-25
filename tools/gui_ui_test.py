"""Headless-browser test of tools/adc_gui.py against its built-in --fake
target: connect, LIVE with the test triangle input, LIVE with a custom
input, a rate change while LIVE, STOP, SINGLE, and no server-side
exception.

How to run (Windows, from the repository root):

    pip install playwright                 (once, into whatever Python has
                                             nicegui/pyserial/numpy too)
    python -m playwright install chrome    (once, if Chrome is not already
                                             on the machine - this test uses
                                             the installed Chrome via
                                             channel="chrome", not the
                                             Playwright-managed Chromium)
    python tools/gui_ui_test.py

The GUI server itself is started with the system `python` (must have
nicegui/pyserial/numpy); only the browser driver needs the `playwright`
package. Exit code is 0 on PASS, 1 on FAIL. A full-page screenshot is
written to the path printed at the end.
"""
import os
import re
import subprocess
import sys
import time

from playwright.sync_api import sync_playwright

def free_port():
    """A TCP port nothing listens on - a fixed one collided with a GUI left
    running from an earlier session, and the browser then tested that one."""
    import socket
    with socket.socket() as so:
        so.bind(("127.0.0.1", 0))
        return so.getsockname()[1]


PORT = free_port()
SCRATCH = r"C:\Users\M91221\AppData\Local\Temp\claude\c--work-Claas-ADC\333dbcfd-bfb8-46c9-aaa8-ccdc3122088f\scratchpad"
SCREENSHOT = SCRATCH + r"\gui_after.png"
GUI_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "adc_gui.py")

gui = subprocess.Popen(["python", GUI_SCRIPT, "--fake",
                        "--no-browser", "--http-port", str(PORT)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(("PASS " if ok else "FAIL ") + name + ("  - " + detail if detail else ""))


def wait_text(page, locator, pattern, timeout=20.0):
    t0 = time.time()
    last = ""
    while time.time() - t0 < timeout:
        try:
            last = locator.inner_text(timeout=1000)
            if re.search(pattern, last):
                return True, last
        except Exception:
            pass
        time.sleep(0.3)
    return False, last


def wait_changed(page, locator, baseline, timeout=20.0):
    """Like wait_text, but waits for the text to differ from `baseline`
    rather than merely matching a pattern - a static label like 'grab 30 ...'
    already matches '^grab \\d+' before the next grab has even happened."""
    t0 = time.time()
    last = baseline
    while time.time() - t0 < timeout:
        try:
            last = locator.inner_text(timeout=1000)
            if last != baseline:
                return True, last
        except Exception:
            pass
        time.sleep(0.3)
    return False, last


try:
    time.sleep(10)  # let the server come up
    with sync_playwright() as p:
        b = p.chromium.launch(channel="chrome", headless=True)
        page = b.new_page(viewport={"width": 1600, "height": 3400})
        page.goto(f"http://127.0.0.1:{PORT}/")
        time.sleep(4)  # auto-connect to the fake target

        cyc = page.get_by_text(re.compile(r"^(no grab yet|grab \d|cycle failed|grab failed|stream on refused)"))
        rate_field = page.get_by_label(re.compile(r"^rate, kSPS", re.I))
        live = page.get_by_role("button", name=re.compile(r"^\W*live$", re.I))
        stop_btn = page.get_by_role("button", name=re.compile(r"^\W*stop$", re.I))
        single = page.get_by_role("button", name=re.compile(r"^\W*single$", re.I))
        triangle_chip = page.get_by_text(re.compile(r"^(PASS|FAIL)$")).first
        fundamental = page.get_by_text(re.compile(r"^fundamental"))

        # ---- connect (fake) ----
        conn = page.locator(".q-chip").filter(has_text="adc_dma_40msps").first
        ok, txt = wait_text(page, conn, r".", timeout=15.0)
        check("connect (fake)", ok, txt[:80])

        # ---- LIVE with the test input: cycles at fs = the chosen rate, PASS triangle ----
        live.click()
        ok, txt = wait_text(page, cyc, r"^grab [3-9]")
        check("LIVE (test input) runs grab cycles", ok, txt[:100])
        ok_rate = "8000 kSPS actual" in cyc.inner_text() or re.search(r"79\d\d kSPS actual|80\d\d kSPS actual", cyc.inner_text())
        check("LIVE (test input) fs follows the chosen rate (8000)", bool(ok_rate), cyc.inner_text()[:100])
        ok, txt = wait_text(page, triangle_chip, r"^PASS$")
        check("LIVE (test input) shows a PASS triangle verdict", ok, txt[:60])

        # ---- a rate change while LIVE: the actual rate follows ----
        rate_field.fill("4000")
        rate_field.press("Tab")
        time.sleep(2.5)
        ok, txt = wait_text(page, cyc, r"4\d\d\d kSPS actual")
        check("rate change while LIVE takes effect (actual rate follows)", ok, txt[:100])

        # back to 8000 before switching input, so the next check is unambiguous
        rate_field.fill("8000")
        rate_field.press("Tab")
        time.sleep(1.5)

        # ---- STOP ----
        stop_btn.first.click()
        time.sleep(1.0)
        ok_live_btn = live.count() == 1
        check("STOP puts the button back to 'live'", ok_live_btn)

        # ---- LIVE with a non-test input: spectrum with a fundamental, no triangle verdict ----
        page.get_by_label(re.compile(r"^input$", re.I)).click()
        page.get_by_text("custom input", exact=True).click()
        core_field = page.get_by_label(re.compile(r"^core \(1\.\.5\)", re.I))
        pin_field = page.get_by_label(re.compile(r"^PINSEL", re.I))
        core_field.click()
        page.get_by_role("option", name="ADC3").click()
        pin_field.fill("5")
        pin_field.press("Tab")

        live.click()
        ok, txt = wait_text(page, cyc, r"^grab [3-9]")
        check("LIVE (custom input core 3 pin 5) runs grab cycles", ok, txt[:100])
        ok, txt = wait_text(page, fundamental, r"fundamental")
        check("LIVE (custom input) spectrum shows a fundamental", ok, txt[:60])
        triangle_card_visible = page.get_by_text("triangle verdict · test input only").is_visible()
        check("LIVE (custom input) hides the triangle verdict card", not triangle_card_visible)

        stop_btn.first.click()
        time.sleep(1.0)

        # ---- SINGLE works when not live ----
        c0 = cyc.inner_text()
        single.click()
        ok, txt = wait_changed(page, cyc, c0, timeout=15.0)
        check("SINGLE grabs once when not live", ok, txt[:100])

        page.screenshot(path=SCREENSHOT, full_page=True)
        b.close()
finally:
    gui.terminate()
    try:
        out = gui.communicate(timeout=10)[0]
    except Exception:
        out = ""
    errs = [l for l in out.splitlines() if re.search(r"Traceback|error|exception", l, re.I)]
    check("no server-side exceptions", not errs, "; ".join(errs[:5]))

# ---- the Curiosity Nano profile: a second GUI whose fake reports the EV17P63A ----
NANO_PORT = free_port()
gui2 = subprocess.Popen(["python", GUI_SCRIPT, "--fake", "--fake-board", "EV17P63A",
                         "--no-browser", "--http-port", str(NANO_PORT)],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
try:
    time.sleep(10)
    with sync_playwright() as p:
        b = p.chromium.launch(channel="chrome", headless=True)
        page = b.new_page(viewport={"width": 1600, "height": 3400})
        page.goto(f"http://127.0.0.1:{NANO_PORT}/")
        time.sleep(4)
        conn = page.locator(".q-chip").filter(has_text="adc_dma_40msps").first
        ok, txt = wait_text(page, conn, r"EV17P63A", timeout=15.0)
        check("Nano: the board is detected from 'version'", ok, txt[:90])
        page.get_by_label(re.compile(r"^input$", re.I)).click()
        page.get_by_text("custom input", exact=True).click()
        core_txt = page.get_by_label(re.compile(r"^core \(1\.\.5\)", re.I)).locator("xpath=ancestor::label").inner_text()
        pin_val = page.get_by_label(re.compile(r"^PINSEL", re.I)).input_value()
        check("Nano: default input switched to core 1, PINSEL 0 (RA2)",
              ("ADC1" in core_txt) and pin_val == "0", f"{core_txt.split()[-1]} / {pin_val}")
        cyc = page.get_by_text(re.compile(r"^(no grab yet|grab \d|cycle failed|grab failed|stream on refused)")).first
        page.get_by_role("button", name=re.compile(r"^\W*live$", re.I)).click()
        ok, txt = wait_text(page, cyc, r"^grab [3-9]")
        check("Nano: LIVE on the default input runs grab cycles", ok, txt[:90])
        ok, txt = wait_text(page, page.get_by_text(re.compile(r"^fundamental")), r"fundamental")
        check("Nano: spectrum shows a fundamental", ok, txt[:60])
        page.get_by_role("button", name=re.compile(r"^\W*stop$", re.I)).first.click()
        b.close()
finally:
    gui2.terminate()
    try:
        out = gui2.communicate(timeout=10)[0]
    except Exception:
        out = ""
    errs = [l for l in out.splitlines() if re.search(r"Traceback|error|exception", l, re.I)]
    check("Nano: no server-side exceptions", not errs, "; ".join(errs[:5]))

print("UI TEST", "PASS" if all(results) else "FAIL")
print("screenshot:", SCREENSHOT)
sys.exit(0 if all(results) else 1)
