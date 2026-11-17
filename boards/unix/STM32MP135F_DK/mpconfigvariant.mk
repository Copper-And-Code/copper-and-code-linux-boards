FROZEN_MANIFEST ?= $(VARIANT_DIR)/manifest.py
MICROPY_PY_FFI := 1
# display_driver runs lv.timer_handler() in a thread while main.py and the REPL call LVGL:
# with the GIL every LVGL call is atomic, as with the scheduled callbacks of the MCU ports.
MICROPY_PY_THREAD_GIL := 1
USER_C_MODULES += $(VARIANT_DIR)/../../../modules/mpyrepl
USER_C_MODULES += $(VARIANT_DIR)/../../../modules/linuxfb
USER_C_MODULES += $(VARIANT_DIR)/../../../modules/linuxproc
LDFLAGS += -Wl,--wrap=mp_hal_set_interrupt_char
