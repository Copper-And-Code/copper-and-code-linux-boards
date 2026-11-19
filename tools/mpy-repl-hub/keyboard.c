// SPDX-License-Identifier: MIT
// Keyboards plugged into the board type into the REPL, as the serial port and TCP do.
//
// Every /dev/input/event* device with letter keys is a keyboard; one plugged in later is found
// within a second. The hub grabs it (EVIOCGRAB): the kernel's console would otherwise get the
// keys too, and a login prompt that nobody sees, on tty1, would take them as a user name.
//
// Keys become what a terminal sends: UTF-8 text from the layout, Ctrl-letter as control
// characters (Ctrl-C reaches the line discipline, so it interrupts as from the serial port),
// Alt as an ESC prefix, VT100 sequences for the cursor, editing and function keys. Layouts: us
// and it (with AltGr for [ ] { } @ # ` ~ and the euro). Auto-repeat is the kernel's.
//
// The Windows key alone, or the Menu key, is the board's home key: the hub counts it in
// /run/mpy-home-key, and mpy_home opens the app menu (or leaves the app) as a tap would.
//
// The layout is the one of settings.json ({"keyboard": {"layout": "it"}}, written by the Settings
// app through mpy_settings), read again whenever the file changes; without it, --keyboard's.
#define _GNU_SOURCE
#include "keyboard.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define BITS_PER_LONG (8 * sizeof(long))
#define TEST_BIT(bit, array) (((array)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1)
#define EVENT_DEVICES 64
#define HOME_KEY_FILE "/run/mpy-home-key"

struct keyboard {
    int fd;
    int number; // N of /dev/input/eventN
};

// The keys whose text depends on the layout: no shift, shift, AltGr, AltGr with shift.
struct layout {
    const char *name;
    const char *keys[KEY_102ND + 1][4];
};

static const struct layout us = {"us", {
    [KEY_1] = {"1", "!"}, [KEY_2] = {"2", "@"}, [KEY_3] = {"3", "#"}, [KEY_4] = {"4", "$"},
    [KEY_5] = {"5", "%"}, [KEY_6] = {"6", "^"}, [KEY_7] = {"7", "&"}, [KEY_8] = {"8", "*"},
    [KEY_9] = {"9", "("}, [KEY_0] = {"0", ")"}, [KEY_MINUS] = {"-", "_"}, [KEY_EQUAL] = {"=", "+"},
    [KEY_LEFTBRACE] = {"[", "{"}, [KEY_RIGHTBRACE] = {"]", "}"}, [KEY_SEMICOLON] = {";", ":"},
    [KEY_APOSTROPHE] = {"'", "\""}, [KEY_GRAVE] = {"`", "~"}, [KEY_BACKSLASH] = {"\\", "|"},
    [KEY_COMMA] = {",", "<"}, [KEY_DOT] = {".", ">"}, [KEY_SLASH] = {"/", "?"},
    [KEY_102ND] = {"\\", "|"},
}};

static const struct layout it = {"it", {
    [KEY_1] = {"1", "!"}, [KEY_2] = {"2", "\""}, [KEY_3] = {"3", "£"}, [KEY_4] = {"4", "$"},
    [KEY_5] = {"5", "%"}, [KEY_6] = {"6", "&"}, [KEY_7] = {"7", "/"}, [KEY_8] = {"8", "("},
    [KEY_9] = {"9", ")"}, [KEY_0] = {"0", "="}, [KEY_MINUS] = {"'", "?", "`"},
    [KEY_EQUAL] = {"ì", "^", "~"}, [KEY_LEFTBRACE] = {"è", "é", "[", "{"},
    [KEY_RIGHTBRACE] = {"+", "*", "]", "}"}, [KEY_SEMICOLON] = {"ò", "ç", "@"},
    [KEY_APOSTROPHE] = {"à", "°", "#"}, [KEY_GRAVE] = {"\\", "|"}, [KEY_BACKSLASH] = {"ù", "§"},
    [KEY_COMMA] = {",", ";"}, [KEY_DOT] = {".", ":"}, [KEY_SLASH] = {"-", "_"},
    [KEY_102ND] = {"<", ">"}, [KEY_E] = {"e", "E", "€"},
}};

static const char *const letters[KEY_M + 1] = {
    [KEY_Q] = "q", [KEY_W] = "w", [KEY_E] = "e", [KEY_R] = "r", [KEY_T] = "t", [KEY_Y] = "y",
    [KEY_U] = "u", [KEY_I] = "i", [KEY_O] = "o", [KEY_P] = "p", [KEY_A] = "a", [KEY_S] = "s",
    [KEY_D] = "d", [KEY_F] = "f", [KEY_G] = "g", [KEY_H] = "h", [KEY_J] = "j", [KEY_K] = "k",
    [KEY_L] = "l", [KEY_Z] = "z", [KEY_X] = "x", [KEY_C] = "c", [KEY_V] = "v", [KEY_B] = "b",
    [KEY_N] = "n", [KEY_M] = "m",
};

