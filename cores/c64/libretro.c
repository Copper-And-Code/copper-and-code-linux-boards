// SPDX-License-Identifier: MIT
// The C64 core of the STM32H743 and RP2350 ports (core/) as a libretro core: PAL C64 with 6510,
// CIAs, VIC-II, SID 6581, the emulated 1541 and 8/16 KiB cartridges.
//
// Games: .prg (a BASIC program is RUN), .d64 (LOAD"*",8,1 and RUN are typed), .crt (the cartridge
// starts). The Commodore ROMs are the user's, in the system directory, in c64/ or directly:
// basic.901226-01.bin, kernal.901227-03.bin, characters.901225-01.bin and, for the 1541,
// dos1541-325302-01+901229-05.bin.
//
// While the 1541's motor turns the machine runs three frames for each one shown: disks load
// three times sooner (the core has room: 3.3 times real time on one Cortex-A7).
//
// Input: the whole keyboard by position (as VICE's positional map): Esc RUN/STOP, Tab CTRL, left
// Ctrl C=, Home CLR/HOME, Insert £, Page Up ↑, ` ←, the cursor keys with SHIFT for up and left,
// F2/F4/F6/F8 as SHIFT+F1/F3/F5/F7. The joystick in port 2 from the gamepad, or from the keypad
// (8 4 6 2, 0 or 5 fire); Select on the gamepad moves it to port 1 and back.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

#include "core/c1541.h"
#include "core/c64.h"
#include "core/crt.h"
#include "core/sid.h"
#include "core/vic_render.h"

#define C64_CLOCK 985248u
#define FRAME_CYCLES 19656u
#define SAMPLE_RATE 48000u
#define BOOT_FRAMES 160  // BASIC is ready for typed text after about three seconds
#define WARP_FRAMES 3    // while the disk drive's motor turns

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static uint8_t basic[8192], kernal[8192], chars[4096], dos[16384];
static bool have_dos;
static uint8_t picture[VICR_LINES][VICR_PACKED];
static uint16_t screen[VICR_LINES][VICR_W];
static int16_t mono[4096], stereo[2 * 4096];
static unsigned samples;
static uint32_t sample_fraction;
static unsigned boot_frames;
static char typed[256];
static unsigned typed_head, typed_tail;
static uint8_t *disk;
static uint32_t disk_size;
static uint8_t cart[65536];
static uint8_t *pending_prg;
static size_t pending_prg_size;
static unsigned joystick_port = 2;
static bool select_was_down;

static void log_message(enum retro_log_level level, const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (log_cb) {
        char text[256];
        vsnprintf(text, sizeof(text), format, args);
        log_cb(level, "%s", text);
    } else {
        vfprintf(stderr, format, args);
    }
    va_end(args);
}

// ---- The machine ----

static uint8_t *line_out(unsigned line) {
    return picture[line - VICR_FIRST_LINE];
}

static void audio_put(int16_t value) {
    if (samples < sizeof(mono) / sizeof(mono[0])) {
        mono[samples++] = value;
    }
}

static void type_text(const char *text) {
    for (; *text && typed_head - typed_tail < sizeof(typed); ++text) {
        typed[typed_head++ % sizeof(typed)] = *text == '\n' ? 13 : *text;
    }
}

static bool disk_read(uint32_t offset, uint8_t *buffer, uint32_t size) {
    if (offset > disk_size || size > disk_size - offset) {
        return false;
    }
    memcpy(buffer, disk + offset, size);
    return true;
}

static void reset(void) {
    c64m.ring_enabled = false;
    c64_power_cycle();
    c64m.ring_head = c64m.ring_tail = 0;
    c64m.lines = 0;
    vicr_init();
    c64m.ring_enabled = true;
    typed_head = typed_tail = 0;
    samples = 0;
    sample_fraction = 0;
    boot_frames = 0;
    memset(c64m.kbd, 0, sizeof(c64m.kbd));
    c64m.joy1 = c64m.joy2 = 0;
}

