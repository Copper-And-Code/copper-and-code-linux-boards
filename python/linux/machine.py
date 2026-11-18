# SPDX-License-Identifier: MIT
# The machine module: MicroPython's own (umachine: Pin on Linux GPIO lines, and the rest), and
# on a board with a Cortex-M4 (STM32MP15x) I2C, ADC, PWM, SPI, UART and Timer served by the M4 (mpy_m4).
#
# MicroPython's machine is an "extensible" built-in: this frozen module takes its place, and
# umachine still names the built-in.

from umachine import *  # noqa: F401,F403

import mpy_m4 as _m4

if _m4.available():
    from mpy_m4 import ADC, I2C, PWM, SPI, Timer, UART  # noqa: F401
del _m4