static const struct {
    unsigned short code;
    const char *text;
} specials[] = {
    {KEY_ENTER, "\r"}, {KEY_KPENTER, "\r"}, {KEY_BACKSPACE, "\x7f"}, {KEY_TAB, "\t"},
    {KEY_ESC, "\x1b"}, {KEY_SPACE, " "}, {KEY_UP, "\x1b[A"}, {KEY_DOWN, "\x1b[B"},
    {KEY_RIGHT, "\x1b[C"}, {KEY_LEFT, "\x1b[D"}, {KEY_HOME, "\x1b[H"}, {KEY_END, "\x1b[F"},
    {KEY_INSERT, "\x1b[2~"}, {KEY_DELETE, "\x1b[3~"}, {KEY_PAGEUP, "\x1b[5~"},
    {KEY_PAGEDOWN, "\x1b[6~"}, {KEY_F1, "\x1bOP"}, {KEY_F2, "\x1bOQ"}, {KEY_F3, "\x1bOR"},
    {KEY_F4, "\x1bOS"}, {KEY_F5, "\x1b[15~"}, {KEY_F6, "\x1b[17~"}, {KEY_F7, "\x1b[18~"},
    {KEY_F8, "\x1b[19~"}, {KEY_F9, "\x1b[20~"}, {KEY_F10, "\x1b[21~"}, {KEY_F11, "\x1b[23~"},
    {KEY_F12, "\x1b[24~"}, {KEY_KPSLASH, "/"}, {KEY_KPASTERISK, "*"}, {KEY_KPMINUS, "-"},
    {KEY_KPPLUS, "+"},
};

// The keypad: its digit with Num Lock, the key printed under it without.
static const struct {
    unsigned short code;
    const char *number, *other;
} keypad[] = {
    {KEY_KP0, "0", "\x1b[2~"}, {KEY_KP1, "1", "\x1b[F"}, {KEY_KP2, "2", "\x1b[B"},
    {KEY_KP3, "3", "\x1b[6~"}, {KEY_KP4, "4", "\x1b[D"}, {KEY_KP5, "5", ""},
    {KEY_KP6, "6", "\x1b[C"}, {KEY_KP7, "7", "\x1b[H"}, {KEY_KP8, "8", "\x1b[A"},
    {KEY_KP9, "9", "\x1b[5~"}, {KEY_KPDOT, ".", "\x1b[3~"},
};

static const struct layout *layout = &us, *default_layout = &us;
static bool enabled = true;
static const char *settings_path;
static struct timespec settings_time; // of the settings.json read last
static struct keyboard keyboards[KEYBOARD_MAX] = {{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}};
static uint64_t checked; // event devices looked at: keyboards open, the others skipped
static bool shift_left, shift_right, ctrl_left, ctrl_right, alt, altgr, caps_lock, num_lock = true;
static bool meta_alone; // the Windows key is down and no other key was pressed with it

static void home_key(void) {
    static unsigned count;
    char text[16];
    int length = snprintf(text, sizeof(text), "%u\n", ++count);
    int fd = open(HOME_KEY_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        if (write(fd, text, length) == length) {
            rename(HOME_KEY_FILE ".tmp", HOME_KEY_FILE);
        }
        close(fd);
    }
}

static const struct layout *find_layout(const char *name, size_t length) {
    if (length == 2 && !strncmp(name, "us", 2)) {
        return &us;
    }
    if (length == 2 && !strncmp(name, "it", 2)) {
        return &it;
    }
    return NULL;
}

bool keyboard_layout(const char *name) {
    if (name && !strcmp(name, "none")) {
        enabled = false;
        return true;
    }
    const struct layout *chosen = !name || !*name ? &us : find_layout(name, strlen(name));
    if (!chosen) {
        return false;
    }
    layout = default_layout = chosen;
    return true;
}

void keyboard_settings(const char *path) {
    settings_path = path;
}