// A program into RAM at its load address; a BASIC one gets its pointers and RUN.
static void load_prg(const uint8_t *data, size_t size) {
    if (size < 3) {
        return;
    }
    unsigned start = data[0] | ((unsigned)data[1] << 8);
    size_t length = size - 2;
    if (length > 0x10000u - start) {
        length = 0x10000u - start;
    }
    memcpy(c64m.ram + start, data + 2, length);
    if (start == 0x0801) {
        unsigned end = start + (unsigned)length;
        c64m.ram[0x2d] = c64m.ram[0x2f] = c64m.ram[0x31] = (uint8_t)end;
        c64m.ram[0x2e] = c64m.ram[0x30] = c64m.ram[0x32] = (uint8_t)(end >> 8);
        type_text("RUN\n");
    } else {
        log_message(RETRO_LOG_INFO, "c64: program at $%04X: start it with SYS\n", start);
    }
}

// ---- Keyboard: (libretro key, matrix column, row, with SHIFT) ----

static const struct {
    unsigned key;
    uint8_t column, row;
    bool shift;
} keymap[] = {
    {RETROK_BACKSPACE, 0, 0, false}, {RETROK_RETURN, 0, 1, false}, {RETROK_KP_ENTER, 0, 1, false},
    {RETROK_RIGHT, 0, 2, false}, {RETROK_LEFT, 0, 2, true}, {RETROK_DOWN, 0, 7, false}, {RETROK_UP, 0, 7, true},
    {RETROK_F7, 0, 3, false}, {RETROK_F8, 0, 3, true}, {RETROK_F1, 0, 4, false}, {RETROK_F2, 0, 4, true},
    {RETROK_F3, 0, 5, false}, {RETROK_F4, 0, 5, true}, {RETROK_F5, 0, 6, false}, {RETROK_F6, 0, 6, true},
    {RETROK_3, 1, 0, false}, {RETROK_w, 1, 1, false}, {RETROK_a, 1, 2, false}, {RETROK_4, 1, 3, false},
    {RETROK_z, 1, 4, false}, {RETROK_s, 1, 5, false}, {RETROK_e, 1, 6, false}, {RETROK_LSHIFT, 1, 7, false},
    {RETROK_5, 2, 0, false}, {RETROK_r, 2, 1, false}, {RETROK_d, 2, 2, false}, {RETROK_6, 2, 3, false},
    {RETROK_c, 2, 4, false}, {RETROK_f, 2, 5, false}, {RETROK_t, 2, 6, false}, {RETROK_x, 2, 7, false},
    {RETROK_7, 3, 0, false}, {RETROK_y, 3, 1, false}, {RETROK_g, 3, 2, false}, {RETROK_8, 3, 3, false},
    {RETROK_b, 3, 4, false}, {RETROK_h, 3, 5, false}, {RETROK_u, 3, 6, false}, {RETROK_v, 3, 7, false},
    {RETROK_9, 4, 0, false}, {RETROK_i, 4, 1, false}, {RETROK_j, 4, 2, false}, {RETROK_0, 4, 3, false},
    {RETROK_m, 4, 4, false}, {RETROK_k, 4, 5, false}, {RETROK_o, 4, 6, false}, {RETROK_n, 4, 7, false},
    {RETROK_MINUS, 5, 0, false}, {RETROK_p, 5, 1, false}, {RETROK_l, 5, 2, false}, {RETROK_EQUALS, 5, 3, false},
    {RETROK_PERIOD, 5, 4, false}, {RETROK_SEMICOLON, 5, 5, false}, {RETROK_LEFTBRACKET, 5, 6, false},
    {RETROK_COMMA, 5, 7, false},
    {RETROK_INSERT, 6, 0, false}, {RETROK_RIGHTBRACKET, 6, 1, false}, {RETROK_QUOTE, 6, 2, false},
    {RETROK_HOME, 6, 3, false}, {RETROK_RSHIFT, 6, 4, false}, {RETROK_BACKSLASH, 6, 5, false},
    {RETROK_PAGEUP, 6, 6, false}, {RETROK_SLASH, 6, 7, false},
    {RETROK_1, 7, 0, false}, {RETROK_BACKQUOTE, 7, 1, false}, {RETROK_TAB, 7, 2, false}, {RETROK_2, 7, 3, false},
    {RETROK_SPACE, 7, 4, false}, {RETROK_LCTRL, 7, 5, false}, {RETROK_q, 7, 6, false}, {RETROK_ESCAPE, 7, 7, false},
};

