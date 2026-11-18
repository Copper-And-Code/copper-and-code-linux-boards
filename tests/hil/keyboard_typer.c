// SPDX-License-Identifier: MIT
// A virtual keyboard (uinput) that types keys, for test_keyboard.sh: the REPL hub must find it
// and turn its keys into text as it does for a real one.
//
//   keyboard_typer KEY...     KEY is a key name, optionally with modifiers: a  shift+2
//                             altgr+leftbrace  ctrl+c  enter  up  kp5
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static const struct {
    const char *name;
    int code;
} names[] = {
    {"esc", KEY_ESC}, {"1", KEY_1}, {"2", KEY_2}, {"3", KEY_3}, {"4", KEY_4}, {"5", KEY_5},
    {"6", KEY_6}, {"7", KEY_7}, {"8", KEY_8}, {"9", KEY_9}, {"0", KEY_0}, {"minus", KEY_MINUS},
    {"equal", KEY_EQUAL}, {"backspace", KEY_BACKSPACE}, {"tab", KEY_TAB}, {"q", KEY_Q},
    {"w", KEY_W}, {"e", KEY_E}, {"r", KEY_R}, {"t", KEY_T}, {"y", KEY_Y}, {"u", KEY_U},
    {"i", KEY_I}, {"o", KEY_O}, {"p", KEY_P}, {"leftbrace", KEY_LEFTBRACE},
    {"rightbrace", KEY_RIGHTBRACE}, {"enter", KEY_ENTER}, {"a", KEY_A}, {"s", KEY_S},
    {"d", KEY_D}, {"f", KEY_F}, {"g", KEY_G}, {"h", KEY_H}, {"j", KEY_J}, {"k", KEY_K},
    {"l", KEY_L}, {"semicolon", KEY_SEMICOLON}, {"apostrophe", KEY_APOSTROPHE},
    {"grave", KEY_GRAVE}, {"backslash", KEY_BACKSLASH}, {"z", KEY_Z}, {"x", KEY_X},
    {"c", KEY_C}, {"v", KEY_V}, {"b", KEY_B}, {"n", KEY_N}, {"m", KEY_M}, {"comma", KEY_COMMA},
    {"dot", KEY_DOT}, {"slash", KEY_SLASH}, {"space", KEY_SPACE}, {"capslock", KEY_CAPSLOCK},
    {"102nd", KEY_102ND}, {"up", KEY_UP}, {"down", KEY_DOWN}, {"left", KEY_LEFT},
    {"right", KEY_RIGHT}, {"home", KEY_HOME}, {"end", KEY_END}, {"delete", KEY_DELETE},
    {"f1", KEY_F1}, {"f5", KEY_F5}, {"kp5", KEY_KP5}, {"kp8", KEY_KP8}, {"numlock", KEY_NUMLOCK},
    {"shift", KEY_LEFTSHIFT}, {"ctrl", KEY_LEFTCTRL}, {"alt", KEY_LEFTALT}, {"altgr", KEY_RIGHTALT},
};

static int lookup(const char *name, size_t length) {
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strlen(names[i].name) == length && !strncmp(names[i].name, name, length)) {
            return names[i].code;
        }
    }
    fprintf(stderr, "keyboard_typer: unknown key %.*s\n", (int)length, name);
    exit(2);
}

static void emit(int fd, int type, int code, int value) {
    struct input_event event = {.type = type, .code = code, .value = value};
    if (write(fd, &event, sizeof(event)) != sizeof(event)) {
        perror("keyboard_typer: write");
        exit(1);
    }
}

static void pause_ms(int ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void press(int fd, int code, int value) {
    emit(fd, EV_KEY, code, value);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    pause_ms(15);
}

int main(int argc, char **argv) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("keyboard_typer: /dev/uinput");
        return 1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        ioctl(fd, UI_SET_KEYBIT, names[i].code);
    }
    struct uinput_setup setup = {.id = {.bustype = BUS_VIRTUAL, .vendor = 1, .product = 1}};
    strcpy(setup.name, "mpy test keyboard");
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("keyboard_typer: create");
        return 1;
    }
    pause_ms(2500); // the hub looks for new keyboards every second
    for (int a = 1; a < argc; ++a) {
        int held[4], count = 0;
        const char *rest = argv[a], *plus;
        while ((plus = strchr(rest, '+')) && plus[1]) {
            held[count++] = lookup(rest, plus - rest);
            rest = plus + 1;
        }
        int code = lookup(rest, strlen(rest));
        for (int i = 0; i < count; ++i) {
            press(fd, held[i], 1);
        }
        press(fd, code, 1);
        press(fd, code, 0);
        for (int i = count - 1; i >= 0; --i) {
            press(fd, held[i], 0);
        }
    }
    pause_ms(300);
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
