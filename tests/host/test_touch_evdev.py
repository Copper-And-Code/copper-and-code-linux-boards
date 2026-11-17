# SPDX-License-Identifier: MIT
# Run from the repository root with: python3 tests/host/test_touch_evdev.py

import struct
import sys

sys.path.insert(0, "python/linux")
import touch_evdev as evdev


class FakeFile:
    def __init__(self, chunks):
        self.chunks = list(chunks)

    def read(self, size):
        assert size == evdev.EVENT_SIZE, "reads must request exactly one evdev event"
        if not self.chunks:
            return b""
        data, self.chunks[0] = self.chunks[0][:size], self.chunks[0][size:]
        if not self.chunks[0]:
            self.chunks.pop(0)
        return data


class FakePoller:
    def __init__(self, source):
        self.source = source

    def poll(self, _timeout):
        return [(0, 1)] if self.source.chunks else []


def event(event_type, code, value):
    return struct.pack(evdev.EVENT_FORMAT, 0, 0, event_type, code, value)


def touch_with(chunks):
    touch = evdev.TouchEvdev(480, 272, path="/dev/null")
    touch.close()
    touch.file = FakeFile(chunks)
    touch.poller = FakePoller(touch.file)
    touch.x = 0
    touch.y = 0
    touch.pressed = False
    touch._pending = b""
    return touch


def frame(pressed, x=100, y=120):
    tracking = 1 if pressed else -1
    return b"".join(
        (
            event(evdev.EV_ABS, evdev.ABS_MT_TRACKING_ID, tracking),
            event(evdev.EV_ABS, evdev.ABS_MT_POSITION_X, x),
            event(evdev.EV_ABS, evdev.ABS_MT_POSITION_Y, y),
            event(evdev.EV_SYN, 0, 0),
        )
    )


quick_tap = touch_with([frame(True) + frame(False)])
assert quick_tap.state() == (100, 120, True), "LVGL must observe the press"
assert quick_tap.pending, "the release frame must remain queued"
assert quick_tap.state() == (100, 120, False), "LVGL must then observe the release"
assert not quick_tap.pending
assert quick_tap.state() == (100, 120, False)

partial = touch_with([frame(True)[:7]])
assert partial.state() == (0, 0, False), "an incomplete frame must not be applied"
partial.file.chunks.append(frame(True)[7:])
assert partial.state() == (100, 120, True)
assert not partial.pending

coordinates = touch_with([frame(True, 20, 30) + frame(True, 200, 210)])
assert coordinates.state() == (20, 30, True)
assert coordinates.pending
assert coordinates.state() == (200, 210, True)
assert not coordinates.pending

print("TOUCH_EVDEV_TEST_PASS")
