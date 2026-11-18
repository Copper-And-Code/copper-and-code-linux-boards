# SPDX-License-Identifier: MIT
# A tap on the touchscreen from a program, for working on the board from afar:
#
#   micropython /usr/local/mpy/lib/mpy_tap.py X Y [MILLISECONDS]
#
# X and Y are screen coordinates as LVGL and the user see them (after MPY_DISPLAY_ROTATION). The
# events are written into the touchscreen's own event device, so display_driver reads them as a
# finger: press, hold (100 ms by default), release. Needs root.

import os
import struct
import sys
import time

import touch_evdev as t

FORMAT = t.EVENT_FORMAT
BTN_TOUCH = 0x14A
EV_KEY = 0x01


def _panel_size():
    # The panel's own size, from the connected connector's preferred mode (for example 480x800):
    # the touchscreen reports in it. The display is not touched.
    root = "/sys/class/drm"
    for name in sorted(os.listdir(root)):
        try:
            with open("%s/%s/status" % (root, name)) as f:
                if f.read().strip() != "connected":
                    continue
            with open("%s/%s/modes" % (root, name)) as f:
                width, height = f.readline().strip().split("x")
            return int(width), int(height)
        except (OSError, ValueError):
            pass
    raise OSError("no connected display in " + root)


def tap(x, y, hold_ms=100, rotation=None, panel=None):
    rotation = int(os.getenv("MPY_DISPLAY_ROTATION") or 0) if rotation is None else rotation
    width, height = panel or _panel_size()
    # The inverse of lv_display_rotate_point(): from the turned screen back to the panel.
    if rotation == 90:
        px, py = y, height - 1 - x
    elif rotation == 180:
        px, py = width - 1 - x, height - 1 - y
    elif rotation == 270:
        px, py = width - 1 - y, x
    else:
        px, py = x, y
    with open(t.find_touch(), "wb") as device:

        def send(kind, code, value):
            device.write(struct.pack(FORMAT, 0, 0, kind, code, value))

        send(t.EV_ABS, t.ABS_MT_SLOT, 0)
        send(t.EV_ABS, t.ABS_MT_TRACKING_ID, 1)
        send(t.EV_ABS, t.ABS_MT_POSITION_X, px)
        send(t.EV_ABS, t.ABS_MT_POSITION_Y, py)
        send(EV_KEY, BTN_TOUCH, 1)
        send(t.EV_ABS, t.ABS_X, px)
        send(t.EV_ABS, t.ABS_Y, py)
        send(t.EV_SYN, 0, 0)
        time.sleep_ms(hold_ms)
        send(t.EV_ABS, t.ABS_MT_TRACKING_ID, -1)
        send(EV_KEY, BTN_TOUCH, 0)
        send(t.EV_SYN, 0, 0)
    return px, py


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: mpy_tap.py X Y [MILLISECONDS]")
        sys.exit(2)
    print("tap at panel", tap(int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) > 3 else 100))
