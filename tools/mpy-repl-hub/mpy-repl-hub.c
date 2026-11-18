// SPDX-License-Identifier: MIT
// One MicroPython REPL process on a pseudo-terminal, shared by the USB serial port and one TCP client.
//
//   mpy-repl-hub [--tty PATH] [--listen ADDRESS:PORT] [--keyboard LAYOUT] -- PROGRAM [ARGUMENT...]
//
// The program runs with the pseudo-terminal as stdin, stdout, stderr and controlling terminal, so
// its line discipline turns Ctrl-C into SIGINT whatever the client: serial or TCP. Its output goes
// to every client, as with MicroPython's dupterm on the MCU ports; input from every client goes to
// it. A new TCP connection replaces the previous one. When the program exits it is started again.
// The keyboards plugged into the board type into it too (keyboard.c): LAYOUT is us (the default),
// it, or none; the "keyboard" section of settings.json (in the working directory, or $MPY_SETTINGS)
// chooses another, as the Settings app does.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "keyboard.h"

#define OUTPUT_SIZE 65536
#define CHUNK 4096
#define STALL_MS 1000
#define RETRY_MS 1000
#define QUICK_EXIT_MS 1000
#define RESPAWN_DELAY_MS 500

struct client {
    const char *name;
    int fd;
    unsigned char output[OUTPUT_SIZE];
    size_t length;
    long long progress_ms;
    bool discarding; // the client stopped reading: its output is dropped until it can take more
};

static struct client serial_client = {.name = "serial", .fd = -1};
static struct client tcp_client = {.name = "tcp", .fd = -1};
static struct client *const clients[] = {&serial_client, &tcp_client};
static volatile sig_atomic_t terminating;

// systemd's notification protocol, without libsystemd: one datagram to $NOTIFY_SOCKET.
static void notify(const char *message) {
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path || (path[0] != '/' && path[0] != '@') || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return;
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    size_t length = strlen(path);
    memcpy(address.sun_path, path, length);
    if (address.sun_path[0] == '@') {
        address.sun_path[0] = 0; // abstract socket
    }
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        sendto(fd, message, strlen(message), MSG_NOSIGNAL, (struct sockaddr *)&address,
            offsetof(struct sockaddr_un, sun_path) + length);
        close(fd);
    }
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_terminate(int signal_number) {
    (void)signal_number;
    terminating = 1;
}

static void set_nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static void client_close(struct client *client, const char *why) {
    if (client->fd >= 0) {
        fprintf(stderr, "mpy-repl-hub: %s closed: %s\n", client->name, why);
        close(client->fd);
    }
    client->fd = -1;
    client->length = 0;
    client->discarding = false;
}

static void client_attach(struct client *client, int fd) {
    client_close(client, "replaced");
    set_nonblocking(fd);
    client->fd = fd;
    client->length = 0;
    client->discarding = false;
    client->progress_ms = now_ms();
    fprintf(stderr, "mpy-repl-hub: %s attached\n", client->name);
}

// The pseudo-terminal as MicroPython's REPL expects it: raw bytes both ways, Ctrl-C as SIGINT while
// code runs (mpyrepl switches ISIG with the interrupt character), no Ctrl-Z or Ctrl-\ signals.
static void configure_repl_terminal(int fd) {
    struct termios termios;
    if (tcgetattr(fd, &termios) == 0) {
        cfmakeraw(&termios);
        termios.c_lflag |= ISIG;
        termios.c_cc[VINTR] = 3;
        termios.c_cc[VQUIT] = _POSIX_VDISABLE;
        termios.c_cc[VSUSP] = _POSIX_VDISABLE;
        termios.c_cc[VMIN] = 1;
        termios.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &termios);
    }
    struct winsize size = {.ws_row = 24, .ws_col = 80};
    ioctl(fd, TIOCSWINSZ, &size);
}

static void serial_open(const char *path) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    struct termios termios;
    if (tcgetattr(fd, &termios) == 0) {
        cfmakeraw(&termios);
        tcsetattr(fd, TCSANOW, &termios);
    }
    client_attach(&serial_client, fd);
}

