# SPDX-License-Identifier: MIT
# machine.Pin outputs on STM32MP135F-DK, checked by eye: the green LED LD7 and the orange LED LD6
# are on IO14 and IO15 of the MCP23017 I/O expander, active high (UM2993 Rev 2, table 11).
#
#   mpremote connect /dev/ttyACM2 run tests/hil/t19_mp135_led.py
#
# Expected: green blinks 5 times, then orange blinks 5 times, then both stay off.

import time

from machine import Pin

green = Pin(("mcp23017", 14), Pin.OUT, value=0)
orange = Pin(("mcp23017", 15), Pin.OUT, value=0)
print(green, orange)
for led, name in ((green, "green"), (orange, "orange")):
    print("MP135_LED", name, "blinking")
    for _ in range(5):
        led.on()
        time.sleep_ms(300)
        led.off()
        time.sleep_ms(300)
    assert led.value() == 0
green.deinit()
orange.deinit()
print("MP135_LED_TEST_DONE: confirm by eye")
