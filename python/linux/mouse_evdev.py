# SPDX-License-Identifier: MIT
# A mouse (USB or any evdev device with relative X/Y) as LVGL's pointer, on any Linux board.
#
# The device is the first the kernel lists with REL_X and REL_Y. One plugged in later is found
# within two seconds, and an unplugged one is let go. The pointer stays inside the screen; the left
# button presses, the wheel is not used.

import os
import select
import struct
import time

from touch_evdev import EVENT_FORMAT, EVENT_SIZE, _bits

EV_KEY = 0x01
EV_REL = 0x02
REL_X = 0x00
REL_Y = 0x01
BTN_LEFT = 0x110
LOOK_MS = 2000


def find_mouse():
    with open("/proc/bus/input/devices") as devices:
        blocks = devices.read().split("\n\n")
    for block in blocks:
        relative = event = None
        for line in block.splitlines():
            if line.startswith("B: REL="):
                relative = _bits(line)
            elif line.startswith("H: Handlers="):
                for handler in line.split("=", 1)[1].split():
                    if handler.startswith("event"):
                        event = "/dev/input/" + handler
        if event is not None and relative is not None and relative & 3 == 3:
            return event
    return None


class MouseEvdev:
    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.x = width // 2
        self.y = height // 2
        self.pressed = False
        self.moved = False  # once it moves, the cursor shows
        self.file = None
        self.poller = None
        self._looked = None

    def _open(self):
        now = time.ticks_ms()
        if self._looked is not None and time.ticks_diff(now, self._looked) < LOOK_MS:
            return
        self._looked = now
        try:
            path = find_mouse()
            if path:
                self.file = open(path, "rb", buffering=0)
                self.poller = select.poll()
                self.poller.register(self.file.fileno(), select.POLLIN)
        except OSError:
            self.file = None

    def _close(self):
        try:
            self.file.close()
        except OSError:
            pass
        self.file = self.poller = None
        self.pressed = False

    def flush(self):
        """Drops the events waiting: clicks made while another program had the screen."""
        try:
            while self.file is not None and self.poller.poll(0):
                if not self.file.read(EVENT_SIZE):
                    break
        except OSError:
            self._close()
        self.pressed = False

    def state(self):
        """(x, y, pressed) after the events waiting; looks for a mouse while there is none."""
        if self.file is None:
            self._open()
            if self.file is None:
                return self.x, self.y, False
        try:
            # One event a read: MicroPython's read(n) waits for all n bytes, and this runs in the
            # display's thread.
            while self.poller.poll(0):
                data = self.file.read(EVENT_SIZE)
                if not data:
                    break
                _s, _u, kind, code, value = struct.unpack(EVENT_FORMAT, data)
                if kind == EV_REL and code == REL_X:
                    self.x = max(0, min(self.x + value, self.width - 1))
                    self.moved = True
                elif kind == EV_REL and code == REL_Y:
                    self.y = max(0, min(self.y + value, self.height - 1))
                    self.moved = True
                elif kind == EV_KEY and code == BTN_LEFT:
                    self.pressed = value != 0
                    if self.pressed:
                        break  # LVGL sees the press before a release waiting behind it
        except OSError:  # unplugged: ENODEV
            self._close()
        return self.x, self.y, self.pressed
