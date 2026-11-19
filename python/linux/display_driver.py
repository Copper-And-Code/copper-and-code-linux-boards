# SPDX-License-Identifier: MIT
# Linux boards: the screen through a DRM dumb buffer (RGB565, the connector's preferred mode), the
# first touchscreen and the first mouse through evdev, the first backlight in sysfs. display_driver
# contract 1.
#
# The screen: a monitor connected at start (HDMI, DisplayPort, ...) wins over the board's own panel
# (DSI, LVDS, ...). Plugging one in or unplugging it later starts MicroPython again on the other
# screen (a soft reset, back to the shell). On a monitor the panel's backlight goes off, its touch
# and its rotation are not used, and a mouse is the pointer.
#
# Environment: MPY_DISPLAY_CONNECTOR (a name as in /sys/class/drm, such as HDMI-A-1 or DSI-1: that
# screen only, no switching), MPY_DISPLAY_MODE (WIDTHxHEIGHT, instead of the preferred mode),
# MPY_BACKLIGHT (a /sys/class/backlight/... directory), MPY_TOUCH_NAME, MPY_TOUCH_RANGE,
# MPY_AUDIO_SINK_MONITOR (the GStreamer sink for the sound on a monitor), MPY_APP_SIZE (set by
# mpy_home: see below), MPY_DISPLAY_ROTATION (the panel's: 0, 90, 180 or 270; for example 90 turns a 480x800 portrait
# panel into an 800x480 landscape screen; LVGL renders turned and linuxfb.blit_rotated() turns it
# back).

import _thread
import os
import time

import lvgl as lv
import linuxfb
import mouse_evdev
import touch_evdev

CONTRACT = 1
PERIOD_MS = 5
REFR_PERIOD_MS = 16
BUFFERS = 1


def _backlight_path():
    path = os.getenv("MPY_BACKLIGHT")
    if path:
        return path
    try:
        names = sorted(os.listdir("/sys/class/backlight"))
    except OSError:
        return None
    return "/sys/class/backlight/" + names[0] if names else None


BACKLIGHT_PATH = _backlight_path()

_WANTED = os.getenv("MPY_DISPLAY_CONNECTOR") or None
_drm_width, _drm_height, _bpp, _stride = linuxfb.open(connector=_WANTED, mode=os.getenv("MPY_DISPLAY_MODE"))
CONNECTOR, INTERNAL = linuxfb.connector()
if _bpp != 16:
    linuxfb.close()
    raise RuntimeError("unexpected DRM mode: %sx%s, %s bpp" % (_drm_width, _drm_height, _bpp))
ROTATION = int(os.getenv("MPY_DISPLAY_ROTATION") or 0) if INTERNAL else 0
# The sound follows the screen: on a monitor MPY_AUDIO_SINK_MONITOR (the board's HDMI audio), on
# the panel the MPY_AUDIO_SINK the service started with, kept in MPY_AUDIO_SINK_PANEL across soft
# resets. Players read MPY_AUDIO_SINK after this module is imported.
if os.getenv("MPY_AUDIO_SINK_PANEL") is None:
    os.putenv("MPY_AUDIO_SINK_PANEL", os.getenv("MPY_AUDIO_SINK") or "")
if os.getenv("MPY_AUDIO_SINK_MONITOR"):
    os.putenv("MPY_AUDIO_SINK", os.getenv("MPY_AUDIO_SINK_PANEL") if INTERNAL else os.getenv("MPY_AUDIO_SINK_MONITOR"))
if ROTATION not in (0, 90, 180, 270):
    linuxfb.close()
    raise ValueError("MPY_DISPLAY_ROTATION must be 0, 90, 180 or 270")
