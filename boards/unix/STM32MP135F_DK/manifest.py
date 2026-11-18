# SPDX-License-Identifier: MIT
include("$(PORT_DIR)/variants/standard/manifest.py")
include("$(LVMP_DIR)/manifest.py")
module("touch_evdev.py", base_path="$(BOARDS_DIR)/python/linux")
module("display_driver.py", base_path="$(BOARDS_DIR)/python/linux")
module("mpy_ready.py", base_path="$(BOARDS_DIR)/python/linux")
module("mpy_settings.py", base_path="$(BOARDS_DIR)/python/linux")
module("mpy_wifi.py", base_path="$(BOARDS_DIR)/python/linux")
module("mpy_home.py", base_path="$(BOARDS_DIR)/python/linux")
# The shell of the home screen; its editors stay files in fs/bin, run as programs.
module("shell_linux.py", base_path="$(BOARDS_DIR)/examples/shell-linux")
module("ansiterm.py", base_path="$(BOARDS_DIR)/examples/shell-linux")
module("ansifont.py", base_path="$(BOARDS_DIR)/examples/shell-linux")
# machine: MicroPython's own, and I2C and ADC served by a Cortex-M4 where there is one (mpy_m4).
module("machine.py", base_path="$(BOARDS_DIR)/python/linux")
module("mpy_m4.py", base_path="$(BOARDS_DIR)/python/linux")
