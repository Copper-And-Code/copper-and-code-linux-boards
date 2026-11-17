# SPDX-License-Identifier: MIT
# machine.Pin on STM32MP135F-DK without driving any line: names, errors, inputs with pulls, release.
#
#   mpremote connect /dev/ttyACM2 run tests/hil/t17_mp135_pin.py
#
# FREE_LINE must be a line no driver uses. Inputs with a pull are harmless whatever is connected; a
# line that follows both pulls is floating.

import gc

from machine import Pin

FREE_LINE = "PA4"
EBADF = 9  # Linux values; MicroPython's errno module does not name them all
EBUSY = 16

assert (Pin.IN, Pin.OUT, Pin.OPEN_DRAIN) == (0, 1, 2)
assert (Pin.PULL_UP, Pin.PULL_DOWN) == (1, 2)

try:
    Pin("NO_SUCH_LINE")
except ValueError:
    pass
else:
    raise AssertionError("an unknown name was accepted")

# B1 belongs to gpio-keys (safe mode): the kernel refuses it.
try:
    Pin("PA14", Pin.IN)
except OSError as error:
    assert error.errno == EBUSY, error
else:
    raise AssertionError("a line used by the kernel was granted")

pin = Pin(FREE_LINE, Pin.IN, Pin.PULL_UP)
print(pin)
up = pin.value()
pin.init(pull=Pin.PULL_DOWN)
down = pin.value()
print("MP135_PIN", FREE_LINE, "pull-up ->", up, "pull-down ->", down, "floating" if (up, down) == (1, 0) else "driven")
assert pin() == down

# The same line by (chip, offset) is busy while the first Pin holds it.
try:
    Pin(("gpiochip0", 4), Pin.IN)
except OSError as error:
    assert error.errno == EBUSY, error
else:
    raise AssertionError("a line was granted twice")

pin.deinit()
print(pin)
try:
    pin.value()
except OSError as error:
    assert error.errno == EBADF, error
else:
    raise AssertionError("value() worked after deinit()")

# Released by deinit(), and by the garbage collector.
again = Pin((0, 4), Pin.IN)
again = None
gc.collect()
Pin(FREE_LINE).deinit()

print("MP135_PIN_TEST_PASS")