// The value of "layout" in the "keyboard" section of settings.json: the file is JSON as
// MicroPython's json.dump writes it, so looking for the two names is enough.
static void read_settings(void) {
    struct stat info;
    if (!settings_path || stat(settings_path, &info) < 0) {
        if (layout != default_layout) {
            layout = default_layout;
            fprintf(stderr, "mpy-repl-hub: keyboard layout %s\n", layout->name);
        }
        settings_time = (struct timespec){0};
        return;
    }
    if (info.st_mtim.tv_sec == settings_time.tv_sec && info.st_mtim.tv_nsec == settings_time.tv_nsec) {
        return;
    }
    settings_time = info.st_mtim;
    char text[8192];
    FILE *file = fopen(settings_path, "r");
    if (!file) {
        return;
    }
    size_t length = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[length] = 0;
    const struct layout *chosen = default_layout;
    char *section = strstr(text, "\"keyboard\"");
    char *end = section ? strchr(section, '}') : NULL;
    char *key = section ? strstr(section, "\"layout\"") : NULL;
    if (key && key < end) {
        char *value = strchr(key + 8, '"');
        char *close = value ? strchr(value + 1, '"') : NULL;
        const struct layout *named = close ? find_layout(value + 1, close - value - 1) : NULL;
        chosen = named ? named : chosen;
    }
    if (chosen != layout) {
        layout = chosen;
        fprintf(stderr, "mpy-repl-hub: keyboard layout %s, from %s\n", layout->name, settings_path);
    }
}

static void set_leds(void) {
    for (size_t i = 0; i < KEYBOARD_MAX; ++i) {
        if (keyboards[i].fd < 0) {
            continue;
        }
        struct input_event events[3] = {
            {.type = EV_LED, .code = LED_CAPSL, .value = caps_lock},
            {.type = EV_LED, .code = LED_NUML, .value = num_lock},
            {.type = EV_SYN, .code = SYN_REPORT},
        };
        if (write(keyboards[i].fd, events, sizeof(events)) < 0) {
            // a keyboard without these LEDs
        }
    }
}

static bool is_keyboard(int fd) {
    unsigned long keys[KEY_CNT / BITS_PER_LONG + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) {
        return false;
    }
    return TEST_BIT(KEY_A, keys) && TEST_BIT(KEY_Z, keys) && TEST_BIT(KEY_ENTER, keys) && TEST_BIT(KEY_SPACE, keys);
}

static void keyboard_close(struct keyboard *keyboard, const char *why) {
    fprintf(stderr, "mpy-repl-hub: keyboard event%d closed: %s\n", keyboard->number, why);
    close(keyboard->fd);
    checked &= ~(1ULL << keyboard->number);
    keyboard->fd = keyboard->number = -1;
    shift_left = shift_right = ctrl_left = ctrl_right = alt = altgr = false; // keys held when it went
}

void keyboard_scan(void) {
    if (!enabled) {
        return;
    }
    static bool started;
    if (!started) {
        // The count starts again with the hub: a file left from before would hide its first
        // presses from mpy_home, which watches for a change.
        started = true;
        unlink(HOME_KEY_FILE);
    }
    read_settings();
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        return;
    }
    uint64_t present = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        char *end;
        if (strncmp(entry->d_name, "event", 5) || !entry->d_name[5]) {
            continue;
        }
        long number = strtol(entry->d_name + 5, &end, 10);
        if (*end || number < 0 || number >= EVENT_DEVICES) {
            continue;
        }
        present |= 1ULL << number;
        if (checked & (1ULL << number)) {
            continue;
        }
        struct keyboard *slot = NULL;
        for (size_t i = 0; i < KEYBOARD_MAX && !slot; ++i) {
            slot = keyboards[i].fd < 0 ? &keyboards[i] : NULL;
        }
        if (!slot) {
            break;
        }
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%ld", number);
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue; // udev may not have set it up yet: tried again at the next scan
        }
        checked |= 1ULL << number;
        if (!is_keyboard(fd)) {
            close(fd);
            continue;
        }
        if (ioctl(fd, EVIOCGRAB, 1) < 0) {
            fprintf(stderr, "mpy-repl-hub: keyboard %s: cannot grab it: %s\n", path, strerror(errno));
        }
        char name[128] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        fprintf(stderr, "mpy-repl-hub: keyboard event%ld: %s, layout %s\n", number, name, layout->name);
        slot->fd = fd;
        slot->number = (int)number;
        set_leds();
    }
    closedir(dir);
    checked &= present; // a device that went away is looked at again when its number comes back
}

size_t keyboard_poll_fds(struct pollfd *fds, short events) {
    size_t count = 0;
    for (size_t i = 0; i < KEYBOARD_MAX; ++i) {
        if (keyboards[i].fd >= 0) {
            fds[count++] = (struct pollfd){keyboards[i].fd, events, 0};
        }
    }
    return count;
}

static size_t append(unsigned char *out, size_t length, size_t size, const char *text) {
    size_t n = strlen(text);
    if (length + n <= size) {
        memcpy(out + length, text, n);
        length += n;
    }
    return length;
}

