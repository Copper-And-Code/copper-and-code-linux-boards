# SPDX-License-Identifier: MIT
# machine.Pin outputs on STM32MP135F-DK, with a jumper between two pins of the GPIO expansion
# connector. Not run automatically: set OUT_LINE and IN_LINE to two free connector pins, checked
# on UM2993 and the MB1635 schematic, fit the jumper, then
#
#   mpremote connect /dev/ttyACM2 run tests/hil/t18_mp135_pin_loopback.py
#
# The input line is requested first, so the two pins are never both outputs.

from machine import Pin

OUT_LINE = None  # for example "PXn", from UM2993
IN_LINE = None

if OUT_LINE is None or IN_LINE is None:
    raise SystemExit("set OUT_LINE and IN_LINE to two connector pins joined by a jumper")

sense = Pin(IN_LINE, Pin.IN)
drive = Pin(OUT_LINE, Pin.OUT, value=0)
for level in (1, 0, 1, 0):
    drive.value(level)
    assert sense.value() == level, (level, sense.value())
drive.toggle()
assert sense() == 1
drive.off()
assert sense() == 0

# Open drain: low is driven, high is released and the input's pull-up decides.
drive.init(Pin.OPEN_DRAIN, value=1)
sense.init(pull=Pin.PULL_UP)
assert sense() == 1
drive.off()
assert sense() == 0
sense.init(pull=Pin.PULL_DOWN)
drive.on()
assert sense() == 0, "an open-drain high must not drive the line"

drive.deinit()
sense.deinit()
print("MP135_PIN_LOOPBACK_TEST_PASS")
