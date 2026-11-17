// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "shared/runtime/pyexec.h"

#ifndef MPYREPL_USER_BUTTON_DEVICE
#define MPYREPL_USER_BUTTON_DEVICE "/dev/input/by-path/platform-gpio-keys-event"
#endif
#ifndef MPYREPL_USER_BUTTON_CODE
#define MPYREPL_USER_BUTTON_CODE BTN_1
#endif

#define MPYREPL_MODE_FILE "/run/mpy-repl-mode"
#define MPYREPL_MODE_TEMP "/run/mpy-repl-mode.tmp"
#define MPYREPL_LAUNCHER "/usr/local/mpy/lib/mpy_launcher.py"

extern char **environ;

static void load_mode(void) {
    int fd = open(MPYREPL_MODE_FILE, O_RDONLY);
    if (fd >= 0) {
        char mode[3];
        if (read(fd, mode, sizeof(mode)) == sizeof(mode) && memcmp(mode, "raw", sizeof(mode)) == 0) {
            pyexec_mode_kind = PYEXEC_MODE_RAW_REPL;
        }
        close(fd);
    }
}

static void save_mode(void) {
    const char *mode = pyexec_mode_kind == PYEXEC_MODE_RAW_REPL ? "raw" : "friendly";
    size_t size = strlen(mode);
    int fd = open(MPYREPL_MODE_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, mode, size) == (ssize_t)size) {
            rename(MPYREPL_MODE_TEMP, MPYREPL_MODE_FILE);
        }
        close(fd);
    }
}

static bool socket_peer_closed(void) {
    char c;
    return recv(STDIN_FILENO, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

// linuxproc, when it is built in: stops the Linux programs the application started.
extern void linuxproc_kill_all(void) __attribute__((weak));

static void exec_self(void) {
    if (!isatty(STDIN_FILENO) && socket_peer_closed()) {
        return;
    }
    if (linuxproc_kill_all) {
        linuxproc_kill_all();
    }
    char *const argv[] = { "micropython", MPYREPL_LAUNCHER, NULL };
    close_range(STDERR_FILENO + 1, UINT_MAX, 0);
    execve("/proc/self/exe", argv, environ);
}

static void set_keyboard_interrupt(bool enabled) {
    struct termios termios;
    if (tcgetattr(STDIN_FILENO, &termios) == 0) {
        if (enabled) {
            // Only Ctrl-C becomes a signal: Ctrl-Z would stop the REPL process, Ctrl-\ kill it.
            termios.c_lflag |= ISIG;
            termios.c_cc[VSUSP] = _POSIX_VDISABLE;
            termios.c_cc[VQUIT] = _POSIX_VDISABLE;
        } else {
            termios.c_lflag &= ~ISIG;
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &termios);
    }
}

extern void __real_mp_hal_set_interrupt_char(char c);

void __wrap_mp_hal_set_interrupt_char(char c) {
    __real_mp_hal_set_interrupt_char(c);
    set_keyboard_interrupt(c == CHAR_CTRL_C);
}

static mp_obj_t mpyrepl_run(mp_obj_t safe_mode) {
    load_mode();

    int ret = pyexec_file_if_exists("boot.py");
    // As on the MCU ports, main.py runs only for the friendly REPL: after a soft reset asked from
    // the raw REPL (mpremote, LVGL MPY Studio) the tool gets the REPL, not the application.
    if (!(ret & PYEXEC_FORCED_EXIT) && !mp_obj_is_true(safe_mode) && pyexec_mode_kind == PYEXEC_MODE_FRIENDLY_REPL) {
        ret = pyexec_file_if_exists("main.py");
    }
    if (!(ret & PYEXEC_FORCED_EXIT)) {
        for (;;) {
            if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
                ret = pyexec_raw_repl();
            } else {
                ret = pyexec_friendly_repl();
            }
            if (ret != 0) {
                if (ret & PYEXEC_FORCED_EXIT) {
                    mp_hal_stdout_tx_str("MPY: soft reboot\r\n");
                }
                break;
            }
        }
    }

    save_mode();
    if (ret & PYEXEC_FORCED_EXIT) {
        exec_self();
    }
    return MP_OBJ_NEW_SMALL_INT(ret);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mpyrepl_run_obj, mpyrepl_run);

// restart(): a soft reset asked by the application, for example to start another app in a clean
// process. It works from any thread, LVGL callbacks included. The REPL mode is kept.
static mp_obj_t mpyrepl_restart(void) {
    save_mode();
    exec_self();
    mp_raise_OSError(errno);  // only if exec failed
}
static MP_DEFINE_CONST_FUN_OBJ_0(mpyrepl_restart_obj, mpyrepl_restart);

// For an interactive main.py such as a shell: when a tool asks for the raw REPL (Ctrl-A), the
// program calls this and returns, and the REPL starts in raw mode, as the tool expects.
static mp_obj_t mpyrepl_raw_repl(void) {
    pyexec_mode_kind = PYEXEC_MODE_RAW_REPL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mpyrepl_raw_repl_obj, mpyrepl_raw_repl);

static mp_obj_t mpyrepl_user_button_pressed(void) {
    uint8_t state[(KEY_MAX + 7) / 8] = {0};
    int fd = open(MPYREPL_USER_BUTTON_DEVICE, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return mp_const_false;
    }
    int ret = ioctl(fd, EVIOCGKEY(sizeof(state)), state);
    close(fd);
    if (ret < 0) {
        return mp_const_false;
    }
    unsigned int code = MPYREPL_USER_BUTTON_CODE;
    return mp_obj_new_bool((state[code / 8] & (1u << (code % 8))) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mpyrepl_user_button_pressed_obj, mpyrepl_user_button_pressed);

static const mp_rom_map_elem_t mpyrepl_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mpyrepl) },
    { MP_ROM_QSTR(MP_QSTR_run), MP_ROM_PTR(&mpyrepl_run_obj) },
    { MP_ROM_QSTR(MP_QSTR_raw_repl), MP_ROM_PTR(&mpyrepl_raw_repl_obj) },
    { MP_ROM_QSTR(MP_QSTR_restart), MP_ROM_PTR(&mpyrepl_restart_obj) },
    { MP_ROM_QSTR(MP_QSTR_user_button_pressed), MP_ROM_PTR(&mpyrepl_user_button_pressed_obj) },
};
static MP_DEFINE_CONST_DICT(mpyrepl_module_globals, mpyrepl_module_globals_table);

const mp_obj_module_t mpyrepl_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mpyrepl_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_mpyrepl, mpyrepl_user_cmodule);