// What a key types, or NULL. buffer holds a character made here.
static const char *translate(unsigned short code, char buffer[4]) {
    bool shift = shift_left || shift_right, ctrl = ctrl_left || ctrl_right;
    // US keyboards have no AltGr: the right Alt is one more Alt.
    bool third = altgr && layout != &us;
    if (code < sizeof(letters) / sizeof(letters[0]) && letters[code]) {
        if (third && layout->keys[code][2 + shift]) {
            return layout->keys[code][2 + shift];
        }
        char c = letters[code][0];
        if (ctrl) {
            buffer[0] = c & 0x1f;
        } else {
            buffer[0] = shift != caps_lock ? c - 'a' + 'A' : c;
        }
        buffer[1] = 0;
        return buffer;
    }
    if (code <= KEY_102ND && layout->keys[code][0]) {
        const char *const *text = layout->keys[code];
        const char *chosen = third ? (text[2 + shift] ? text[2 + shift] : text[2]) : text[shift];
        if (!chosen) {
            return NULL;
        }
        // Ctrl with the characters of the control codes: Ctrl-[ is ESC, Ctrl-] is 0x1d, ...
        if (ctrl && !chosen[1] && chosen[0] >= '@' && chosen[0] <= '_') {
            buffer[0] = chosen[0] & 0x1f;
            buffer[1] = 0;
            return buffer;
        }
        return chosen;
    }
    if (code == KEY_SPACE && ctrl) {
        return ""; // NUL cannot be appended as a string; Ctrl-Space does nothing
    }
    if (code == KEY_TAB && shift) {
        return "\x1b[Z";
    }
    for (size_t i = 0; i < sizeof(specials) / sizeof(specials[0]); ++i) {
        if (specials[i].code == code) {
            return specials[i].text;
        }
    }
    for (size_t i = 0; i < sizeof(keypad) / sizeof(keypad[0]); ++i) {
        if (keypad[i].code == code) {
            return num_lock && !shift ? keypad[i].number : keypad[i].other;
        }
    }
    return NULL;
}

static size_t key(unsigned short code, int value, unsigned char *out, size_t length, size_t size) {
    bool down = value != 0;
    if (value == 1 && code != KEY_LEFTMETA && code != KEY_RIGHTMETA) {
        meta_alone = false;
    }
    switch (code) {
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:
            if (value == 1) {
                meta_alone = true;
            } else if (value == 0 && meta_alone) {
                meta_alone = false;
                home_key();
            }
            return length;
        case KEY_COMPOSE: // the Menu key
            if (value == 1) {
                home_key();
            }
            return length;
        case KEY_LEFTSHIFT: shift_left = down; return length;
        case KEY_RIGHTSHIFT: shift_right = down; return length;
        case KEY_LEFTCTRL: ctrl_left = down; return length;
        case KEY_RIGHTCTRL: ctrl_right = down; return length;
        case KEY_LEFTALT: alt = down; return length;
        case KEY_RIGHTALT: altgr = down; return length;
        case KEY_CAPSLOCK:
            if (value == 1) {
                caps_lock = !caps_lock;
                set_leds();
            }
            return length;
        case KEY_NUMLOCK:
            if (value == 1) {
                num_lock = !num_lock;
                set_leds();
            }
            return length;
    }
    if (!down) {
        return length;
    }
    char buffer[4];
    const char *text = translate(code, buffer);
    if (!text || !*text) {
        return length;
    }
    if (alt || (altgr && layout == &us)) {
        length = append(out, length, size, "\x1b");
    }
    return append(out, length, size, text);
}

size_t keyboard_read(const struct pollfd *fds, size_t count, unsigned char *out, size_t length, size_t size) {
    for (size_t slot = 0; slot < count; ++slot) {
        if (!fds[slot].revents) {
            continue;
        }
        struct keyboard *keyboard = NULL;
        for (size_t i = 0; i < KEYBOARD_MAX && !keyboard; ++i) {
            keyboard = keyboards[i].fd == fds[slot].fd ? &keyboards[i] : NULL;
        }
        if (!keyboard) {
            continue;
        }
        struct input_event events[64];
        ssize_t got = read(keyboard->fd, events, sizeof(events));
        if (got < 0) {
            if (errno != EAGAIN && errno != EINTR) {
                keyboard_close(keyboard, strerror(errno)); // unplugged: ENODEV
            }
            continue;
        }
        for (size_t i = 0; i < (size_t)got / sizeof(events[0]); ++i) {
            if (events[i].type == EV_KEY) {
                length = key(events[i].code, events[i].value, out, length, size);
            }
        }
    }
    return length;
}
