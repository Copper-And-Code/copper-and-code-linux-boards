# SPDX-License-Identifier: MIT
# Guided two-finger touch test on STM32MP135F-DK, by hand: the panel counts down, says when to
# start and shows PRESSED or RELEASED live. Hold one finger on the button, put a second finger
# elsewhere and lift it (once or more), then lift the first finger. Expected: the state stays
# PRESSED until the first finger lifts, no MP135_TOUCH_JUMP line, one click per first-finger press.
#
#   mpremote connect /dev/ttyACM2 run tests/hil/t20_mp135_two_fingers.py

import time

import display_driver as driver
import lvgl as lv

screen = lv.obj()
screen.set_style_bg_color(lv.color_hex(0x101828), 0)
label = lv.label(screen)
label.set_text("Tap the button ten times")
label.set_pos(24, 40)
button = lv.button(screen)
button.set_size(260, 100)
button.set_pos(110, 110)
counter_label = lv.label(button)
counter_label.set_text("Clicks: 0")
counter_label.center()
count = [0]
last_state = [None]
original_read = driver._read


def traced_read(indev, data):
    started = time.ticks_us()
    original_read(indev, data)
    state = data.state
    if state != last_state[0]:
        name = "DOWN" if state == lv.INDEV_STATE.PRESSED else "UP"
        print(
            "MP135_TOUCH_STATE",
            name,
            data.point.x,
            data.point.y,
            "us",
            time.ticks_diff(time.ticks_us(), started),
        )
        last_state[0] = state


def clicked(_event):
    count[0] += 1
    counter_label.set_text("Clicks: %d" % count[0])
    print("MP135_TOUCH_CLICK", count[0])


driver.indev.set_read_cb(traced_read)
button.add_event_cb(clicked, lv.EVENT.CLICKED, None)
lv.screen_load(screen)
print("MP135_TOUCH_INTERACTIVE_READY")

# Guided two-finger session: instructions and live state on the panel.
label.set_text("Ready in 5 s...")
status = lv.label(screen)
status.set_pos(24, 225)
status.set_style_text_color(lv.color_hex(0xFFFFFF), 0)
status.set_text("")
_last_point = [None]
_inner_read = traced_read
_t0 = time.ticks_ms()


def jump_read(indev, data):
    _inner_read(indev, data)
    pressed = data.state == lv.INDEV_STATE.PRESSED
    status.set_text("PRESSED %d,%d" % (data.point.x, data.point.y) if pressed else "RELEASED")
    if pressed:
        point = (data.point.x, data.point.y)
        if _last_point[0] and (abs(point[0] - _last_point[0][0]) > 30 or abs(point[1] - _last_point[0][1]) > 30):
            print("MP135_TOUCH_JUMP", _last_point[0], "->", point, "t", time.ticks_diff(time.ticks_ms(), _t0))
        _last_point[0] = point
    else:
        _last_point[0] = None


_inner_print = print


def stamped(*args):
    _inner_print("t=%5d" % time.ticks_diff(time.ticks_ms(), _t0), *args)


print = stamped
driver.indev.set_read_cb(jump_read)
for n in (5, 4, 3, 2, 1):
    label.set_text("Ready in %d s..." % n)
    time.sleep(1)
label.set_text("GO! Hold one finger on the button.\nSecond finger elsewhere: down, up.\nThen lift the first finger.")
print("MP135_TOUCH_GO")
time.sleep(35)
label.set_text("END - clicks: %d" % count[0])
print("MP135_TOUCH_END clicks", count[0])
