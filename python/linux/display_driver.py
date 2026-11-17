# SPDX-License-Identifier: MIT
# STM32MP135F-DK: DRM RGB565 panel and Goodix touch, contract 1.

import _thread
import time

import lvgl as lv
import linuxfb
import touch_evdev

CONTRACT = 1
PERIOD_MS = 5
REFR_PERIOD_MS = 16
BUFFERS = 1
BACKLIGHT_PATH = "/sys/class/backlight/panel-backlight"

_drm_width, _drm_height, _bpp, _stride = linuxfb.open()
if (_drm_width, _drm_height, _bpp) != (480, 272, 16):
    raise RuntimeError("unexpected DRM mode: %sx%s, %s bpp" % (_drm_width, _drm_height, _bpp))
WIDTH = _drm_width
HEIGHT = _drm_height

if not lv.is_initialized():
    lv.init()
_buffer = bytearray(WIDTH * HEIGHT * 2)
display = lv.display_create(WIDTH, HEIGHT)
display.set_color_format(lv.COLOR_FORMAT.RGB565)
display.set_buffers(_buffer, None, len(_buffer), lv.DISPLAY_RENDER_MODE.DIRECT)
display.get_refr_timer().set_period(REFR_PERIOD_MS)


def _flush(disp, area, _color_p):
    x, y = area.x1, area.y1
    offset = (y * WIDTH + x) * 2
    linuxfb.blit(x, y, area.get_width(), area.get_height(), memoryview(_buffer)[offset:], WIDTH * 2)
    disp.flush_ready()


display.set_flush_cb(_flush)

try:
    touch = touch_evdev.TouchEvdev(WIDTH, HEIGHT)
except OSError as error:
    touch = None
    print("display_driver: no touch:", error)


def _read(_indev, data):
    if touch is not None:
        x, y, pressed = touch.state()
        data.point.x = x
        data.point.y = y
        data.state = lv.INDEV_STATE.PRESSED if pressed else lv.INDEV_STATE.RELEASED


indev = lv.indev_create()
indev.set_type(lv.INDEV_TYPE.POINTER)
indev.set_display(display)
indev.set_read_cb(_read)

_running = False
_active = False
_service_id = None


def _service():
    global _running, _active, _service_id
    _service_id = _thread.get_ident()
    previous = time.ticks_ms()
    try:
        while _running:
            now = time.ticks_ms()
            lv.tick_inc(time.ticks_diff(now, previous))
            previous = now
            lv.timer_handler()
            time.sleep_ms(PERIOD_MS)
    finally:
        _running = False
        _active = False


def start():
    global _running, _active
    if not _running and not _active:
        _running = True
        _active = True
        _thread.start_new_thread(_service, ())


def stop():
    global _running
    _running = False
    while _active and _thread.get_ident() != _service_id:
        time.sleep_ms(1)


def running():
    return _running


def backlight(percent=None):
    with open(BACKLIGHT_PATH + "/max_brightness") as source:
        maximum = int(source.read())
    if percent is None:
        with open(BACKLIGHT_PATH + "/brightness") as source:
            return int(source.read()) * 100 // maximum
    value = max(0, min(int(percent), 100))
    with open(BACKLIGHT_PATH + "/brightness", "w") as target:
        target.write(str((value * maximum + 50) // 100))


backlight(100)
start()
