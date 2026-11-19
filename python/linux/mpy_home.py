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
# The home key of a keyboard plugged into the board (the Windows key alone, or the Menu key; the
# REPL hub counts it in /run/mpy-home-key) does what a tap does: on the shell it opens the menu,
# and closes it again; in an app it brings the shell back, as the home button. In the menu the
# arrows, Tab, Home and End move between the buttons, Enter or Space presses one, Esc goes back
# (mpy_keys).
#
# LVGL MPY Studio and mpremote take the board as usual: the shell leaves on their Ctrl-A, and
# after a soft reset from the raw REPL main.py does not run.

import os
import sys

import lvgl as lv
import mpy_keys
import mpy_settings
import mpyrepl

HOME_KEY = "/run/mpy-home-key"

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


_menu = None  # the menu's screen, while it is built
_choices = []  # the buttons the keyboard moves between: the tiles, or those of a question
_chosen = 0
_back = None  # what Esc does


def _choose(buttons, back, index=0):
    global _choices, _chosen, _back
    if _choices:
        _choices[_chosen].set_style_outline_width(0, 0)
    _choices, _back, _chosen = buttons, back, 0
    _select(index)


def _select(index):
    global _chosen
    if not _choices:
        return
    _choices[_chosen].set_style_outline_width(0, 0)
    _chosen = max(0, min(index, len(_choices) - 1))
    button = _choices[_chosen]
    button.set_style_outline_width(3, 0)
    button.set_style_outline_color(lv.color_hex(0xFFFFFF), 0)
    button.set_style_outline_pad(2, 0)


def _vertical(step):
    # The button of the next (step 1) or previous (step -1) row nearest below or above.
    here = _choices[_chosen]
    y, x = here.get_y(), here.get_x()
    rows = [b.get_y() for b in _choices if (b.get_y() - y) * step > 0]
    if not rows:
        return
    row = min(rows) if step > 0 else max(rows)
    nearest = min((abs(b.get_x() - x), i) for i, b in enumerate(_choices) if b.get_y() == row)
    _select(nearest[1])


def _menu_key(code):
    if code in (mpy_keys.RIGHT, mpy_keys.NEXT):
        _select(_chosen + 1)
    elif code in (mpy_keys.LEFT, mpy_keys.PREV):
        _select(_chosen - 1)
    elif code == mpy_keys.DOWN:
        _vertical(1)
    elif code == mpy_keys.UP:
        _vertical(-1)
    elif code == mpy_keys.HOME:
        _select(0)
    elif code == mpy_keys.END:
        _select(len(_choices) - 1)
    elif code == mpy_keys.ENTER and _choices:
        _choices[_chosen].send_event(lv.EVENT.CLICKED, None)
    elif code == mpy_keys.ESC and _back is not None:
        _back()


def _watch_menu_keys():
    mpy_keys.listen(_menu_key, wanted=lambda: _menu is not None and lv.screen_active() == _menu)


def _home_key_count():
    try:
        with open(HOME_KEY) as source:
            return source.read()
    except OSError:
        return None


def _watch_home_key(action):
    # The hub writes the file; a timer of LVGL, which runs in the display's thread, reads it.
    seen = [_home_key_count()]

    def check(_timer):
        count = _home_key_count()
        if count != seen[0]:
            seen[0] = count
            action()

    lv.timer_create(check, 200, None)


def _toggle_menu():
    import ansiterm

    if _menu is not None and lv.screen_active() == _menu:
        ansiterm.term.show()
    else:
        menu()


def menu():
    # The app menu, over the shell. The shell keeps running behind it.
    global _menu, _choices
    import ansiterm

    if _menu is not None and lv.screen_active() != _menu:
        _choices = []  # its buttons go with it
        _menu.delete()  # the menu of the time before: one at a time in memory
    screen = _menu = lv.obj()
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
    tiles = [_tile(grid, app["name"], TILE, lambda p=app["path"]: launch(p)) for app in apps()]
    shell = _tile(grid, lv.SYMBOL.KEYBOARD + "  Shell", ACCENT, lambda: ansiterm.term.show())
    tiles.append(shell)
    tiles.append(_tile(grid, lv.SYMBOL.KEYBOARD + "  Linux shell", 0x2A3444, lambda: launch(LINUX_SHELL)))
    tiles.append(_tile(grid, lv.SYMBOL.POWER + "  Power off", 0xB3261E, lambda: confirm_power_off(screen)))
    if not apps():
        note = _label(screen, "Add apps to settings.json, section \"home\"", 14, 0x9AA6B8)
        note.align(lv.ALIGN.BOTTOM_LEFT, 4, 0)
    lv.screen_load(screen)
    grid.update_layout()  # the tiles' places, for the arrow keys
    _choose(tiles, lambda: ansiterm.term.show())


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
    tiles, chosen = _choices, _chosen

    def back():
        _choose(tiles, lambda: __import__("ansiterm").term.show(), chosen)  # before its buttons go
        box.delete()

    cancel.add_event_cb(lambda e: back(), lv.EVENT.CLICKED, None)
    off = lv.button(box)
    off.set_size(120, 44)
    off.align(lv.ALIGN.BOTTOM_RIGHT, 0, 0)
    off.set_style_bg_color(lv.color_hex(0xB3261E), 0)
    _label(off, "Power off", 16, 0xFFFFFF).center()
    off.add_event_cb(lambda e: power_off(), lv.EVENT.CLICKED, None)
    _choose([cancel, off], back)  # Cancel first: Enter twice must not switch the board off


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


def _app_size(folder):
    # The screen an LVGL MPY Studio app was made for, from the profile the Studio uploaded with it.
    import json

    try:
        with open(folder + "/board_profile.json") as source:
            profile = json.load(source)
        return "%dx%d" % (profile["logicalWidth"], profile["logicalHeight"])
    except (OSError, ValueError, KeyError, TypeError):
        return None


def run_app(path):
    folder = path.rsplit("/", 1)[0]
    size = _app_size(folder)
    if size:
        os.putenv("MPY_APP_SIZE", size)  # on a bigger monitor display_driver centres it
    if folder not in sys.path:
        sys.path.insert(0, folder)
    with open(path) as source:
        code = source.read()
    exec(code, {"__name__": "__main__", "__file__": path})
    # Once the app made its screen (LVGL and the display are up): keyboard and gamepad move in it,
    # unless it took the keys itself
    mpy_keys.navigate()
    _home_button()
    _watch_home_key(go_home)


def main():
    os.unsetenv("MPY_APP_SIZE")  # an app's size, from before the soft reset: the shell has the whole screen
    mpy_keys.stop()  # a menu open before the soft reset no longer wants the keys
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

    import display_driver  # LVGL first, then its timer

    ansiterm.on_screen = _on_terminal
    _watch_home_key(_toggle_menu)
    _watch_menu_keys()
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
