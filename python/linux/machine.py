# SPDX-License-Identifier: MIT
# The machine module: MicroPython's own (umachine: Pin on Linux GPIO lines, and the rest), and
# I2C, ADC, PWM, SPI, UART and Timer from one of two backends, with the same API:
#
#   - a board with a Cortex-M4 beside Linux (STM32MP15x): the M4 serves them (mpy_m4), on time;
#   - any other board: Linux's own drivers (machine_kernel: i2c-dev, spidev, ttys, the PWM and
#     IIO frameworks, a thread for Timer), those the board's description (mpy_board) names.
#
# A class the board cannot serve is not here: hasattr(machine, "PWM") tells. MicroPython's
# machine is an "extensible" built-in: this frozen module takes its place, and umachine still
# names the built-in.

from umachine import *  # noqa: F401,F403

import mpy_m4 as _m4

if _m4.available():
    from mpy_m4 import ADC, I2C, PWM, SPI, Timer, UART  # noqa: F401
else:
    import machine_kernel as _kernel

    for _name in _kernel.offered():
        globals()[_name] = getattr(_kernel, _name)
    del _kernel, _name
del _m4