static void read_input(void) {
    input_poll_cb();
    uint8_t matrix[8] = {0};
    bool shifted = false;
    for (size_t i = 0; i < sizeof(keymap) / sizeof(keymap[0]); ++i) {
        if (input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, keymap[i].key)) {
            matrix[keymap[i].column] |= (uint8_t)(1u << keymap[i].row);
            shifted |= keymap[i].shift;
        }
    }
    if (shifted) {
        matrix[1] |= 0x80;  // left SHIFT
    }
    // A typed line (autostart) goes into the keyboard buffer once BASIC is up.
    if (boot_frames >= BOOT_FRAMES && typed_tail != typed_head && c64m.ram[0xc6] < 10) {
        c64m.ram[0x277 + c64m.ram[0xc6]++] = (uint8_t)typed[typed_tail++ % sizeof(typed)];
    }
    memcpy(c64m.kbd, matrix, sizeof(matrix));

    uint8_t joy = 0;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP) || input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP8)) {
        joy |= 1;
    }
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN) || input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP2)) {
        joy |= 2;
    }
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT) || input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP4)) {
        joy |= 4;
    }
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT) || input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP6)) {
        joy |= 8;
    }
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) || input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A)
        || input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP0) || input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP5)) {
        joy |= 16;
    }
    bool select = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT) != 0;
    if (select && !select_was_down) {
        joystick_port = joystick_port == 2 ? 1 : 2;
        log_message(RETRO_LOG_INFO, "c64: joystick in port %u\n", joystick_port);
    }
    select_was_down = select;
    c64m.joy1 = joystick_port == 1 ? joy : 0;
    c64m.joy2 = joystick_port == 2 ? joy : 0;
}

// ---- libretro ----

unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
        log_cb = logging.log;
    }
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port, (void)device; }

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "C64 (lvgl-mpy)";
    info->library_version = "1";
    info->valid_extensions = "prg|d64|crt";
    info->need_fullpath = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = info->geometry.max_width = VICR_W;
    info->geometry.base_height = info->geometry.max_height = VICR_LINES;
    info->geometry.aspect_ratio = VICR_W * 0.9365f / VICR_LINES;  // PAL pixels are a little narrow
    info->timing.fps = (double)C64_CLOCK / FRAME_CYCLES;
    info->timing.sample_rate = SAMPLE_RATE;
}

static bool load_rom(const char *dir, const char *name, uint8_t *out, size_t size) {
    const char *places[] = {"%s/c64/%s", "%s/%s"};
    for (size_t i = 0; i < 2; ++i) {
        char path[1024];
        snprintf(path, sizeof(path), places[i], dir, name);
        FILE *file = fopen(path, "rb");
        if (file) {
            size_t got = fread(out, 1, size, file);
            fclose(file);
            if (got == size) {
                return true;
            }
        }
    }
    return false;
}

void retro_init(void) {}

