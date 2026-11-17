# SPDX-License-Identifier: MIT
# Measure full-screen rendering throughput on STM32MP135F-DK.

import time

import display_driver
import lvgl as lv

SECONDS = 10
screen = lv.obj()
screen.set_style_border_width(0, 0)
screen.set_style_radius(0, 0)
lv.screen_load(screen)

frames = 0
pixels = 0
shade = 0
driver_flush = display_driver._flush


def measured_flush(disp, area, color_p):
    global frames, pixels
    frames += 1
    pixels += area.get_width() * area.get_height()
    driver_flush(disp, area, color_p)


def animate(_timer):
    global shade
    shade = (shade + 1) & 0xFF
    screen.set_style_bg_color(lv.color_hex((shade << 8) | shade), 0)


display_driver.display.set_flush_cb(measured_flush)
timer = lv.timer_create(animate, display_driver.REFR_PERIOD_MS, None)
start = time.ticks_ms()
time.sleep(SECONDS)
elapsed = time.ticks_diff(time.ticks_ms(), start)
timer.delete()
display_driver.display.set_flush_cb(driver_flush)

assert elapsed > 0
assert frames > 0
print("MP135_PERF_ELAPSED_MS", elapsed)
print("MP135_PERF_FRAMES", frames)
print("MP135_PERF_FPS", frames * 1000 / elapsed)
print("MP135_PERF_FULLFRAME_PERCENT", pixels * 100 // (frames * display_driver.WIDTH * display_driver.HEIGHT))
print("MP135_PERF_TEST_PASS")
