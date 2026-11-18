# SPDX-License-Identifier: MIT
# The board's home, started by main.py: at power-on the shell on the display, with the keyboard of
# the serial or TCP terminal. A tap on the terminal opens a menu of apps; an app runs in a fresh
# process (a soft reset), and the home button at the top brings the shell back the same way.
#
# The apps of the menu are in settings.json, section "home", each an LVGL MPY Studio app on the
# board (the entry point the Studio uploaded) and a name:
#
#   {"home": {"apps": [{"name": "Weather", "path": "/lms_apps/app_09a333ec182f/app_main.py"}]}}
#
# LVGL MPY Studio and mpremote take the board as usual: the shell leaves on their Ctrl-A, and
# after a soft reset from the raw REPL main.py does not run.

import os
import sys

import lvgl as lv
import mpy_settings
import mpyrepl

NEXT = "/run/mpy-home-next"  # the app to run after the restart; in RAM, so a reboot forgets it
LINUX_SHELL = "linux-shell"  # NEXT for the Linux shell instead of an app
BACK = 0x121820
TILE = 0x1E2633
ACCENT = 0x2F6FDE


def apps():
    home = mpy_settings.get("home") or {}
    return [a for a in home.get("apps", []) if isinstance(a, dict) and a.get("name") and a.get("path")]


def launch(path):
    with open(NEXT, "w") as target:
        target.write(path)
    mpyrepl.restart()


def go_home():
    try:
        os.remove(NEXT)
    except OSError:
        pass
    mpyrepl.restart()


def _label(parent, text, size, color):
    o = lv.label(parent)
    o.set_text(text)
    o.set_style_text_font(getattr(lv, "font_montserrat_%d" % size), 0)
    o.set_style_text_color(lv.color_hex(color), 0)
    return o


def _tile(parent, text, color, callback):
    b = lv.button(parent)
    b.set_size(140, 72)
    b.set_style_bg_color(lv.color_hex(color), 0)
    b.set_style_radius(14, 0)
    b.set_style_shadow_width(0, 0)
    _label(b, text, 16, 0xFFFFFF).center()
    b.add_event_cb(lambda e: callback(), lv.EVENT.CLICKED, None)
    return b


def menu():
    # The app menu, over the shell. The shell keeps running behind it.
    import ansiterm

    screen = lv.obj()
    screen.set_style_bg_color(lv.color_hex(BACK), 0)
    screen.set_style_pad_all(12, 0)
    _label(screen, "Apps", 24, 0xFFFFFF).set_pos(4, 0)
    grid = lv.obj(screen)
    grid.set_size(lv.pct(100), 210)
    grid.set_pos(0, 38)
    grid.set_style_bg_opa(0, 0)
    grid.set_style_border_width(0, 0)
    grid.set_style_pad_all(4, 0)
    grid.set_style_pad_row(10, 0)
    grid.set_style_pad_column(10, 0)
    grid.set_flex_flow(lv.FLEX_FLOW.ROW_WRAP)
    for app in apps():
        _tile(grid, app["name"], TILE, lambda p=app["path"]: launch(p))
    _tile(grid, lv.SYMBOL.KEYBOARD + "  Shell", ACCENT, lambda: ansiterm.term.show())
    _tile(grid, lv.SYMBOL.KEYBOARD + "  Linux shell", 0x2A3444, lambda: launch(LINUX_SHELL))
    _tile(grid, lv.SYMBOL.POWER + "  Power off", 0xB3261E, lambda: confirm_power_off(screen))
    if not apps():
        note = _label(screen, "Add apps to settings.json, section \"home\"", 14, 0x9AA6B8)
        note.align(lv.ALIGN.BOTTOM_LEFT, 4, 0)
    lv.screen_load(screen)


def confirm_power_off(screen):
    # A tap too many must not switch the board off: ask first.
    box = lv.obj(screen)
    box.set_size(300, 130)
    box.center()
    box.set_style_bg_color(lv.color_hex(TILE), 0)
    box.set_style_border_width(0, 0)
    box.set_style_radius(14, 0)
    question = _label(box, "Power off the board?", 16, 0xFFFFFF)
    question.align(lv.ALIGN.TOP_MID, 0, 6)
    cancel = lv.button(box)
    cancel.set_size(120, 44)
    cancel.align(lv.ALIGN.BOTTOM_LEFT, 0, 0)
    cancel.set_style_bg_color(lv.color_hex(0x2A3444), 0)
    _label(cancel, "Cancel", 16, 0xFFFFFF).center()
    cancel.add_event_cb(lambda e: box.delete(), lv.EVENT.CLICKED, None)
    off = lv.button(box)
    off.set_size(120, 44)
    off.align(lv.ALIGN.BOTTOM_RIGHT, 0, 0)
    off.set_style_bg_color(lv.color_hex(0xB3261E), 0)
    _label(off, "Power off", 16, 0xFFFFFF).center()
    off.add_event_cb(lambda e: power_off(), lv.EVENT.CLICKED, None)


def power_off():
    import linuxproc

    screen = lv.obj()
    screen.set_style_bg_color(lv.color_hex(0), 0)
    note = _label(screen, "Shutting down...\nUnplug the power when the screen goes dark.", 16, 0xFFFFFF)
    note.set_style_text_align(lv.TEXT_ALIGN.CENTER, 0)
    note.center()
    lv.screen_load(screen)
    lv.refr_now(None)
    # In the background: the shutdown stops this process too.
    linuxproc.Process(["systemctl", "poweroff"])


def _on_terminal(screen):
    screen.add_flag(lv.obj.FLAG.CLICKABLE)
    screen.add_event_cb(lambda e: menu(), lv.EVENT.CLICKED, None)


def _home_button():
    # A small tab at the top centre, over every screen of the app.
    b = lv.button(lv.layer_top())
    b.set_size(56, 24)
    b.align(lv.ALIGN.TOP_MID, 0, 0)
    b.set_style_radius(8, 0)
    b.set_style_bg_color(lv.color_hex(0x000000), 0)
    b.set_style_bg_opa(140, 0)
    b.set_style_shadow_width(0, 0)
    _label(b, lv.SYMBOL.HOME, 14, 0xFFFFFF).center()
    b.add_event_cb(lambda e: go_home(), lv.EVENT.CLICKED, None)


def run_app(path):
    folder = path.rsplit("/", 1)[0]
    if folder not in sys.path:
        sys.path.insert(0, folder)
    with open(path) as source:
        code = source.read()
    exec(code, {"__name__": "__main__", "__file__": path})
    _home_button()


def main():
    try:
        with open(NEXT) as source:
            path = source.read().strip()
        os.remove(NEXT)  # the next soft reset, or the home button, goes back to the shell
    except OSError:
        path = None
    if path and path != LINUX_SHELL:
        try:
            run_app(path)
            return
        except Exception as error:  # a missing or broken app: say so and fall back to the shell
            sys.print_exception(error)
    import ansiterm
    import shell_linux

    ansiterm.on_screen = _on_terminal
    startup = None
    if path == LINUX_SHELL:
        # The system's own shell, full screen; its `exit` leaves the MicroPython shell. While it
        # runs, Ctrl-C and Ctrl-A are its own: LVGL MPY Studio and mpremote wait for `exit`.
        bash = any(_exists(d + "/bash") for d in ("/bin", "/usr/bin"))
        startup = "linux bash -l" if bash else "linux sh -l"
    shell_linux.main(startup)


def _exists(path):
    try:
        os.stat(path)
        return True
    except OSError:
        return False
