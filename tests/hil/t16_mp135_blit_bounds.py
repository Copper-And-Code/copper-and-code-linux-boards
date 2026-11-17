# SPDX-License-Identifier: MIT
# linuxfb.blit() rejects every rectangle, stride and buffer that would reach outside the panel or
# the buffer, including values chosen to overflow 32-bit arithmetic. Run with display_driver loaded.

import display_driver
import linuxfb

W, H = display_driver.WIDTH, display_driver.HEIGHT
small = bytearray(W * 2)
cases = [
    (0, 0, W + 1, 1, small, 0),
    (1, 0, 0x7FFFFFFF, 1, small, 0),
    (0, 1, 1, 0x7FFFFFFF, small, 0),
    (W - 1, 0, 2, 1, small, 0),
    (0, H - 1, 1, 2, small, 0),
    (-1, 0, 1, 1, small, 0),
    (0, 0, 1, 1, small, 1),
    (0, 0, 1, H, small, 0x0F1D4C00),
    (0, 0, W, 2, small, W * 2),
]
for case in cases:
    try:
        linuxfb.blit(*case)
    except ValueError:
        pass
    else:
        raise AssertionError("accepted %r" % (case[:4] + (case[5],)))
linuxfb.blit(0, 0, W, 1, small, W * 2)
linuxfb.blit(W - 1, H - 1, 1, 1, small, 0)
print("MP135_BLIT_BOUNDS_TEST_PASS")
