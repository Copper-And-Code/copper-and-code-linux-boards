// SPDX-License-Identifier: MIT
// The HAL modules the firmware uses beyond those of ST's OpenAMP_TTY_echo, whose configuration
// file (found next on the include path) does the rest.
#ifndef MPY_M4_HAL_CONF_H
#define MPY_M4_HAL_CONF_H

#define HAL_ADC_MODULE_ENABLED
#define HAL_I2C_MODULE_ENABLED

#include_next "stm32mp1xx_hal_conf.h"

#endif
