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
import json
import os
import tempfile
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
# The test's own settings file, so that "save" in it never touches the
# user's tools/adc_gui_settings.json.
SETTINGS_TMP = os.path.join(tempfile.mkdtemp(prefix="adc_gui_test_"), "settings.json")


def server_errors(out):
    """Error lines of a GUI's output. A client that goes away while the
    server shuts down leaves a ConnectionResetError (WinError 10054) from
    asyncio's proactor on Windows - that is the browser closing, not the
    GUI failing, so the traceback it belongs to is not counted."""
    lines = out.splitlines()
    if any("WinError 10054" in l for l in lines):
        lines = [l for l in lines if not re.search(r"_call_connection_lost|10054|Traceback|"
                                                   r"^\s+File |^\s+\^|self\._sock|ConnectionResetError", l)]
    return [l for l in lines if re.search(r"Traceback|error|exception", l, re.I)]
SCRATCH = r"C:\Users\M91221\AppData\Local\Temp\claude\c--work-Claas-ADC\333dbcfd-bfb8-46c9-aaa8-ccdc3122088f\scratchpad"
SCREENSHOT = SCRATCH + r"\gui_after.png"
GUI_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "adc_gui.py")

gui = subprocess.Popen(["python", GUI_SCRIPT, "--fake", "--settings", SETTINGS_TMP,
                        "--no-browser", "--http-port", str(PORT)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
results = []


def check(name, ok, detail=""):
    results.append(ok)
    line = ("PASS " if ok else "FAIL ") + name + ("  - " + detail if detail else "")
    print(line.encode("ascii", "replace").decode())   # the Windows console is cp1252


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

        # ---- tooltips: readable size, and the header checkbox hides them ----
        rate_field.hover()
        time.sleep(1.5)
        tip = page.locator(".q-tooltip").first
        vis = tip.is_visible()
        size = tip.evaluate("e => getComputedStyle(e).fontSize") if vis else "-"
        check("tooltip shows on hover, at 18px", vis and size == "18px", size)
        page.mouse.move(5, 5)
        time.sleep(0.8)
        page.get_by_role("checkbox", name=re.compile(r"tooltips", re.I)).click()
        time.sleep(0.5)
        rate_field.hover()
        time.sleep(1.5)
        hidden = all(not t.is_visible() for t in page.locator(".q-tooltip").all())
        check("header checkbox 'tooltips' off hides every tooltip", hidden)
        page.get_by_role("checkbox", name=re.compile(r"tooltips", re.I)).click()

        # ---- time chart: tooltip "time / counts (dot) volts / sample", vref ----
        tchart = page.locator(".tile", has=page.locator(".card-title", has_text="time signal")) \
                     .locator(".nicegui-echart").first
        TT_RE = re.compile(r"^([\d.]+) \S+\n(\d+)\D+([\d.]+) V\nsample (\d+)$")

        def chart_tooltip():
            bx = tchart.bounding_box()
            page.mouse.move(bx["x"] + bx["width"] * 0.55, bx["y"] + bx["height"] * 0.5)
            time.sleep(1.2)
            tt = page.evaluate("() => [...document.querySelectorAll('div')].filter(d => d.style && "
                               "d.style.zIndex === '9999999' && d.innerText).map(d => d.innerText)")
            return tt[0] if tt else ""
        tt = chart_tooltip()
        m = TT_RE.match(tt)
        ok_tt = bool(m) and abs(float(m.group(3)) - int(m.group(2)) * 3.3 / 4096) < 0.002
        check("time tooltip: time / counts, dot, volts / sample (at 3.3 V)", ok_tt,
              tt.replace("\n", " | "))
        vref = page.get_by_label(re.compile(r"^reference voltage", re.I))
        vref.fill("2.5")
        vref.press("Tab")
        time.sleep(0.8)
        tt = chart_tooltip()
        m = TT_RE.match(tt)
        ok_tt = bool(m) and abs(float(m.group(3)) - int(m.group(2)) * 2.5 / 4096) < 0.002
        check("reference voltage 2.5 V: the tooltip's volts follow", ok_tt, tt.replace("\n", " | "))
        page.mouse.move(5, 5)

        # ---- tiles fold and unfold on a click on their title ----
        title = page.locator(".tile .card-title", has_text="spectrum")
        # what sits under the title: the tile's second child (the chart)
        canvas = page.locator(".tile", has=page.locator(".card-title", has_text="spectrum"))                      .locator(":scope > :nth-child(2)").first
        before = canvas.is_visible()
        title.click()
        time.sleep(0.5)
        folded = not canvas.is_visible()
        arrow = title.evaluate("e => getComputedStyle(e, '::before').content")
        title.click()
        time.sleep(0.8)
        unfolded = canvas.is_visible()
        check("a tile folds on a click on its title and unfolds again",
              before and folded and unfolded,
              f"visible before {before}, hidden when folded {folded}, visible again {unfolded}")

        # ---- settings: fold a tile, keep vref 2.5 V, save to the test's file ----
        page.locator(".tile .card-title", has_text="buffer").click()
        time.sleep(0.5)
        page.get_by_role("button", name=re.compile(r"^\W*save$", re.I)).click()
        time.sleep(1.0)
        try:
            saved = json.load(open(SETTINGS_TMP, encoding="utf-8"))
        except (OSError, ValueError):
            saved = {}
        check("save writes view.collapsed, view.vref and connection.port",
              "buffer" in saved.get("view", {}).get("collapsed", []) and
              abs(saved.get("view", {}).get("vref", 0) - 2.5) < 1e-6 and
              "port" in saved.get("connection", {}),
              json.dumps(saved.get("view", {}))[:120])

        page.screenshot(path=SCREENSHOT, full_page=True)
        b.close()
finally:
    gui.terminate()
    try:
        out = gui.communicate(timeout=10)[0]
    except Exception:
        out = ""
    errs = server_errors(out)
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

        # The preset after detection is the DAC loopback: DAC2 -> RA8 -> AD5AN3,
        # shown as such on the board tile (core ADC5, AN3 · RA8, source DAC2).
        # The tile's selects are disabled in test mode, so they are read as
        # field texts ("label value"), not looked up by their aria label.
        fields = [t.replace("\n", " ") for t in page.locator(".q-field").all_inner_texts()]

        def field_text(label):
            return " ".join(t for t in fields if t.startswith(label))
        core_t = field_text("core ")
        chan_t = field_text("channel (PINSEL)")
        src_t = field_text("signal source")
        page.screenshot(path=SCRATCH + r"\gui_nano.png", full_page=True)
        check("Nano: preset is the DAC loopback on the board tile (ADC5, AN3 = RA8, DAC2)",
              ("ADC5" in core_t) and ("AN3" in chan_t and "RA8" in chan_t) and ("DAC2" in src_t),
              f"{core_t[:30]} | {chan_t[:40]} | {src_t[:40]}")
        cyc = page.get_by_text(re.compile(r"^(no grab yet|grab \d|cycle failed|grab failed|stream on refused)")).first
        page.get_by_role("button", name=re.compile(r"^\W*live$", re.I)).click()
        ok, txt = wait_text(page, cyc, r"^grab [3-9]")
        check("Nano: LIVE on the loopback runs grab cycles", ok, txt[:90])
        ok, txt = wait_text(page, page.get_by_text(re.compile(r"^(PASS|FAIL)$")).first, r"^PASS$")
        check("Nano: the loopback triangle passes the grid check", ok, txt[:30])
        page.get_by_role("button", name=re.compile(r"^\W*stop$", re.I)).first.click()
        time.sleep(1.0)

        page.get_by_label(re.compile(r"^input$", re.I)).click()
        page.get_by_text("custom input", exact=True).click()
        core_txt = page.get_by_label(re.compile(r"^core \(1\.\.5\)", re.I)).locator("xpath=ancestor::label").inner_text()
        pin_val = page.get_by_label(re.compile(r"^PINSEL", re.I)).input_value()
        check("Nano: default input switched to core 1, PINSEL 0 (RA2)",
              ("ADC1" in core_txt) and pin_val == "0", f"{core_txt.split()[-1]} / {pin_val}")
        c0 = cyc.inner_text()
        page.get_by_role("button", name=re.compile(r"^\W*live$", re.I)).click()
        ok, txt = wait_changed(page, cyc, c0, timeout=15.0)
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
    errs = server_errors(out)
    check("Nano: no server-side exceptions", not errs, "; ".join(errs[:5]))

# ---- a new start reads the saved file; "standard" puts the standard back ----
PORT3 = free_port()
gui3 = subprocess.Popen(["python", GUI_SCRIPT, "--fake", "--settings", SETTINGS_TMP,
                         "--no-browser", "--http-port", str(PORT3)],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
try:
    time.sleep(10)
    with sync_playwright() as p:
        b = p.chromium.launch(channel="chrome", headless=True)
        page = b.new_page(viewport={"width": 1600, "height": 3400})
        page.goto(f"http://127.0.0.1:{PORT3}/")
        time.sleep(5)
        buf_tile = page.locator(".tile", has=page.locator(".card-title", has_text="buffer")).first
        vref = page.get_by_label(re.compile(r"^reference voltage", re.I))
        folded = "collapsed" in (buf_tile.get_attribute("class") or "")
        v = vref.input_value()
        check("restart: the saved file is read (buffer tile folded, vref 2.5)",
              folded and v.replace(",", ".").startswith("2.5"), f"folded {folded}, vref {v}")
        page.get_by_role("button", name=re.compile(r"^\W*standard$", re.I)).click()
        time.sleep(1.5)
        folded = "collapsed" in (buf_tile.get_attribute("class") or "")
        v = vref.input_value()
        check("'standard' puts the standard back (tile open, vref 3.3)",
              (not folded) and v.replace(",", ".").startswith("3.3"), f"folded {folded}, vref {v}")
        b.close()
finally:
    gui3.terminate()
    try:
        out = gui3.communicate(timeout=10)[0]
    except Exception:
        out = ""
    errs = server_errors(out)
    check("settings restart: no server-side exceptions", not errs, "; ".join(errs[:5]))

print("UI TEST", "PASS" if all(results) else "FAIL")
print("screenshot:", SCREENSHOT)
sys.exit(0 if all(results) else 1)
