# SPDX-License-Identifier: MIT
# Run on STM32MP135F-DK; set DISPLAY_TEST_SECONDS to keep the image visible.

import os
import time

import display_driver
import lvgl as lv

assert display_driver.CONTRACT == 1
assert (display_driver.WIDTH, display_driver.HEIGHT) == (480, 272)
assert display_driver.BUFFERS == 1
assert display_driver.indev is not None
assert display_driver.running()
assert display_driver.backlight() == 100

screen = lv.obj()
screen.set_style_bg_color(lv.color_hex(0x202020), 0)
screen.set_style_border_width(0, 0)
colors = (0xF80000, 0x00C000, 0x0000FF, 0xFFFFFF)
for index, color in enumerate(colors):
    bar = lv.obj(screen)
    bar.set_pos(index * 120, 0)
    bar.set_size(120, 272)
    bar.set_style_bg_color(lv.color_hex(color), 0)
    bar.set_style_bg_opa(255, 0)
    bar.set_style_border_width(0, 0)
    bar.set_style_radius(0, 0)
label = lv.label(screen)
label.set_text("STM32MP135F-DK  LVGL")
label.set_style_text_color(lv.color_hex(0xFFFF00), 0)
label.set_style_bg_color(lv.color_hex(0x000000), 0)
label.set_style_bg_opa(180, 0)
label.set_pos(8, 8)
lv.screen_load(screen)

print("MP135_DISPLAY_READY 480x272 RGB565 touch=True")
time.sleep(int(os.getenv("DISPLAY_TEST_SECONDS") or 5))
display_driver.stop()
assert not display_driver.running()
display_driver.start()
assert display_driver.running()
print("MP135_DISPLAY_TEST_PASS")
