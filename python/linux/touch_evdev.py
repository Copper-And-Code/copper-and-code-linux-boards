# SPDX-License-Identifier: MIT
# Goodix touch input through the Linux event interface on 32-bit ARM.

import select
import struct

EV_SYN = 0x00
EV_ABS = 0x03
ABS_X = 0x00
ABS_Y = 0x01
ABS_MT_POSITION_X = 0x35
ABS_MT_POSITION_Y = 0x36
ABS_MT_TRACKING_ID = 0x39
BTN_TOUCH = 0x14A
EVENT_FORMAT = "llHHi"
EVENT_SIZE = struct.calcsize(EVENT_FORMAT)


def find_touch(name="Goodix Capacitive TouchScreen"):
    with open("/proc/bus/input/devices") as devices:
        blocks = devices.read().split("\n\n")
    for block in blocks:
        if 'N: Name="' + name + '"' not in block:
            continue
        for line in block.splitlines():
            if line.startswith("H: Handlers="):
                for handler in line.split("=", 1)[1].split():
                    if handler.startswith("event"):
                        return "/dev/input/" + handler
    raise OSError("touch input not found: " + name)


class TouchEvdev:
    def __init__(self, width=480, height=272, path=None):
        self.width = width
        self.height = height
        self.path = path or find_touch()
        self.file = open(self.path, "rb", buffering=0)
        self.fd = self.file.fileno()
        self.poller = select.poll()
        self.poller.register(self.fd, select.POLLIN)
        self.x = 0
        self.y = 0
        self.pressed = False
        self._pending = b""

    def _coordinate(self, value, limit):
        return max(0, min(value, limit - 1))

    def poll(self):
        if not self.poller.poll(0):
            return False
        events = self.file.read(EVENT_SIZE * 32)
        if not events:
            return False
        self._pending += events
        complete = len(self._pending) - len(self._pending) % EVENT_SIZE
        packet, self._pending = self._pending[:complete], self._pending[complete:]
        changed = False
        for offset in range(0, complete, EVENT_SIZE):
            _sec, _usec, event_type, code, value = struct.unpack_from(EVENT_FORMAT, packet, offset)
            if event_type == EV_ABS:
                if code in (ABS_X, ABS_MT_POSITION_X):
                    self.x = self._coordinate(value, self.width)
                    changed = True
                elif code in (ABS_Y, ABS_MT_POSITION_Y):
                    self.y = self._coordinate(value, self.height)
                    changed = True
                elif code == ABS_MT_TRACKING_ID:
                    self.pressed = value >= 0
                    changed = True
            elif event_type == 0x01 and code == BTN_TOUCH:
                self.pressed = bool(value)
                changed = True
            elif event_type == EV_SYN:
                changed = True
        return changed

    def state(self):
        self.poll()
        return self.x, self.y, self.pressed

    def close(self):
        if self.file is not None:
            self.poller.unregister(self.fd)
            self.file.close()
            self.file = None
