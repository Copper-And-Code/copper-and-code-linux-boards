/* SPDX-License-Identifier: MIT */

#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)
#define MICROPY_PY_OS_DUPTERM (1)

// machine.Pin on the Linux GPIO character device (modules/linuxmachine). The tag of the struct is
// enough here: the address only goes into machine's globals table.
#ifndef __ASSEMBLER__
extern const struct _mp_obj_type_t machine_pin_type;
#endif
#define MICROPY_PY_MACHINE_EXTRA_GLOBALS { MP_ROM_QSTR(MP_QSTR_Pin), MP_ROM_PTR(&machine_pin_type) },

#include "variants/mpconfigvariant_common.h"
