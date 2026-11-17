// SPDX-License-Identifier: MIT
// machine.Pin on the Linux GPIO character device (uAPI v2).
//
//   Pin(id, mode=-1, pull=-1, *, value=None)
//
// id is a line name as the kernel reports it ("PA4"), or (chip, offset) with chip a number, a
// device name ("gpiochip9") or a chip label ("mcp23017"). The line is requested when the Pin is made and released by deinit(), by the
// garbage collector or when the process ends. A line already used by the kernel or another process
// raises OSError(EBUSY). Without a mode the line is requested as an input.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/gpio.h>

#include "py/mperrno.h"
#include "py/runtime.h"

#define PIN_IN (0)
#define PIN_OUT (1)
#define PIN_OPEN_DRAIN (2)
#define PIN_PULL_NONE (0)
#define PIN_PULL_UP (1)
#define PIN_PULL_DOWN (2)
#define MAX_CHIPS (64)

typedef struct _machine_pin_obj_t {
    mp_obj_base_t base;
    int line_fd;
    int chip;
    unsigned int offset;
    char name[GPIO_MAX_NAME_SIZE];
    int mode;
    int pull;
    int value;
} machine_pin_obj_t;

static int chip_open(int chip) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/gpiochip%d", chip);
    return open(path, O_RDWR | O_CLOEXEC);
}

static void pin_find(machine_pin_obj_t *self, mp_obj_t id) {
    if (mp_obj_is_str(id)) {
        const char *wanted = mp_obj_str_get_str(id);
        for (int chip = 0; chip < MAX_CHIPS; ++chip) {
            int fd = chip_open(chip);
            if (fd < 0) {
                if (errno == ENOENT) {
                    continue;
                }
                mp_raise_OSError(errno);
            }
            struct gpiochip_info info;
            if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0) {
                for (unsigned int offset = 0; offset < info.lines; ++offset) {
                    struct gpio_v2_line_info line = {.offset = offset};
                    if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &line) == 0 && strcmp(line.name, wanted) == 0) {
                        close(fd);
                        self->chip = chip;
                        self->offset = offset;
                        strncpy(self->name, wanted, sizeof(self->name) - 1);
                        return;
                    }
                }
            }
            close(fd);
        }
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("unknown pin %s"), wanted);
    }
    size_t count;
    mp_obj_t *items;
    mp_obj_get_array(id, &count, &items);
    if (count != 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("pin id must be a name or (chip, offset)"));
    }
    if (mp_obj_is_str(items[0])) {
        const char *chip = mp_obj_str_get_str(items[0]);
        if (strncmp(chip, "gpiochip", 8) == 0) {
            self->chip = atoi(chip + 8);
        } else {
            // A chip label, such as "GPIOA" or "mcp23017": it does not depend on probe order.
            self->chip = -1;
            for (int number = 0; number < MAX_CHIPS && self->chip < 0; ++number) {
                int fd = chip_open(number);
                if (fd < 0) {
                    continue;
                }
                struct gpiochip_info info;
                if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 && strcmp(info.label, chip) == 0) {
                    self->chip = number;
                }
                close(fd);
            }
            if (self->chip < 0) {
                mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("unknown GPIO chip %s"), chip);
            }
        }
    } else {
        self->chip = mp_obj_get_int(items[0]);
    }
    mp_int_t offset = mp_obj_get_int(items[1]);
    if (self->chip < 0 || self->chip >= MAX_CHIPS || offset < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid chip or offset"));
    }
    self->offset = offset;
    snprintf(self->name, sizeof(self->name), "gpiochip%d:%u", self->chip, self->offset);
}

static void pin_config(machine_pin_obj_t *self, struct gpio_v2_line_config *config) {
    memset(config, 0, sizeof(*config));
    if (self->mode == PIN_IN) {
        config->flags = GPIO_V2_LINE_FLAG_INPUT;
    } else {
        config->flags = GPIO_V2_LINE_FLAG_OUTPUT | (self->mode == PIN_OPEN_DRAIN ? GPIO_V2_LINE_FLAG_OPEN_DRAIN : 0);
        // The output starts at the requested level, with no glitch through the other one.
        config->num_attrs = 1;
        config->attrs[0].mask = 1;
        config->attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        config->attrs[0].attr.values = self->value ? 1 : 0;
    }
    if (self->pull == PIN_PULL_UP) {
        config->flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    } else if (self->pull == PIN_PULL_DOWN) {
        config->flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    } else {
        config->flags |= GPIO_V2_LINE_FLAG_BIAS_DISABLED;
    }
}

static void pin_apply(machine_pin_obj_t *self) {
    struct gpio_v2_line_config config;
    pin_config(self, &config);
    if (self->line_fd >= 0) {
        if (ioctl(self->line_fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &config) < 0) {
            mp_raise_OSError(errno);
        }
        return;
    }
    int chip_fd = chip_open(self->chip);
    if (chip_fd < 0) {
        mp_raise_OSError(errno);
    }
    struct gpio_v2_line_request request;
    memset(&request, 0, sizeof(request));
    request.offsets[0] = self->offset;
    request.num_lines = 1;
    strncpy(request.consumer, "micropython", sizeof(request.consumer) - 1);
    request.config = config;
    int result = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request);
    int error = errno;
    close(chip_fd);
    if (result < 0) {
        mp_raise_OSError(error);
    }
    self->line_fd = request.fd;
}

static machine_pin_obj_t *pin_open(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->line_fd < 0) {
        mp_raise_OSError(MP_EBADF);
    }
    return self;
}

