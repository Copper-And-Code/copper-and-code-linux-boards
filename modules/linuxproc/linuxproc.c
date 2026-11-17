// SPDX-License-Identifier: MIT
// Run a Linux program on a pseudo-terminal and stream it through MicroPython.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "py/mphal.h"
#include "py/runtime.h"

static mp_obj_t linuxproc_run(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_argv, ARG_output, ARG_rows, ARG_cols, ARG_input_fd };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_argv, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_output, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_rows, MP_ARG_INT, {.u_int = 24} },
        { MP_QSTR_cols, MP_ARG_INT, {.u_int = 80} },
        { MP_QSTR_input_fd, MP_ARG_INT, {.u_int = STDIN_FILENO} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    size_t count;
    mp_obj_t *items;
    mp_obj_get_array(args[ARG_argv].u_obj, &count, &items);
    if (!count) {
        mp_raise_ValueError(MP_ERROR_TEXT("linuxproc: empty argv"));
    }
    char **argv = m_new(char *, count + 1);
    for (size_t i = 0; i < count; ++i) {
        argv[i] = (char *)mp_obj_str_get_str(items[i]);
    }
    argv[count] = NULL;

    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (master < 0 || grantpt(master) || unlockpt(master)) {
        int error = errno;
        if (master >= 0) {
            close(master);
        }
        mp_raise_OSError(error);
    }
    int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    if (slave < 0) {
        int error = errno;
        close(master);
        mp_raise_OSError(error);
    }
    struct winsize size = {
        .ws_row = args[ARG_rows].u_int,
        .ws_col = args[ARG_cols].u_int,
    };
    ioctl(slave, TIOCSWINSZ, &size);

    pid_t child = fork();
    if (child < 0) {
        int error = errno;
        close(slave);
        close(master);
        mp_raise_OSError(error);
    }
    if (child == 0) {
        close(master);
        if (setsid() < 0) {
            const char *message = "linuxproc: setsid failed\n";
            write(STDERR_FILENO, message, strlen(message));
            _exit(126);
        }
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        close(slave);
        close_range(STDERR_FILENO + 1, UINT_MAX, 0);
        execvp(argv[0], argv);
        const char *message = "linuxproc: exec failed\n";
        write(STDERR_FILENO, message, strlen(message));
        _exit(127);
    }
    close(slave);

    int status = 0;
    bool child_done = false;
    int input_fd = args[ARG_input_fd].u_int;
    uint8_t input[256];
    size_t input_offset = 0;
    size_t input_size = 0;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        for (;;) {
            if (!child_done) {
                pid_t result = waitpid(child, &status, WNOHANG);
                child_done = result == child;
                if (result < 0 && errno != EINTR) {
                    mp_raise_OSError(errno);
                }
            }
            bool input_pending = input_offset < input_size;
            struct pollfd fds[2] = {
                {master, POLLIN | (input_pending ? POLLOUT : 0), 0},
                {input_fd, POLLIN, 0},
            };
            nfds_t nfds = input_fd >= 0 && !input_pending ? 2 : 1;
            MP_THREAD_GIL_EXIT();
            int ready = poll(fds, nfds, child_done ? 0 : 20);
            MP_THREAD_GIL_ENTER();
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                mp_raise_OSError(errno);
            }
            if (input_pending && (fds[0].revents & POLLOUT)) {
                ssize_t written = write(master, input + input_offset, input_size - input_offset);
                if (written > 0) {
                    input_offset += written;
                    if (input_offset == input_size) {
                        input_offset = input_size = 0;
                    }
                } else if (written < 0 && errno == EIO) {
                    input_offset = input_size = 0;
                    input_fd = -1;
                } else if (written < 0 && errno != EINTR && errno != EAGAIN) {
                    mp_raise_OSError(errno);
                }
            }
            if (nfds == 2 && (fds[1].revents & (POLLIN | POLLHUP))) {
                ssize_t got = read(input_fd, input, sizeof(input));
                if (got > 0) {
                    input_offset = 0;
                    input_size = got;
                } else if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) {
                    input_fd = -1;
                }
            }
            bool master_done = false;
            for (;;) {
                uint8_t output[512];
                ssize_t got = read(master, output, sizeof(output));
                if (got > 0) {
                    mp_obj_t data = mp_obj_new_bytes(output, got);
                    mp_call_function_1(args[ARG_output].u_obj, data);
                } else if (got == 0 || (got < 0 && errno == EIO)) {
                    master_done = true;
                    break;
                } else if (got < 0 && errno == EAGAIN) {
                    break;
                } else if (got < 0 && errno != EINTR) {
                    mp_raise_OSError(errno);
                }
            }
            if (master_done) {
                break;
            }
            mp_handle_pending(true);
        }
        nlr_pop();
    } else {
        if (kill(-child, SIGKILL) < 0 && errno == ESRCH) {
            kill(child, SIGKILL);
        }
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        close(master);
        nlr_raise(nlr.ret_val);
    }
    if (!child_done) {
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }
    close(master);
    if (WIFEXITED(status)) {
        return MP_OBJ_NEW_SMALL_INT(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return MP_OBJ_NEW_SMALL_INT(-WTERMSIG(status));
    }
    return MP_OBJ_NEW_SMALL_INT(-1);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(linuxproc_run_obj, 2, linuxproc_run);

static const mp_rom_map_elem_t linuxproc_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_linuxproc) },
    { MP_ROM_QSTR(MP_QSTR_run), MP_ROM_PTR(&linuxproc_run_obj) },
};
static MP_DEFINE_CONST_DICT(linuxproc_module_globals, linuxproc_module_globals_table);

const mp_obj_module_t linuxproc_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&linuxproc_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_linuxproc, linuxproc_user_cmodule);
