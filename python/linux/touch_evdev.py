# SPDX-License-Identifier: MIT
# Goodix touch input through the Linux event interface on 32-bit ARM.

import select
import struct

EV_SYN = 0x00
EV_KEY = 0x01
SYN_REPORT = 0x00
SYN_DROPPED = 0x03
EV_ABS = 0x03
ABS_X = 0x00
ABS_Y = 0x01
ABS_MT_SLOT = 0x2F
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
        # The kernel's single-pointer emulation (ABS_X/ABS_Y/BTN_TOUCH) follows the oldest contact;
        # once seen, the per-slot multitouch events are ignored, so a second finger cannot move
        # or release the pointer.
        self._pointer = False
        # After SYN_DROPPED the kernel's buffer overflowed: drop events up to the next SYN_REPORT.
        self._dropping = False

    def _coordinate(self, value, limit):
        return max(0, min(value, limit - 1))

    def _report_end(self):
        complete = len(self._pending) - len(self._pending) % EVENT_SIZE
        for offset in range(0, complete, EVENT_SIZE):
            event_type = struct.unpack_from(EVENT_FORMAT, self._pending, offset)[2]
            if event_type == EV_SYN:
                return offset + EVENT_SIZE
        return 0

    def poll(self):
        end = self._report_end()
        while not end:
            if not self.poller.poll(0):
                return False
            events = self.file.read(EVENT_SIZE)
            if not events:
                return False
            self._pending += events
            end = self._report_end()
        packet, self._pending = self._pending[:end], self._pending[end:]
        events = [struct.unpack_from(EVENT_FORMAT, packet, offset)[2:] for offset in range(0, end, EVENT_SIZE)]
        event_type, code, _value = events[-1]
        if code == SYN_DROPPED:
            self._dropping = True
            return False
        if self._dropping:
            self._dropping = False
            return False
        if not self._pointer:
            self._pointer = any(
                (event_type == EV_ABS and code in (ABS_X, ABS_Y)) or (event_type == EV_KEY and code == BTN_TOUCH)
                for event_type, code, _value in events
            )
        slot = 0
        changed = False
        for event_type, code, value in events:
            if event_type == EV_ABS:
                if code == ABS_MT_SLOT:
                    slot = value
                elif code in (ABS_X, ABS_Y):
                    if code == ABS_X:
                        self.x = self._coordinate(value, self.width)
                    else:
                        self.y = self._coordinate(value, self.height)
                    changed = True
                elif self._pointer or slot != 0:
                    continue
                elif code == ABS_MT_POSITION_X:
                    self.x = self._coordinate(value, self.width)
                    changed = True
                elif code == ABS_MT_POSITION_Y:
                    self.y = self._coordinate(value, self.height)
                    changed = True
                elif code == ABS_MT_TRACKING_ID:
                    self.pressed = value >= 0
                    changed = True
            elif event_type == EV_KEY and code == BTN_TOUCH:
                self.pressed = bool(value)
                changed = True
            elif event_type == EV_SYN:
                changed = True
        return changed

    @property
    def pending(self):
        return self._report_end() != 0 or bool(self.poller.poll(0))

    def state(self):
        self.poll()
        return self.x, self.y, self.pressed

    def close(self):
        if self.file is not None:
            self.poller.unregister(self.fd)
            self.file.close()
            self.file = None