PANEL_WIDTH = _drm_width
PANEL_HEIGHT = _drm_height
# What LVGL and the application see.
WIDTH, HEIGHT = (_drm_height, _drm_width) if ROTATION in (90, 270) else (_drm_width, _drm_height)
# An app made for a smaller screen runs at its own size in the middle of a monitor, on black:
# mpy_home sets MPY_APP_SIZE (WIDTHxHEIGHT) from the app's board profile.
OX = OY = 0
_app_size = os.getenv("MPY_APP_SIZE")
if _app_size and not INTERNAL:
    _w, _, _h = _app_size.lower().partition("x")
    _w, _h = int(_w), int(_h)
    if _w <= WIDTH and _h <= HEIGHT:
        OX, OY = (WIDTH - _w) // 2, (HEIGHT - _h) // 2
        WIDTH = PANEL_WIDTH = _w
        HEIGHT = PANEL_HEIGHT = _h

if not lv.is_initialized():
    lv.init()
display = lv.display_create(PANEL_WIDTH, PANEL_HEIGHT)
display.set_color_format(lv.COLOR_FORMAT.RGB565)
# A full-screen buffer up to 800x480 or so; a monitor's 1280x720 (1.8 MB) would not fit in the heap.
DIRECT = ROTATION == 0 and WIDTH * HEIGHT * 2 <= 1024 * 1024
if DIRECT:
    # One full-screen buffer copied to the panel as it is: only the changed rectangles move.
    _buffer = bytearray(WIDTH * HEIGHT * 2)
    display.set_buffers(_buffer, None, len(_buffer), lv.DISPLAY_RENDER_MODE.DIRECT)
