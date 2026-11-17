# SPDX-License-Identifier: MIT
# Physical STM32MP135F-DK touch test: every press/release is printed and every click
# increments the visible counter. Run with mpremote and tap the button ten times.

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
