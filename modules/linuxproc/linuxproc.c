// SPDX-License-Identifier: MIT
// Run a Linux program on a pseudo-terminal and stream it through MicroPython.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"

// MicroPython may run at real-time priority (mpy-repl.service): the programs it starts do not.
static void linuxproc_normal_priority(void) {
    struct sched_param normal = {.sched_priority = 0};
    sched_setscheduler(0, SCHED_OTHER, &normal);
}

static void linuxproc_track(pid_t pid, bool add);

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
        linuxproc_normal_priority();
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
    linuxproc_track(child, true);

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
                    // The child has exited and its output is drained. A background process it left
                    // may still hold the terminal open: do not wait for it, or the loop spins.
                    master_done = child_done;
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
        linuxproc_track(child, false);
        nlr_raise(nlr.ret_val);
    }
    if (!child_done) {
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }
    close(master);
    linuxproc_track(child, false);
    if (WIFEXITED(status)) {
        return MP_OBJ_NEW_SMALL_INT(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return MP_OBJ_NEW_SMALL_INT(-WTERMSIG(status));
    }
    return MP_OBJ_NEW_SMALL_INT(-1);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(linuxproc_run_obj, 2, linuxproc_run);

// Process(argv, rows=24, cols=80): a Linux program running in the background on its own
// pseudo-terminal, for applications that keep their GUI live while it runs (a player, a download).
// Nothing blocks: read() returns what is there, write() what fits, poll() the exit status or None.
// The program and its process group are stopped by kill(), close(), or when the object is collected.

// The programs started by Process and still running: a soft reset re-executes MicroPython, which
// would leave them running with nobody to stop them (a player would go on playing).
#define LINUXPROC_TRACKED (16)
static pid_t linuxproc_tracked[LINUXPROC_TRACKED];

static void linuxproc_track(pid_t pid, bool add) {
    for (int i = 0; i < LINUXPROC_TRACKED; ++i) {
        if (add ? linuxproc_tracked[i] == 0 : linuxproc_tracked[i] == pid) {
            linuxproc_tracked[i] = add ? pid : 0;
            return;
        }
    }
}

// Called by mpyrepl before a soft reset.
void linuxproc_kill_all(void) {
    for (int i = 0; i < LINUXPROC_TRACKED; ++i) {
        if (linuxproc_tracked[i] > 0) {
            kill(-linuxproc_tracked[i], SIGKILL);
            kill(linuxproc_tracked[i], SIGKILL);
            // Collect it now: after the exec nobody would, and it would stay a zombie.
            while (waitpid(linuxproc_tracked[i], NULL, 0) < 0 && errno == EINTR) {
            }
            linuxproc_tracked[i] = 0;
        }
    }
}

typedef struct _linuxproc_process_obj_t {
    mp_obj_base_t base;
    pid_t pid;
    int fd;
    bool exited;
    int status;
} linuxproc_process_obj_t;

static mp_obj_t process_status(linuxproc_process_obj_t *self) {
    if (WIFEXITED(self->status)) {
        return MP_OBJ_NEW_SMALL_INT(WEXITSTATUS(self->status));
    }
    if (WIFSIGNALED(self->status)) {
        return MP_OBJ_NEW_SMALL_INT(-WTERMSIG(self->status));
    }
    return MP_OBJ_NEW_SMALL_INT(-1);
}

static bool process_reap(linuxproc_process_obj_t *self, int options) {
    if (!self->exited && self->pid > 0) {
        pid_t result;
        do {
            result = waitpid(self->pid, &self->status, options);
        } while (result < 0 && errno == EINTR);
        if (result == self->pid || (result < 0 && errno == ECHILD)) {
            self->exited = true;
            linuxproc_track(self->pid, false);
        }
    }
    return self->exited;
}

static mp_obj_t linuxproc_process_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_argv, ARG_rows, ARG_cols };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_argv, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_rows, MP_ARG_INT, {.u_int = 24} },
        { MP_QSTR_cols, MP_ARG_INT, {.u_int = 80} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

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

    linuxproc_process_obj_t *self = mp_obj_malloc_with_finaliser(linuxproc_process_obj_t, type);
    self->pid = -1;
    self->fd = -1;
    self->exited = false;
    self->status = 0;

    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
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
    struct termios termios;
    if (tcgetattr(slave, &termios) == 0) {
        termios.c_lflag &= ~ECHO; // what the application writes is input, not output to show
        tcsetattr(slave, TCSANOW, &termios);
    }
    struct winsize size = {.ws_row = args[ARG_rows].u_int, .ws_col = args[ARG_cols].u_int};
    ioctl(slave, TIOCSWINSZ, &size);

    pid_t child = fork();
    if (child < 0) {
        int error = errno;
        close(slave);
        close(master);
        mp_raise_OSError(error);
    }
    if (child == 0) {
        linuxproc_normal_priority();
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        close_range(STDERR_FILENO + 1, UINT_MAX, 0);
        execvp(argv[0], argv);
        const char *message = "linuxproc: exec failed\n";
        write(STDERR_FILENO, message, strlen(message));
        _exit(127);
    }
    close(slave);
    self->pid = child;
    self->fd = master;
    linuxproc_track(child, true);
    return MP_OBJ_FROM_PTR(self);
}