elif ROTATION == 0:
    # A strip of the screen at a time, each copied to its place.
    _buffer = bytearray(WIDTH * HEIGHT // 8 * 2)
    display.set_buffers(_buffer, None, len(_buffer), lv.DISPLAY_RENDER_MODE.PARTIAL)
else:
    # LVGL renders a part of the screen at a time in the turned orientation; blit_rotated() turns
    # each part into the panel's own while it copies it.
    _buffer = bytearray(WIDTH * HEIGHT // 4 * 2)
    display.set_buffers(_buffer, None, len(_buffer), lv.DISPLAY_RENDER_MODE.PARTIAL)
    display.set_rotation(getattr(lv.DISPLAY_ROTATION, "_%d" % ROTATION))
display.get_refr_timer().set_period(REFR_PERIOD_MS)


def _flush(disp, area, _color_p):
    x, y = area.x1, area.y1
    if ROTATION:
        linuxfb.blit_rotated(x, y, area.get_width(), area.get_height(), _buffer, ROTATION)
    elif DIRECT:
        offset = (y * WIDTH + x) * 2
        linuxfb.blit(x + OX, y + OY, area.get_width(), area.get_height(), memoryview(_buffer)[offset:], WIDTH * 2)
    else:
        linuxfb.blit(x + OX, y + OY, area.get_width(), area.get_height(), _buffer, area.get_width() * 2)
    disp.flush_ready()


display.set_flush_cb(_flush)

touch = None
if INTERNAL:
    try:
        # The panel's own coordinates: LVGL turns them with the display.
        touch = touch_evdev.TouchEvdev(PANEL_WIDTH, PANEL_HEIGHT)
    except OSError as error:
        print("display_driver: no touch:", error)
mouse = mouse_evdev.MouseEvdev(WIDTH, HEIGHT)


def _read(_indev, data):
    if touch is not None:
        x, y, pressed = touch.state()
        data.point.x = x
        data.point.y = y
        data.state = lv.INDEV_STATE.PRESSED if pressed else lv.INDEV_STATE.RELEASED
        data.continue_reading = touch.pending


indev = lv.indev_create()
indev.set_type(lv.INDEV_TYPE.POINTER)
indev.set_display(display)
indev.set_read_cb(_read)


def _panel_point(x, y):
    # The mouse moves on the screen as the user sees it; LVGL turns every pointer from the panel's
    # coordinates (lv_display_rotate_point), so hand it the point it turns back into this one.
    if ROTATION == 90:
        return y, PANEL_HEIGHT - 1 - x
    if ROTATION == 180:
        return PANEL_WIDTH - 1 - x, PANEL_HEIGHT - 1 - y
    if ROTATION == 270:
        return PANEL_WIDTH - 1 - y, x
    return x, y


def _read_mouse(_indev, data):
    x, y, pressed = mouse.state()
    if mouse.moved and _cursor.has_flag(lv.obj.FLAG.HIDDEN):
        _cursor.remove_flag(lv.obj.FLAG.HIDDEN)
    data.point.x, data.point.y = _panel_point(x, y)
    data.state = lv.INDEV_STATE.PRESSED if pressed else lv.INDEV_STATE.RELEASED


# The mouse's pointer, in screen coordinates: hidden until a mouse moves.
mouse_indev = lv.indev_create()
mouse_indev.set_type(lv.INDEV_TYPE.POINTER)
mouse_indev.set_display(display)
mouse_indev.set_read_cb(_read_mouse)
_cursor = lv.obj(lv.layer_sys())
_cursor.set_size(12, 12)
_cursor.set_style_radius(6, 0)
_cursor.set_style_bg_color(lv.color_hex(0xFFFFFF), 0)
_cursor.set_style_border_color(lv.color_hex(0x000000), 0)
_cursor.set_style_border_width(2, 0)
_cursor.set_style_pad_all(0, 0)
_cursor.remove_flag(lv.obj.FLAG.CLICKABLE)
_cursor.add_flag(lv.obj.FLAG.HIDDEN)
mouse_indev.set_cursor(_cursor)


# Internal panels by the names of their connectors in /sys/class/drm.
_PANEL_TYPES = ("DSI-", "LVDS-", "eDP-", "DPI-", "SPI-", "Writeback-")
WATCH_MS = 2000


def _monitor_connected():
    """True when a screen other than the board's panel is connected, by sysfs (no probing)."""
    try:
        names = os.listdir("/sys/class/drm")
    except OSError:
        return False
    for name in names:
        connector = name.partition("-")[2]
        if not name.startswith("card") or not connector or connector.startswith(_PANEL_TYPES):
            continue
        try:
            with open("/sys/class/drm/%s/status" % name) as source:
                if source.read().strip() == "connected":
                    return True
        except OSError:
            pass
    return False


_MONITOR_AT_START = _monitor_connected()


def _watch_screens():
    # Plugged in or unplugged: start again, and the new process opens the other screen. A change,
    # not the state: a monitor with no mode the board can drive leaves the panel on, once.
    if _WANTED is None and _monitor_connected() != _MONITOR_AT_START:
        print("display_driver: screen changed, starting again")
        import mpyrepl

        mpyrepl.restart()

_running = False
_active = False
_service_id = None


def _service():
    global _running, _active, _service_id
    _service_id = _thread.get_ident()
    previous = watched = time.ticks_ms()
    try:
        while _running:
            now = time.ticks_ms()
            if time.ticks_diff(now, watched) >= WATCH_MS:
                watched = now
                _watch_screens()
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
    # Without a backlight in sysfs the panel is always on: 100, and setting does nothing. On a
    # monitor too: the panel's backlight is not the monitor's (its own buttons set it).
    if BACKLIGHT_PATH is None or not INTERNAL:
        return 100 if percent is None else None
    with open(BACKLIGHT_PATH + "/max_brightness") as source:
        maximum = int(source.read())
    if percent is None:
        with open(BACKLIGHT_PATH + "/brightness") as source:
            return int(source.read()) * 100 // maximum
    value = max(0, min(int(percent), 100))
    with open(BACKLIGHT_PATH + "/brightness", "w") as target:
        target.write(str((value * maximum + 50) // 100))


try:
    if INTERNAL:
        backlight(100)
    elif BACKLIGHT_PATH is not None:  # on a monitor the panel is dark
        with open(BACKLIGHT_PATH + "/brightness", "w") as target:
            target.write("0")
except OSError as error:
    print("display_driver: no backlight control:", error)
start()