static int listen_open(const char *address) {
    char host[64];
    const char *colon = strrchr(address, ':');
    if (!colon || (size_t)(colon - address) >= sizeof(host)) {
        fprintf(stderr, "mpy-repl-hub: --listen wants ADDRESS:PORT, not %s\n", address);
        exit(2);
    }
    memcpy(host, address, colon - address);
    host[colon - address] = 0;
    struct sockaddr_in socket_address = {.sin_family = AF_INET, .sin_port = htons(atoi(colon + 1))};
    if (inet_pton(AF_INET, host, &socket_address.sin_addr) != 1) {
        fprintf(stderr, "mpy-repl-hub: invalid IPv4 address %s\n", host);
        exit(2);
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // The USB network address may not be assigned yet during boot.
    setsockopt(fd, IPPROTO_IP, IP_FREEBIND, &one, sizeof(one));
    if (fd < 0 || bind(fd, (struct sockaddr *)&socket_address, sizeof(socket_address)) < 0 || listen(fd, 2) < 0) {
        perror("mpy-repl-hub: listen");
        exit(1);
    }
    return fd;
}

static pid_t spawn(int slave, char **argv) {
    configure_repl_terminal(slave);
    pid_t pid = fork();
    if (pid < 0) {
        perror("mpy-repl-hub: fork");
        return -1;
    }
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        close_range(STDERR_FILENO + 1, ~0U, 0);
        execvp(argv[0], argv);
        perror("mpy-repl-hub: exec");
        _exit(127);
    }
    fprintf(stderr, "mpy-repl-hub: started %s, pid %d\n", argv[0], (int)pid);
    return pid;
}

static void client_flush(struct client *client) {
    if (client->fd < 0 || client->length == 0) {
        return;
    }
    ssize_t written = write(client->fd, client->output, client->length);
    if (written > 0) {
        memmove(client->output, client->output + written, client->length - written);
        client->length -= written;
        client->progress_ms = now_ms();
    } else if (written < 0 && errno != EAGAIN && errno != EINTR) {
        client_close(client, strerror(errno));
    }
}