bool retro_load_game(const struct retro_game_info *game) {
    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
        return false;
    }
    const char *dir = NULL;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) || !dir) {
        dir = ".";
    }
    if (!load_rom(dir, "basic.901226-01.bin", basic, sizeof(basic))
        || !load_rom(dir, "kernal.901227-03.bin", kernal, sizeof(kernal))
        || !load_rom(dir, "characters.901225-01.bin", chars, sizeof(chars))) {
        log_message(RETRO_LOG_ERROR, "c64: the Commodore ROMs are missing from %s/c64\n", dir);
        return false;
    }
    have_dos = load_rom(dir, "dos1541-325302-01+901229-05.bin", dos, sizeof(dos));
    sid_init(audio_put);
    sid_set_model(6581);
    c1541_init(have_dos ? dos : NULL);
    c64_init(basic, kernal, chars);
    c1541_power(have_dos, c64m.cpu.cycles);
    memset(picture, 0, sizeof(picture));

    if (!game || !game->data || game->size < 2) {
        reset();
        return true;  // BASIC alone
    }
    const char *path = game->path ? game->path : "";
    const char *dot = strrchr(path, '.');
    char extension[8] = "";
    for (size_t i = 0; dot && dot[i + 1] && i < sizeof(extension) - 1; ++i) {
        extension[i] = (char)(dot[i + 1] | 0x20);
    }
    if (!strcmp(extension, "crt")) {
        if (game->size > sizeof(cart)) {
            return false;
        }
        memcpy(cart, game->data, game->size);
        crt_t image;
        const char *error = crt_parse(cart, game->size, &image);
        if (error) {
            log_message(RETRO_LOG_ERROR, "c64: %s\n", error);
            return false;
        }
        c64_cart(image.roml, image.romh, image.game, image.exrom);
        reset();
    } else if (!strcmp(extension, "d64")) {
        if (!have_dos) {
            log_message(RETRO_LOG_ERROR, "c64: a disk needs the 1541 ROM in %s/c64\n", dir);
            return false;
        }
        disk_size = (uint32_t)game->size;
        if (disk_size != 174848 && disk_size != 175531 && disk_size != 196608 && disk_size != 197376) {
            log_message(RETRO_LOG_ERROR, "c64: not a D64 image\n");
            return false;
        }
        disk = malloc(disk_size);
        if (!disk) {
            return false;
        }
        memcpy(disk, game->data, disk_size);
        reset();
        c1541_insert_reader(disk_read, disk_size);
        type_text("LOAD\"*\",8,1\nRUN\n");
    } else {
        reset();
        pending_prg = malloc(game->size);
        if (pending_prg) {
            memcpy(pending_prg, game->data, game->size);
            pending_prg_size = game->size;
        }
    }
    return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t count) {
    (void)type, (void)info, (void)count;
    return false;
}

void retro_run(void) {
    read_input();
    if (pending_prg && boot_frames == BOOT_FRAMES) {
        load_prg(pending_prg, pending_prg_size);
        free(pending_prg);
        pending_prg = NULL;
    }
    // While the 1541's motor turns, WARP_FRAMES frames for one: a disk loads that much sooner. Only
    // the last frame's sound is heard.
    c1541_state_t drive;
    c1541_state(&drive);
    unsigned frames = drive.motor ? WARP_FRAMES : 1;
    for (unsigned f = 0; f < frames; ++f) {
        samples = 0;
        uint64_t n = (uint64_t)SAMPLE_RATE * FRAME_CYCLES + sample_fraction;
        sid_frame_begin(c64m.line_start, FRAME_CYCLES, (unsigned)(n / C64_CLOCK));
        sample_fraction = (uint32_t)(n % C64_CLOCK);
        do {
            unsigned line = c64m.line;
            uint32_t start = c64m.line_start;
            c64_run_line();
            c64_ring_line(line, start);
            while (c64m.ring_tail != c64m.ring_head) {
                c64_vic_write_t e = c64m.ring[c64m.ring_tail++ & (C64_VIC_RING - 1)];
                vicr_feed(&e, line_out);
            }
        } while (c64m.line != 0);
        sid_frame_end();
        ++boot_frames;
    }

    for (unsigned y = 0; y < VICR_LINES; ++y) {
        vicr_expand_native(picture[y], screen[y]);
    }
    video_cb(screen, VICR_W, VICR_LINES, VICR_W * sizeof(uint16_t));
    for (unsigned i = 0; i < samples; ++i) {
        stereo[2 * i] = stereo[2 * i + 1] = mono[i];
    }
    if (samples) {
        audio_batch_cb(stereo, samples);
    }
    samples = 0;
}

void retro_reset(void) {
    reset();
}

void retro_unload_game(void) {
    free(disk);
    disk = NULL;
    disk_size = 0;
    free(pending_prg);
    pending_prg = NULL;
}

void retro_deinit(void) {}
unsigned retro_get_region(void) { return RETRO_REGION_PAL; }
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data, (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data, (void)size; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index, (void)enabled, (void)code; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