static int pin_read(machine_pin_obj_t *self) {
    struct gpio_v2_line_values values = {.mask = 1};
    if (ioctl(self->line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0) {
        mp_raise_OSError(errno);
    }
    return values.bits & 1;
}

static void pin_write(machine_pin_obj_t *self, int value) {
    self->value = value ? 1 : 0;
    if (self->mode == PIN_IN) {
        return; // kept, and driven when the pin becomes an output
    }
    struct gpio_v2_line_values values = {.mask = 1, .bits = self->value};
    if (ioctl(self->line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0) {
        mp_raise_OSError(errno);
    }
}

static void pin_init_helper(machine_pin_obj_t *self, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_mode, ARG_pull, ARG_value };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_pull, MP_ARG_OBJ, {.u_obj = MP_OBJ_NEW_SMALL_INT(-1)} },
        { MP_QSTR_value, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t mode = args[ARG_mode].u_int;
    if (mode != -1) {
        if (mode != PIN_IN && mode != PIN_OUT && mode != PIN_OPEN_DRAIN) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid pin mode"));
        }
        self->mode = mode;
    }
    mp_int_t pull = args[ARG_pull].u_obj == mp_const_none ? PIN_PULL_NONE : mp_obj_get_int(args[ARG_pull].u_obj);
    if (pull != -1) {
        if (pull != PIN_PULL_NONE && pull != PIN_PULL_UP && pull != PIN_PULL_DOWN) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid pin pull"));
        }
        self->pull = pull;
    }
    if (args[ARG_value].u_obj != mp_const_none) {
        self->value = mp_obj_is_true(args[ARG_value].u_obj);
    }
    pin_apply(self);
}

static mp_obj_t machine_pin_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);
    machine_pin_obj_t *self = mp_obj_malloc_with_finaliser(machine_pin_obj_t, type);
    self->line_fd = -1;
    self->mode = PIN_IN;
    self->pull = PIN_PULL_NONE;
    self->value = 0;
    self->name[0] = 0;
    pin_find(self, args[0]);
    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
    pin_init_helper(self, n_args - 1, args + 1, &kw_args);
    return MP_OBJ_FROM_PTR(self);
}

static void machine_pin_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    static const char *const modes[] = {"IN", "OUT", "OPEN_DRAIN"};
    static const char *const pulls[] = {"None", "PULL_UP", "PULL_DOWN"};
    if (self->line_fd < 0) {
        mp_printf(print, "Pin('%s', closed)", self->name);
    } else {
        mp_printf(print, "Pin('%s', mode=%s, pull=%s)", self->name, modes[self->mode], pulls[self->pull]);
    }
}

static mp_obj_t machine_pin_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    machine_pin_obj_t *self = pin_open(self_in);
    if (n_args == 0) {
        return MP_OBJ_NEW_SMALL_INT(pin_read(self));
    }
    pin_write(self, mp_obj_is_true(args[0]));
    return mp_const_none;
}

static mp_obj_t machine_pin_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    pin_init_helper(pin_open(args[0]), n_args - 1, args + 1, kw_args);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_pin_init_obj, 1, machine_pin_init);

static mp_obj_t machine_pin_value(size_t n_args, const mp_obj_t *args) {
    return machine_pin_call(args[0], n_args - 1, 0, args + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_pin_value_obj, 1, 2, machine_pin_value);

static mp_obj_t machine_pin_on(mp_obj_t self_in) {
    pin_write(pin_open(self_in), 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_on_obj, machine_pin_on);

static mp_obj_t machine_pin_off(mp_obj_t self_in) {
    pin_write(pin_open(self_in), 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_off_obj, machine_pin_off);

static mp_obj_t machine_pin_toggle(mp_obj_t self_in) {
    machine_pin_obj_t *self = pin_open(self_in);
    pin_write(self, !self->value);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_toggle_obj, machine_pin_toggle);

static mp_obj_t machine_pin_deinit(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->line_fd >= 0) {
        close(self->line_fd);
        self->line_fd = -1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_deinit_obj, machine_pin_deinit);

static const mp_rom_map_elem_t machine_pin_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&machine_pin_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&machine_pin_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&machine_pin_on_obj) },
    { MP_ROM_QSTR(MP_QSTR_off), MP_ROM_PTR(&machine_pin_off_obj) },
    { MP_ROM_QSTR(MP_QSTR_toggle), MP_ROM_PTR(&machine_pin_toggle_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&machine_pin_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&machine_pin_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_IN), MP_ROM_INT(PIN_IN) },
    { MP_ROM_QSTR(MP_QSTR_OUT), MP_ROM_INT(PIN_OUT) },
    { MP_ROM_QSTR(MP_QSTR_OPEN_DRAIN), MP_ROM_INT(PIN_OPEN_DRAIN) },
    { MP_ROM_QSTR(MP_QSTR_PULL_UP), MP_ROM_INT(PIN_PULL_UP) },
    { MP_ROM_QSTR(MP_QSTR_PULL_DOWN), MP_ROM_INT(PIN_PULL_DOWN) },
};
static MP_DEFINE_CONST_DICT(machine_pin_locals_dict, machine_pin_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_pin_type,
    MP_QSTR_Pin,
    MP_TYPE_FLAG_NONE,
    make_new, machine_pin_make_new,
    print, machine_pin_print,
    call, machine_pin_call,
    locals_dict, &machine_pin_locals_dict
    );