static void usage(void) {
    fprintf(stderr, "usage: mpy-repl-hub [--tty PATH] [--listen ADDRESS:PORT] [--keyboard us|it|none] -- PROGRAM [ARGUMENT...]\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *tty_path = NULL;
    const char *listen_address = NULL;
    int index = 1;
    for (; index < argc; ++index) {
        if (!strcmp(argv[index], "--tty") && index + 1 < argc) {
            tty_path = argv[++index];
        } else if (!strcmp(argv[index], "--listen") && index + 1 < argc) {
            listen_address = argv[++index];
        } else if (!strcmp(argv[index], "--keyboard") && index + 1 < argc) {
            if (!keyboard_layout(argv[++index])) {
                fprintf(stderr, "mpy-repl-hub: unknown keyboard layout %s\n", argv[index]);
                usage();
            }
        } else if (!strcmp(argv[index], "--")) {
            ++index;
            break;
        } else {
            usage();
        }
    }
    if (index >= argc) {
        usage();
    }
    char **program = argv + index;
    keyboard_settings(getenv("MPY_SETTINGS") ? getenv("MPY_SETTINGS") : "settings.json");

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_terminate);
    signal(SIGINT, on_terminate);

    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || grantpt(master) || unlockpt(master)) {
        perror("mpy-repl-hub: pseudo-terminal");
        return 1;
    }
    // The hub keeps the slave open, so the master never reports EIO while the program restarts.
    int slave = open(ptsname(master), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        perror("mpy-repl-hub: pseudo-terminal slave");
        return 1;
    }
    set_nonblocking(master);
    int listener = listen_address ? listen_open(listen_address) : -1;

    unsigned char input[CHUNK];
    size_t input_offset = 0, input_length = 0;
    long long serial_retry_ms = 0, keyboard_scan_ms = 0, started_ms = now_ms(), respawn_ms = 0;
    pid_t child = spawn(slave, program);
    notify("READY=1");
    // WatchdogSec= in the unit: the hub's loop pings at a third of the period. It shows that the
    // hub serves its clients; a MicroPython program that hangs is a matter for machine.WDT.
    const char *watchdog = getenv("WATCHDOG_USEC");
    long long watchdog_ms = watchdog ? atoll(watchdog) / 3000 : 0, pinged_ms = 0;

    while (!terminating) {
        long long now = now_ms();
        if (watchdog_ms > 0 && now - pinged_ms >= watchdog_ms) {
            notify("WATCHDOG=1");
            pinged_ms = now;
        }
        if (child > 0) {
            int status;
            if (waitpid(child, &status, WNOHANG) == child) {
                fprintf(stderr, "mpy-repl-hub: pid %d ended, status 0x%x\n", (int)child, status);
                child = -1;
                // A program that fails at once is not restarted in a tight loop.
                respawn_ms = now - started_ms < QUICK_EXIT_MS ? now + RESPAWN_DELAY_MS : now;
            }
        }
        if (child < 0 && now >= respawn_ms) {
            started_ms = now;
            child = spawn(slave, program);
            if (child < 0) {
                respawn_ms = now + RETRY_MS;
            }
        }
        if (tty_path && serial_client.fd < 0 && now >= serial_retry_ms) {
            serial_open(tty_path);
            serial_retry_ms = now + RETRY_MS;
        }
        if (now >= keyboard_scan_ms) {
            keyboard_scan();
            keyboard_scan_ms = now + RETRY_MS;
        }
        for (size_t i = 0; i < sizeof(clients) / sizeof(clients[0]); ++i) {
            struct client *client = clients[i];
            if (client->fd >= 0 && client->length && !client->discarding && now - client->progress_ms > STALL_MS) {
                fprintf(stderr, "mpy-repl-hub: %s is not reading, its output is dropped\n", client->name);
                client->discarding = true;
                client->length = 0;
            }
        }

        // Read the program's output only when every client that listens has room for it.
        bool room = true;
        for (size_t i = 0; i < sizeof(clients) / sizeof(clients[0]); ++i) {
            struct client *client = clients[i];
            if (client->fd >= 0 && !client->discarding && client->length > OUTPUT_SIZE - CHUNK) {
                room = false;
            }
        }
        struct pollfd fds[4 + KEYBOARD_MAX];
        nfds_t count = 0;
        fds[count++] = (struct pollfd){master, (room ? POLLIN : 0) | (input_length ? POLLOUT : 0), 0};
        int client_slot[2] = {-1, -1};
        for (size_t i = 0; i < 2; ++i) {
            struct client *client = clients[i];
            if (client->fd >= 0) {
                short events = (input_length ? 0 : POLLIN) | (client->length || client->discarding ? POLLOUT : 0);
                client_slot[i] = count;
                fds[count++] = (struct pollfd){client->fd, events, 0};
            }
        }
        int listener_slot = -1;
        if (listener >= 0) {
            listener_slot = count;
            fds[count++] = (struct pollfd){listener, POLLIN, 0};
        }
        // The keyboards wait, as the clients do, while earlier input is still on its way.
        nfds_t keyboard_slot = count;
        count += keyboard_poll_fds(fds + count, input_length ? 0 : POLLIN);
        if (poll(fds, count, 100) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("mpy-repl-hub: poll");
            break;
        }

        if (fds[0].revents & POLLIN) {
            unsigned char chunk[CHUNK];
            ssize_t got = read(master, chunk, sizeof(chunk));
            for (size_t i = 0; got > 0 && i < 2; ++i) {
                struct client *client = clients[i];
                if (client->fd >= 0 && !client->discarding) {
                    if (client->length == 0) {
                        client->progress_ms = now_ms();
                    }
                    memcpy(client->output + client->length, chunk, got);
                    client->length += got;
                }
            }
        }
        if (input_length && (fds[0].revents & POLLOUT)) {
            ssize_t written = write(master, input + input_offset, input_length - input_offset);
            if (written > 0) {
                input_offset += written;
                if (input_offset == input_length) {
                    input_offset = input_length = 0;
                }
            }
        }
        for (size_t i = 0; i < 2; ++i) {
            struct client *client = clients[i];
            if (client_slot[i] < 0 || client->fd < 0) {
                continue;
            }
            short revents = fds[client_slot[i]].revents;
            if (revents & POLLOUT) {
                if (client->discarding) {
                    fprintf(stderr, "mpy-repl-hub: %s reads again\n", client->name);
                    client->discarding = false;
                    client->progress_ms = now_ms();
                }
                client_flush(client);
            }
            if (client->fd >= 0 && !input_length && (revents & (POLLIN | POLLHUP | POLLERR))) {
                ssize_t got = read(client->fd, input, sizeof(input));
                if (got > 0) {
                    input_offset = 0;
                    input_length = got;
                } else if (got == 0) {
                    client_close(client, "end of input");
                } else if (errno != EAGAIN && errno != EINTR) {
                    client_close(client, strerror(errno));
                }
            }
        }
        if (!input_length) {
            input_length = keyboard_read(fds + keyboard_slot, count - keyboard_slot, input, 0, sizeof(input));
            input_offset = 0;
        }
        if (listener_slot >= 0 && (fds[listener_slot].revents & POLLIN)) {
            int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
            if (fd >= 0) {
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
                client_attach(&tcp_client, fd);
            }
        }
    }

    if (child > 0) {
        kill(-child, SIGTERM);
        waitpid(child, NULL, 0);
    }
    return 0;
}
