/* SPDX-License-Identifier: MIT */

#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)
#define MICROPY_PY_OS_DUPTERM (1)

// machine.Pin on the Linux GPIO character device (modules/linuxmachine). The tag of the struct is
// enough here: the address only goes into machine's globals table.
#ifndef __ASSEMBLER__
extern const struct _mp_obj_type_t machine_pin_type;
#endif
#define MICROPY_PY_MACHINE_EXTRA_GLOBALS { MP_ROM_QSTR(MP_QSTR_Pin), MP_ROM_PTR(&machine_pin_type) },

// time.sleep_ms() releases the GIL once per 10 ms instead of every 0.5 ms (modules/linuxmachine/
// linux_delay.c): threads on a single core no longer wait their turn twenty times per sleep.
#ifndef __ASSEMBLER__
#include <stdint.h>
void mpy_linux_delay_ms(uintptr_t ms);
#endif
#define mp_hal_delay_ms mpy_linux_delay_ms

#include "variants/mpconfigvariant_common.h"
