# SPDX-License-Identifier: MIT
# The moving keys of a keyboard plugged into the board, for an LVGL screen: arrows, Tab, Home,
# End, Enter (Space too) and Esc. While a screen wants them, /run/mpy-menu exists and the REPL hub
# sends those keys here, one datagram each on /run/mpy-keys, instead of to the terminal; the other
# keys do nothing then. The app menu of mpy_home uses them, and so can an app:
#
#   import mpy_keys
#   mpy_keys.listen(on_key)                  # on_key(mpy_keys.UP), ...: in LVGL's thread
#   mpy_keys.listen(on_key, wanted=shown)    # only while shown() is true, checked every 40 ms
#
# One listener for each MicroPython process: a second listen() replaces the first.
#
# An app that does not listen gets navigate() from mpy_home: Tab, the d-pad (or the arrows) and
# the shoulders move the focus between the buttons, switches, sliders... of the screen shown;
# Enter, Space, or A, B and Start on a gamepad press the focused one. On a slider, arc or spinbox
# left and right change its value, on a roller or an open dropdown up and down; Esc closes it.

import os

import lvgl as lv

UP, DOWN, RIGHT, LEFT, ESC = 17, 18, 19, 20, 27  # LVGL's key codes (lv_key_t)
ENTER, NEXT, PREV, HOME, END = 10, 9, 11, 2, 3
FLAG = "/run/mpy-menu"
SOCKET = "/run/mpy-keys"

_socket = None
_timer = None
_flag = None  # whether FLAG exists


def _set_flag(on):
    global _flag
    if on == _flag:
        return
    _flag = on
    try:
        if on:
            open(FLAG, "w").close()
        else:
            os.remove(FLAG)
    except OSError:
        pass


def _open():
    global _socket
    if _socket is not None:
        return
    import socket
    import struct

    try:
        os.remove(SOCKET)
    except OSError:
        pass
    _socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    _socket.bind(struct.pack("H", socket.AF_UNIX) + SOCKET.encode() + b"\0")
    _socket.setblocking(False)


def listen(callback, wanted=None):
    """callback(key) for every key while wanted() is true (always without it)."""
    global _timer
    _open()
    _set_flag(False)
    if _timer is not None:
        _timer.delete()

    def check(_timer):
        on = wanted() if wanted is not None else True
        _set_flag(on)
        while True:
            try:
                data = _socket.recv(16)
            except OSError:
                return
            if on and data:
                callback(data[0])

    _timer = lv.timer_create(check, 40, None)
    return _timer


def stop():
    """The keys go back to the terminal."""
    global _timer
    if _timer is not None:
        _timer.delete()
        _timer = None
    _set_flag(False)


# navigate(): the widgets a key can reach, and the LVGL keypad that presses them
_NAV_CLASSES = ("button", "checkbox", "switch", "slider", "arc", "spinbox", "roller", "dropdown",
                "buttonmatrix", "imagebutton", "calendar")
_HORIZONTAL = ("slider", "arc", "spinbox")  # left and right are the widget's
_VERTICAL = ("roller",)  # up and down are the widget's
_GRID = ("buttonmatrix",)  # all four (the on-screen keyboard, a calendar's days): Tab leaves
_nav = None


class _Navigation:
    def __init__(self):
        self.group = lv.group_create()
        self.indev = lv.indev_create()
        self.indev.set_type(lv.INDEV_TYPE.KEYPAD)
        self.indev.set_read_cb(self._read)
        self.indev.set_group(self.group)
        self.classes = [getattr(lv, name + "_class") for name in _NAV_CLASSES if hasattr(lv, name + "_class")]
        probe = lv.obj(lv.layer_sys())
        self.handlers = probe.get_event_count()  # the ones every object has
        probe.delete()
        self.queue = []
        self.pressed = None  # the key being pressed: released on the next read
        self.members = []
        self.shown = False  # whether a key showed the focus on these widgets

    def _reachable(self, obj):
        for cls in self.classes:
            if obj.has_class(cls):
                return True
        # An object made to be tapped: a tile with a click handler of the app
        return obj.has_flag(lv.obj.FLAG.CLICKABLE) and obj.get_event_count() > self.handlers

    def _collect(self, obj, found):
        for i in range(obj.get_child_count()):
            child = obj.get_child(i)
            if child.has_flag(lv.obj.FLAG.HIDDEN) or child.has_state(lv.STATE.DISABLED):
                continue
            if self._reachable(child):
                found.append(child)  # not its parts: a button's label
            else:
                self._collect(child, found)  # a container: a list's rows, a panel's buttons

    def _refresh(self):
        # The widgets of the screen shown, in the order they were made; the focus stays where it was.
        found = []
        self._collect(lv.screen_active(), found)
        if found == self.members:
            return
        focused = self.group.get_focused()
        self.group.remove_all_objs()
        for obj in found:
            self.group.add_obj(obj)
        self.members = found
        if focused is not None and focused in found:
            lv.group_focus_obj(focused)
        else:
            self.shown = False

    def _kind(self, obj):
        for name in _HORIZONTAL + _VERTICAL + _GRID + ("dropdown",):
            cls = getattr(lv, name + "_class", None)
            if cls is not None and obj.has_class(cls):
                return name
        return None

    def key(self, key):
        self._refresh()
        if not self.members:
            return
        focused = self.group.get_focused()
        kind = self._kind(focused)
        if key in (LEFT, RIGHT) and kind in ("slider", "arc") and self.shown:
            # A twentieth of the range at a time (LVGL's keypad moves by one)
            low, high = focused.get_min_value(), focused.get_max_value()
            step = max(1, (high - low) // 20) * (1 if key == RIGHT else -1)
            value = max(low, min(high, max(low, focused.get_value()) + step))  # an arc starts below its range
            if kind == "slider":
                focused.set_value(value, False)  # no animation
            else:
                focused.set_value(value)  # an arc's takes no animation
            focused.send_event(lv.EVENT.VALUE_CHANGED, None)
            return
        if key in (LEFT, RIGHT) and (kind in _HORIZONTAL or kind in _GRID):
            pass  # the widget's own
        elif key in (UP, DOWN) and (kind in _VERTICAL or kind in _GRID or (kind == "dropdown" and focused.is_open())):
            pass
        elif key in (UP, LEFT):
            key = PREV
        elif key in (DOWN, RIGHT):
            key = NEXT
        if not self.shown:
            # The first key shows the focus, on the first widget, and does nothing else: Enter
            # does not press what could not be seen
            self.shown = True
            lv.group_focus_obj(self.members[0])
            return
        self.queue.append(key)

    def _read(self, _indev, data):
        if self.pressed is not None:
            data.key = self.pressed
            data.state = lv.INDEV_STATE.RELEASED
            self.pressed = None
        elif self.queue:
            key = self.queue.pop(0)
            data.key = key
            data.state = lv.INDEV_STATE.PRESSED
            self.pressed = key
        else:
            data.state = lv.INDEV_STATE.RELEASED


def navigate():
    """Keyboard and gamepad move between the widgets of any screen (see above), until an app
    calls listen() itself; nothing if it already did. After LVGL and the display are up."""
    global _nav
    if _timer is not None:
        return
    if _nav is None:
        _nav = _Navigation()
    listen(_nav.key)
