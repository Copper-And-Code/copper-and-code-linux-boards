include("$(PORT_DIR)/variants/standard/manifest.py")
include("$(LVMP_DIR)/manifest.py")
module("touch_evdev.py", base_path="$(BOARDS_DIR)/python/linux")
module("display_driver.py", base_path="$(BOARDS_DIR)/python/linux")
