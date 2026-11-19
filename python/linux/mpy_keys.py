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
