FROZEN_MANIFEST ?= $(VARIANT_DIR)/manifest.py
MICROPY_PY_FFI := 1
USER_C_MODULES += $(VARIANT_DIR)/../../../modules/mpyrepl
USER_C_MODULES += $(VARIANT_DIR)/../../../modules/linuxfb
LDFLAGS += -Wl,--wrap=mp_hal_set_interrupt_char