static linuxproc_process_obj_t *process_open(mp_obj_t self_in) {
    linuxproc_process_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd < 0) {
        mp_raise_OSError(MP_EBADF);
    }
    return self;
}

static mp_obj_t linuxproc_process_read(size_t n_args, const mp_obj_t *args) {
    linuxproc_process_obj_t *self = process_open(args[0]);
    mp_int_t wanted = n_args > 1 ? mp_obj_get_int(args[1]) : 4096;
    if (wanted <= 0) {
        return mp_const_empty_bytes;
    }
    vstr_t buffer;
    vstr_init_len(&buffer, wanted);
    ssize_t got = read(self->fd, buffer.buf, wanted);
    if (got < 0) {
        int error = errno;
        vstr_clear(&buffer);
        // EAGAIN: nothing yet. EIO: every program on the terminal has closed it.
        if (error == EAGAIN || error == EINTR || error == EIO) {
            return mp_const_empty_bytes;
        }
        mp_raise_OSError(error);
    }
    buffer.len = got;
    return mp_obj_new_bytes_from_vstr(&buffer);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(linuxproc_process_read_obj, 1, 2, linuxproc_process_read);

static mp_obj_t linuxproc_process_write(mp_obj_t self_in, mp_obj_t data_in) {
    linuxproc_process_obj_t *self = process_open(self_in);
    mp_buffer_info_t data;
    mp_get_buffer_raise(data_in, &data, MP_BUFFER_READ);
    ssize_t written = write(self->fd, data.buf, data.len);
    if (written < 0) {
        if (errno == EAGAIN || errno == EINTR) {
            return MP_OBJ_NEW_SMALL_INT(0);
        }
        mp_raise_OSError(errno);
    }
    return MP_OBJ_NEW_SMALL_INT(written);
}
static MP_DEFINE_CONST_FUN_OBJ_2(linuxproc_process_write_obj, linuxproc_process_write);

static mp_obj_t linuxproc_process_poll(mp_obj_t self_in) {
    linuxproc_process_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return process_reap(self, WNOHANG) ? process_status(self) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(linuxproc_process_poll_obj, linuxproc_process_poll);

static mp_obj_t linuxproc_process_kill(size_t n_args, const mp_obj_t *args) {
    linuxproc_process_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    int signal_number = n_args > 1 ? mp_obj_get_int(args[1]) : SIGTERM;
    if (!process_reap(self, WNOHANG)) {
        if (kill(-self->pid, signal_number) < 0 && errno == ESRCH) {
            kill(self->pid, signal_number);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(linuxproc_process_kill_obj, 1, 2, linuxproc_process_kill);

static mp_obj_t linuxproc_process_close(mp_obj_t self_in) {
    linuxproc_process_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->pid > 0 && !process_reap(self, WNOHANG)) {
        kill(-self->pid, SIGTERM);
        for (int tries = 0; tries < 50 && !process_reap(self, WNOHANG); ++tries) {
            usleep(20000); // up to 1 s to exit cleanly
        }
        if (!process_reap(self, WNOHANG)) {
            kill(-self->pid, SIGKILL);
            process_reap(self, 0);
        }
    }
    if (self->fd >= 0) {
        close(self->fd);
        self->fd = -1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(linuxproc_process_close_obj, linuxproc_process_close);

static mp_obj_t linuxproc_process_fileno(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(process_open(self_in)->fd);
}
static MP_DEFINE_CONST_FUN_OBJ_1(linuxproc_process_fileno_obj, linuxproc_process_fileno);

static void linuxproc_process_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    linuxproc_process_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_pid) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(self->pid);
    } else {
        dest[1] = MP_OBJ_SENTINEL; // continue in the locals dict
    }
}

static const mp_rom_map_elem_t linuxproc_process_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&linuxproc_process_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&linuxproc_process_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll), MP_ROM_PTR(&linuxproc_process_poll_obj) },
    { MP_ROM_QSTR(MP_QSTR_kill), MP_ROM_PTR(&linuxproc_process_kill_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&linuxproc_process_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_fileno), MP_ROM_PTR(&linuxproc_process_fileno_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&linuxproc_process_close_obj) },
};
static MP_DEFINE_CONST_DICT(linuxproc_process_locals_dict, linuxproc_process_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    linuxproc_process_type,
    MP_QSTR_Process,
    MP_TYPE_FLAG_NONE,
    make_new, linuxproc_process_make_new,
    attr, linuxproc_process_attr,
    locals_dict, &linuxproc_process_locals_dict
    );

static const mp_rom_map_elem_t linuxproc_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_linuxproc) },
    { MP_ROM_QSTR(MP_QSTR_run), MP_ROM_PTR(&linuxproc_run_obj) },
    { MP_ROM_QSTR(MP_QSTR_Process), MP_ROM_PTR(&linuxproc_process_type) },
};
static MP_DEFINE_CONST_DICT(linuxproc_module_globals, linuxproc_module_globals_table);

const mp_obj_module_t linuxproc_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&linuxproc_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_linuxproc, linuxproc_user_cmodule);
