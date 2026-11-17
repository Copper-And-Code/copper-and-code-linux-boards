# SPDX-License-Identifier: MIT
# The screen shown at start when there is no main.py (or in safe mode): the board is ready and says
# where LVGL MPY Studio can reach it. Frozen; called by the launcher before the REPL starts.

import os
import time

import display_driver
import lvgl as lv


def _uptime():
    with open("/proc/uptime") as source:
        return source.read().split()[0]


def show(safe_mode=False):
    tcp = os.getenv("MPY_REPL_TCP") or "-"
    screen = lv.obj()
    screen.set_style_bg_color(lv.color_hex(0x101828), 0)
    title = lv.label(screen)
    title.set_text("MicroPython ready")
    title.set_style_text_color(lv.color_hex(0xFFFFFF), 0)
    title.set_style_text_font(lv.font_montserrat_24, 0)
    title.align(lv.ALIGN.TOP_MID, 0, 40)
    lines = [
        "Safe mode: main.py skipped" if safe_mode else "Waiting for LVGL MPY Studio",
        "USB serial: the CDC-ACM port",
        "TCP: socket://" + tcp,
    ]
    body = lv.label(screen)
    body.set_text("\n".join(lines))
    body.set_style_text_color(lv.color_hex(0xFFB020 if safe_mode else 0xC8D0E0), 0)
    body.set_style_text_align(lv.TEXT_ALIGN.CENTER, 0)
    body.align(lv.ALIGN.CENTER, 0, 30)
    lv.screen_load(screen)
    lv.refr_now(display_driver.display)
    uptime = _uptime()
    print("MPY_READY_SCREEN uptime", uptime)
    # For boot-time measurements, when no client reads the REPL yet.
    try:
        with open("/run/mpy-ready", "w") as target:
            target.write(uptime + "\n")
    except OSError:
        pass
