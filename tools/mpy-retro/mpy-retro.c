// SPDX-License-Identifier: MIT
// A libretro frontend for Linux boards: one core, one game, full screen.
//
//   mpy-retro CORE.so GAME [--frames N]
//
// Video: DRM/KMS, the screen display_driver would choose (a monitor connected wins over the board's
// panel, MPY_DISPLAY_CONNECTOR and MPY_DISPLAY_MODE as there) at the refresh nearest the game's,
// three dumb buffers flipped on the vertical blank. A second thread scales the core's frame to the
// largest size with its aspect that fits, and turns it with the panel (MPY_DISPLAY_ROTATION on the
// board's own panel), so the core keeps one CPU to itself. Whoever had the screen (MicroPython)
// must let it go first.
//
// Audio: ALSA, the device of MPY_AUDIO_SINK ("alsasink device=plughw:0,0") or "default". Writing
// the samples paces the emulation; without a sound card a clock does.
//
// Input: keyboards and gamepads through evdev, as player 1's RetroPad. Keyboard: arrows, X = A,
// Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Shift or Backspace = Select. Gamepad: by
// position (south B, east A, west Y, north X), shoulders L and R, Select, Start, d-pad, hat or
// left stick. The REPL hub lends the keyboards while /run/mpy-keyboard-lent names a live process.
//
// A core that reads the keyboard (RETRO_DEVICE_KEYBOARD, a computer such as the C64) gets all of
// it: then the keys are no RetroPad, and Esc is the core's.
//
// Leaving: Esc (unless the core reads the keyboard), the Windows key, Select and Start together,
// the gamepad's home button, SIGTERM or SIGINT. The save RAM is kept next to the game, as GAME.srm;
// the cores' ROMs are in /usr/local/mpy/retro/system (MPY_RETRO_SYSTEM).

#define _GNU_SOURCE
#include <dirent.h>
#include <math.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "libretro.h"

#define LENT_FILE "/run/mpy-keyboard-lent"
#define MAX_INPUTS 8
#define BITS_PER_LONG (8 * sizeof(long))
#define TEST_BIT(bit, array) (((array)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1)

static volatile sig_atomic_t quit;

static void die(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, "mpy-retro: ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(1);
}

static long long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// ---- The core ----

static struct {
    void *handle;
    void (*init)(void);
    void (*deinit)(void);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void (*run)(void);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
} core;

static void *symbol(const char *name) {
    void *address = dlsym(core.handle, name);
    if (!address) {
        die("%s: no %s", "core", name);
    }
    return address;
}

static void load_core(const char *path) {
    core.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!core.handle) {
        die("%s", dlerror());
    }
    core.init = symbol("retro_init");
    core.deinit = symbol("retro_deinit");
    core.set_environment = symbol("retro_set_environment");
    core.set_video_refresh = symbol("retro_set_video_refresh");
    core.set_audio_sample = symbol("retro_set_audio_sample");
    core.set_audio_sample_batch = symbol("retro_set_audio_sample_batch");
    core.set_input_poll = symbol("retro_set_input_poll");
    core.set_input_state = symbol("retro_set_input_state");
    core.get_system_info = symbol("retro_get_system_info");
    core.get_system_av_info = symbol("retro_get_system_av_info");
    core.load_game = symbol("retro_load_game");
    core.unload_game = symbol("retro_unload_game");
    core.run = symbol("retro_run");
    core.get_memory_data = symbol("retro_get_memory_data");
    core.get_memory_size = symbol("retro_get_memory_size");
}

static enum retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static char system_dir[512] = "/usr/local/mpy/retro/system";  // the cores' ROMs (MPY_RETRO_SYSTEM)
static char save_dir[512] = ".";  // next to the game
static float aspect;  // the core's, width over height; 0: its pixels are square
static double fps = 60;  // the core's frames a second

static void core_log(enum retro_log_level level, const char *format, ...) {
    if (level < RETRO_LOG_WARN) {
        return;
    }
    va_list args;
    va_start(args, format);
    fprintf(stderr, "core: ");
    vfprintf(stderr, format, args);
    va_end(args);
}

static bool environment(unsigned command, void *data) {
    switch (command) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            enum retro_pixel_format format = *(const enum retro_pixel_format *)data;
            if (format != RETRO_PIXEL_FORMAT_RGB565 && format != RETRO_PIXEL_FORMAT_XRGB8888
                && format != RETRO_PIXEL_FORMAT_0RGB1555) {
                return false;
            }
            pixel_format = format;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char **)data = system_dir;
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char **)data = save_dir;
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((struct retro_log_callback *)data)->log = core_log;
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool *)data = true;
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool *)data = false;
            return true;
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {
            const struct retro_game_geometry *geometry = data;
            aspect = geometry->aspect_ratio > 0 ? geometry->aspect_ratio : 0;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_MESSAGE:
            fprintf(stderr, "core: %s\n", ((const struct retro_message *)data)->msg);
            return true;
        default:
            return false;
    }
}

// ---- Video ----

static struct {
    int fd;
    drmModeConnector *connector;
    drmModeModeInfo mode;
    uint32_t crtc_id;
    drmModeCrtc *saved_crtc;
    struct {
        uint32_t handle, fb_id, pitch;
        uint64_t size;
        uint16_t *map;
    } buffer[3];
    int back;
    int rotation;
    bool flip_pending;
} drm = {.fd = -1};

static bool connector_internal(uint32_t type) {
    return type == DRM_MODE_CONNECTOR_DSI || type == DRM_MODE_CONNECTOR_LVDS || type == DRM_MODE_CONNECTOR_eDP
        || type == DRM_MODE_CONNECTOR_DPI || type == DRM_MODE_CONNECTOR_SPI;
}

// The same choice as linuxfb.open(): a connected monitor before the board's panel, the preferred
// mode, else the largest at 50 Hz or more.
static void open_screen(void) {
    drm.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm.fd < 0 || drmSetMaster(drm.fd) != 0) {
        die("the screen is in use (MicroPython's display_driver.release() lets it go)");
    }
    drmModeRes *resources = drmModeGetResources(drm.fd);
    if (!resources) {
        die("DRM resources: %s", strerror(errno));
    }
    const char *wanted = getenv("MPY_DISPLAY_CONNECTOR");
    for (int pass = 0; pass < 2 && !drm.connector; ++pass) {
        for (int i = 0; i < resources->count_connectors && !drm.connector; ++i) {
            drmModeConnector *connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
            if (!connector) {
                continue;
            }
            char name[32];
            const char *type = drmModeGetConnectorTypeName(connector->connector_type);
            snprintf(name, sizeof(name), "%s-%u", type ? type : "Unknown", connector->connector_type_id);
            bool usable = connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0
                && connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK;
            bool chosen = wanted && *wanted ? !strcmp(name, wanted)
                : connector_internal(connector->connector_type) == (pass == 1);
            if (usable && chosen) {
                drm.connector = connector;
            } else {
                drmModeFreeConnector(connector);
            }
        }
    }
    if (!drm.connector) {
        die("no screen connected");
    }
    int chosen = -1, want_width = 0, want_height = 0;
    const char *mode = getenv("MPY_DISPLAY_MODE");
    if (mode && *mode) {
        sscanf(mode, "%dx%d", &want_width, &want_height);
    }
    for (int i = 0; i < drm.connector->count_modes && chosen < 0; ++i) {
        const drmModeModeInfo *candidate = &drm.connector->modes[i];
        if (want_width ? candidate->hdisplay == want_width && candidate->vdisplay == want_height
                       : (candidate->type & DRM_MODE_TYPE_PREFERRED) != 0) {
            chosen = i;
        }
    }
    for (int i = 0; chosen < 0 && i < drm.connector->count_modes; ++i) {
        if (drm.connector->modes[i].vrefresh >= 50) {
            chosen = i;  // the kernel lists the largest first
        }
    }
    drm.mode = drm.connector->modes[chosen < 0 ? 0 : chosen];
    // At that size, the refresh nearest the game's: the STM32MP157C-DK2's panel prefers 50 Hz but
    // also has 60, which shows every frame of a 60 Hz game.
    for (int i = 0; i < drm.connector->count_modes; ++i) {
        const drmModeModeInfo *candidate = &drm.connector->modes[i];
        if (candidate->hdisplay == drm.mode.hdisplay && candidate->vdisplay == drm.mode.vdisplay
            && fabs(candidate->vrefresh - fps) < fabs(drm.mode.vrefresh - fps)) {
            drm.mode = *candidate;
        }
    }
    drm.crtc_id = resources->count_crtcs > 0 ? resources->crtcs[0] : 0;
    if (drm.connector->encoder_id) {
        drmModeEncoder *encoder = drmModeGetEncoder(drm.fd, drm.connector->encoder_id);
        if (encoder && encoder->crtc_id) {
            drm.crtc_id = encoder->crtc_id;
        }
        drmModeFreeEncoder(encoder);
    }
    drmModeFreeResources(resources);
    drm.saved_crtc = drmModeGetCrtc(drm.fd, drm.crtc_id);
    const char *rotation = getenv("MPY_DISPLAY_ROTATION");
    drm.rotation = connector_internal(drm.connector->connector_type) && rotation ? atoi(rotation) : 0;
    for (int i = 0; i < 3; ++i) {
        if (drmModeCreateDumbBuffer(drm.fd, drm.mode.hdisplay, drm.mode.vdisplay, 16, 0, &drm.buffer[i].handle,
                &drm.buffer[i].pitch, &drm.buffer[i].size)
            || drmModeAddFB(drm.fd, drm.mode.hdisplay, drm.mode.vdisplay, 16, 16, drm.buffer[i].pitch,
                drm.buffer[i].handle, &drm.buffer[i].fb_id)) {
            die("DRM buffer: %s", strerror(errno));
        }
        uint64_t offset;
        drmModeMapDumbBuffer(drm.fd, drm.buffer[i].handle, &offset);
        drm.buffer[i].map = mmap(NULL, drm.buffer[i].size, PROT_READ | PROT_WRITE, MAP_SHARED, drm.fd, offset);
        if (drm.buffer[i].map == MAP_FAILED) {
            die("DRM map: %s", strerror(errno));
        }
        memset(drm.buffer[i].map, 0, drm.buffer[i].size);
    }
    if (drmModeSetCrtc(drm.fd, drm.crtc_id, drm.buffer[0].fb_id, 0, 0, &drm.connector->connector_id, 1, &drm.mode)) {
        die("DRM mode: %s", strerror(errno));
    }
    fprintf(stderr, "mpy-retro: screen %ux%u at %u Hz, rotation %d\n", drm.mode.hdisplay, drm.mode.vdisplay,
        drm.mode.vrefresh, drm.rotation);
}

static void close_screen(void) {
    if (drm.fd < 0) {
        return;
    }
    // The last image stays until MicroPython opens the screen again and draws.
    for (int i = 0; i < 3; ++i) {
        munmap(drm.buffer[i].map, drm.buffer[i].size);
    }
    drmDropMaster(drm.fd);
    close(drm.fd);  // the kernel frees the buffers with the descriptor
    drm.fd = -1;
}

static unsigned long frames_shown, draws;
static long long draw_us;

static void flip_done(int fd, unsigned frame, unsigned sec, unsigned usec, void *data) {
    (void)fd, (void)frame, (void)sec, (void)usec, (void)data;
    drm.flip_pending = false;
}

// The core's latest frame, handed to the video thread.
static struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    uint8_t *pixels;
    unsigned width, height, pitch;
    size_t capacity;
    bool fresh;
} frame = {.lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER};

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (!data) {
        return;  // a duplicated frame: the screen keeps the last one
    }
    unsigned bytes = pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
    pthread_mutex_lock(&frame.lock);
    size_t needed = (size_t)width * height * bytes;
    if (needed > frame.capacity) {
        free(frame.pixels);
        frame.pixels = malloc(needed);
        frame.capacity = frame.pixels ? needed : 0;
    }
    if (frame.pixels) {
        for (unsigned y = 0; y < height; ++y) {
            memcpy(frame.pixels + (size_t)y * width * bytes, (const uint8_t *)data + y * pitch, (size_t)width * bytes);
        }
        frame.width = width;
        frame.height = height;
        frame.pitch = width * bytes;
        frame.fresh = true;
        pthread_cond_signal(&frame.ready);
    }
    pthread_mutex_unlock(&frame.lock);
}

static inline uint16_t to565(const uint8_t *pixels, size_t index) {
    if (pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
        uint32_t c = ((const uint32_t *)pixels)[index];
        return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
    }
    uint16_t c = ((const uint16_t *)pixels)[index];
    if (pixel_format == RETRO_PIXEL_FORMAT_0RGB1555) {
        return (uint16_t)(((c << 1) & 0xFFC0) | (c & 0x001F));
    }
    return c;
}

// Scales (nearest) and turns the frame into the back buffer. Physical pixel (px, py) of the panel
// shows logical pixel (lx, ly) of the turned screen, as LVGL turns it (lv_display_rotate_point).
static void draw(const uint8_t *pixels, unsigned width, unsigned height, int *last_w, int *last_h) {
    int pw = drm.mode.hdisplay, ph = drm.mode.vdisplay;
    int lw = drm.rotation % 180 ? ph : pw, lh = drm.rotation % 180 ? pw : ph;
    float ratio = aspect > 0 ? aspect : (float)width / height;
    int tw = lw, th = (int)(lw / ratio);
    if (th > lh) {
        th = lh;
        tw = (int)(lh * ratio);
    }
    int ox = (lw - tw) / 2, oy = (lh - th) / 2;
    uint16_t *out = drm.buffer[drm.back].map;
    int stride = drm.buffer[drm.back].pitch / 2;
    if (tw != last_w[drm.back] || th != last_h[drm.back]) {
        memset(out, 0, drm.buffer[drm.back].size);  // a new size: the old borders go
        last_w[drm.back] = tw;
        last_h[drm.back] = th;
    }
    static int xs[4096], ys[4096];
    for (int i = 0; i < tw && i < 4096; ++i) {
        xs[i] = i * (int)width / tw;
    }
    for (int i = 0; i < th && i < 4096; ++i) {
        ys[i] = i * (int)height / th;
    }
    bool direct = pixel_format == RETRO_PIXEL_FORMAT_RGB565;
    const uint16_t *source = (const uint16_t *)pixels;
    // The screen's buffers are uncached (write-combined): rows are built in a cached buffer and
    // only written there, in order; a row scaled up again is the same buffer written again.
    static uint16_t line[4096];
    bool have_line = false;
    size_t line_source = 0;
    if (drm.rotation == 90) {
        // Physical row py is logical column lx = ph - 1 - py: one column of the source.
        for (int py = 0; py < ph; ++py) {
            int lx = ph - 1 - py - ox;
            if (lx < 0 || lx >= tw) {
                continue;
            }
            size_t column = (size_t)xs[lx];
            if (!have_line || column != line_source) {
                if (direct) {
                    for (int ly = 0; ly < th; ++ly) {
                        line[ly] = source[(size_t)ys[ly] * width + column];
                    }
                } else {
                    for (int ly = 0; ly < th; ++ly) {
                        line[ly] = to565(pixels, (size_t)ys[ly] * width + column);
                    }
                }
                have_line = true;
                line_source = column;
            }
            memcpy(out + (size_t)py * stride + oy, line, (size_t)th * 2);
        }
    } else if (drm.rotation == 0) {
        for (int ly = 0; ly < th; ++ly) {
            size_t base = (size_t)ys[ly] * width;
            if (!have_line || base != line_source) {
                if (direct) {
                    for (int lx = 0; lx < tw; ++lx) {
                        line[lx] = source[base + xs[lx]];
                    }
                } else {
                    for (int lx = 0; lx < tw; ++lx) {
                        line[lx] = to565(pixels, base + xs[lx]);
                    }
                }
                have_line = true;
                line_source = base;
            }
            memcpy(out + (size_t)(ly + oy) * stride + ox, line, (size_t)tw * 2);
        }
    } else {
        for (int py = 0; py < ph; ++py) {
            uint16_t *row = out + (size_t)py * stride;
            for (int px = 0; px < pw; ++px) {
                int lx = drm.rotation == 180 ? pw - 1 - px : py;
                int ly = drm.rotation == 180 ? ph - 1 - py : pw - 1 - px;
                lx -= ox;
                ly -= oy;
                if (lx >= 0 && ly >= 0 && lx < tw && ly < th) {
                    row[px] = to565(pixels, (size_t)ys[ly] * width + xs[lx]);
                }
            }
        }
    }
}

// Three buffers: the one shown, one waiting for the vertical blank, and the one being drawn, so a
// frame that comes while a flip waits is drawn at once and shown at the next blank.
static void *video_thread(void *unused) {
    (void)unused;
    uint8_t *copy = NULL;
    size_t capacity = 0;
    int last_w[3] = {0}, last_h[3] = {0};
    int front = 0, pending = -1, ready = -1;
    drmEventContext events = {.version = 2, .page_flip_handler = flip_done};
    while (!quit) {
        struct pollfd fds = {drm.fd, POLLIN, 0};
        if (pending >= 0 && poll(&fds, 1, 0) > 0) {
            drmHandleEvent(drm.fd, &events);
            if (!drm.flip_pending) {
                front = pending;
                pending = -1;
                ++frames_shown;
            }
        }
        if (pending < 0 && ready >= 0) {
            if (drmModePageFlip(drm.fd, drm.crtc_id, drm.buffer[ready].fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0) {
                drm.flip_pending = true;
                pending = ready;
            }
            ready = -1;
        }
        if (ready >= 0) {
            // A frame already waits for its blank: the next one waits its turn (the sound paces
            // the core, which hands over its frames in small bursts), so none is lost.
            struct pollfd wait = {drm.fd, POLLIN, 0};
            poll(&wait, 1, 2);
            continue;
        }
        pthread_mutex_lock(&frame.lock);
        if (!frame.fresh) {
            // Two milliseconds at most: a flip may end meanwhile.
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 2000000;
            if (until.tv_nsec >= 1000000000) {
                until.tv_sec += 1;
                until.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&frame.ready, &frame.lock, &until);
        }
        if (!frame.fresh || quit) {
            pthread_mutex_unlock(&frame.lock);
            continue;
        }
        size_t size = (size_t)frame.pitch * frame.height;
        if (size > capacity) {
            free(copy);
            copy = malloc(size);
            capacity = copy ? size : 0;
        }
        unsigned width = frame.width, height = frame.height;
        if (copy) {
            memcpy(copy, frame.pixels, size);
        }
        frame.fresh = false;
        pthread_mutex_unlock(&frame.lock);
        if (!copy) {
            continue;
        }
        // A buffer neither shown nor waiting for its blank.
        drm.back = (front + 1) % 3 == pending ? (front + 2) % 3 : (front + 1) % 3;
        long long drawing = now_us();
        draw(copy, width, height, last_w, last_h);
        draw_us += now_us() - drawing;
        ++draws;
        ready = drm.back;
    }
    free(copy);
    return NULL;
}

// ---- Audio ----

static snd_pcm_t *pcm;
static long long next_frame_us;
static int16_t mono_buffer[2 * 4096];
static size_t mono_count;

static void open_audio(double rate) {
    char device[128] = "default";
    const char *sink = getenv("MPY_AUDIO_SINK");
    const char *at = sink ? strstr(sink, "device=") : NULL;
    if (at) {
        sscanf(at + 7, "%127s", device);
    }
    if (snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0) < 0
        || snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, (unsigned)(rate + 0.5), 1,
               80000) < 0) {
        fprintf(stderr, "mpy-retro: no sound on %s: a clock paces the game\n", device);
        if (pcm) {
            snd_pcm_close(pcm);
        }
        pcm = NULL;
        return;
    }
    fprintf(stderr, "mpy-retro: sound on %s at %u Hz\n", device, (unsigned)(rate + 0.5));
}

static size_t audio_batch(const int16_t *data, size_t frames) {
    if (!pcm) {
        return frames;
    }
    size_t done = 0;
    while (done < frames && !quit) {
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, data + done * 2, frames - done);
        if (written < 0) {
            if (snd_pcm_recover(pcm, (int)written, 1) < 0) {
                break;
            }
            continue;
        }
        done += (size_t)written;
    }
    return frames;
}

static void audio_sample(int16_t left, int16_t right) {
    mono_buffer[mono_count * 2] = left;
    mono_buffer[mono_count * 2 + 1] = right;
    if (++mono_count == sizeof(mono_buffer) / sizeof(mono_buffer[0]) / 2) {
        audio_batch(mono_buffer, mono_count);
        mono_count = 0;
    }
}

// Without sound, the frame period paces the game.
static void pace(void) {
    if (mono_count) {
        audio_batch(mono_buffer, mono_count);
        mono_count = 0;
    }
    if (pcm) {
        return;
    }
    long long now = now_us(), period = (long long)(1000000 / fps);
    if (!next_frame_us || now - next_frame_us > 100000) {
        next_frame_us = now;
    }
    next_frame_us += period;
    if (next_frame_us > now) {
        usleep((useconds_t)(next_frame_us - now));
    }
}

// ---- Input ----

static struct {
    int fd;
    int number;  // N of /dev/input/eventN
    bool keyboard;
    int axis_min[2], axis_max[2];
} inputs[MAX_INPUTS];
static int input_count;
static uint64_t looked;  // event devices already looked at: ours, or not keyboards nor gamepads
static long long looked_us;
static bool pad[16];          // RetroPad buttons, by RETRO_DEVICE_ID_JOYPAD_*
static bool key_held[16];     // the keyboard's share
static bool button_held[16];  // the gamepads' share
static int hat[2], stick[2];
static bool lent;
static bool key_down[KEY_CNT];  // the keyboards' keys, for cores that read a keyboard
static bool key_tapped[KEY_CNT];  // pressed since the last poll: a key down and up between two polls
                                  // still counts once
static bool core_keyboard;     // the core reads the keyboard: it is no RetroPad, and Esc is the core's

// libretro's keys by evdev code, for RETRO_DEVICE_KEYBOARD.
static const struct {
    unsigned short code;
    unsigned key;
} retro_keys[] = {
    {KEY_A, RETROK_a}, {KEY_B, RETROK_b}, {KEY_C, RETROK_c}, {KEY_D, RETROK_d}, {KEY_E, RETROK_e},
    {KEY_F, RETROK_f}, {KEY_G, RETROK_g}, {KEY_H, RETROK_h}, {KEY_I, RETROK_i}, {KEY_J, RETROK_j},
    {KEY_K, RETROK_k}, {KEY_L, RETROK_l}, {KEY_M, RETROK_m}, {KEY_N, RETROK_n}, {KEY_O, RETROK_o},
    {KEY_P, RETROK_p}, {KEY_Q, RETROK_q}, {KEY_R, RETROK_r}, {KEY_S, RETROK_s}, {KEY_T, RETROK_t},
    {KEY_U, RETROK_u}, {KEY_V, RETROK_v}, {KEY_W, RETROK_w}, {KEY_X, RETROK_x}, {KEY_Y, RETROK_y},
    {KEY_Z, RETROK_z}, {KEY_1, RETROK_1}, {KEY_2, RETROK_2}, {KEY_3, RETROK_3}, {KEY_4, RETROK_4},
    {KEY_5, RETROK_5}, {KEY_6, RETROK_6}, {KEY_7, RETROK_7}, {KEY_8, RETROK_8}, {KEY_9, RETROK_9},
    {KEY_0, RETROK_0}, {KEY_ENTER, RETROK_RETURN}, {KEY_ESC, RETROK_ESCAPE}, {KEY_BACKSPACE, RETROK_BACKSPACE},
    {KEY_TAB, RETROK_TAB}, {KEY_SPACE, RETROK_SPACE}, {KEY_MINUS, RETROK_MINUS}, {KEY_EQUAL, RETROK_EQUALS},
    {KEY_LEFTBRACE, RETROK_LEFTBRACKET}, {KEY_RIGHTBRACE, RETROK_RIGHTBRACKET}, {KEY_BACKSLASH, RETROK_BACKSLASH},
    {KEY_102ND, RETROK_LESS}, {KEY_SEMICOLON, RETROK_SEMICOLON}, {KEY_APOSTROPHE, RETROK_QUOTE},
    {KEY_GRAVE, RETROK_BACKQUOTE}, {KEY_COMMA, RETROK_COMMA}, {KEY_DOT, RETROK_PERIOD}, {KEY_SLASH, RETROK_SLASH},
    {KEY_CAPSLOCK, RETROK_CAPSLOCK}, {KEY_F1, RETROK_F1}, {KEY_F2, RETROK_F2}, {KEY_F3, RETROK_F3},
    {KEY_F4, RETROK_F4}, {KEY_F5, RETROK_F5}, {KEY_F6, RETROK_F6}, {KEY_F7, RETROK_F7}, {KEY_F8, RETROK_F8},
    {KEY_F9, RETROK_F9}, {KEY_F10, RETROK_F10}, {KEY_F11, RETROK_F11}, {KEY_F12, RETROK_F12},
    {KEY_INSERT, RETROK_INSERT}, {KEY_HOME, RETROK_HOME}, {KEY_PAGEUP, RETROK_PAGEUP}, {KEY_DELETE, RETROK_DELETE},
    {KEY_END, RETROK_END}, {KEY_PAGEDOWN, RETROK_PAGEDOWN}, {KEY_RIGHT, RETROK_RIGHT}, {KEY_LEFT, RETROK_LEFT},
    {KEY_DOWN, RETROK_DOWN}, {KEY_UP, RETROK_UP}, {KEY_KP0, RETROK_KP0}, {KEY_KP1, RETROK_KP1},
    {KEY_KP2, RETROK_KP2}, {KEY_KP3, RETROK_KP3}, {KEY_KP4, RETROK_KP4}, {KEY_KP5, RETROK_KP5},
    {KEY_KP6, RETROK_KP6}, {KEY_KP7, RETROK_KP7}, {KEY_KP8, RETROK_KP8}, {KEY_KP9, RETROK_KP9},
    {KEY_KPDOT, RETROK_KP_PERIOD}, {KEY_KPSLASH, RETROK_KP_DIVIDE}, {KEY_KPASTERISK, RETROK_KP_MULTIPLY},
    {KEY_KPMINUS, RETROK_KP_MINUS}, {KEY_KPPLUS, RETROK_KP_PLUS}, {KEY_KPENTER, RETROK_KP_ENTER},
    {KEY_LEFTSHIFT, RETROK_LSHIFT}, {KEY_RIGHTSHIFT, RETROK_RSHIFT}, {KEY_LEFTCTRL, RETROK_LCTRL},
    {KEY_RIGHTCTRL, RETROK_RCTRL}, {KEY_LEFTALT, RETROK_LALT}, {KEY_RIGHTALT, RETROK_RALT},
};

static int keyboard_button(int code) {
    switch (code) {
        case KEY_UP: return RETRO_DEVICE_ID_JOYPAD_UP;
        case KEY_DOWN: return RETRO_DEVICE_ID_JOYPAD_DOWN;
        case KEY_LEFT: return RETRO_DEVICE_ID_JOYPAD_LEFT;
        case KEY_RIGHT: return RETRO_DEVICE_ID_JOYPAD_RIGHT;
        case KEY_X: return RETRO_DEVICE_ID_JOYPAD_A;
        case KEY_Z: return RETRO_DEVICE_ID_JOYPAD_B;
        case KEY_S: return RETRO_DEVICE_ID_JOYPAD_X;
        case KEY_A: return RETRO_DEVICE_ID_JOYPAD_Y;
        case KEY_Q: return RETRO_DEVICE_ID_JOYPAD_L;
        case KEY_W: return RETRO_DEVICE_ID_JOYPAD_R;
        case KEY_ENTER: case KEY_KPENTER: return RETRO_DEVICE_ID_JOYPAD_START;
        case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_BACKSPACE: return RETRO_DEVICE_ID_JOYPAD_SELECT;
        default: return -1;
    }
}

static int gamepad_button(int code) {
    switch (code) {
        case BTN_SOUTH: return RETRO_DEVICE_ID_JOYPAD_B;
        case BTN_EAST: return RETRO_DEVICE_ID_JOYPAD_A;
        case BTN_WEST: return RETRO_DEVICE_ID_JOYPAD_Y;
        case BTN_NORTH: return RETRO_DEVICE_ID_JOYPAD_X;
        case BTN_TL: case BTN_TL2: return RETRO_DEVICE_ID_JOYPAD_L;
        case BTN_TR: case BTN_TR2: return RETRO_DEVICE_ID_JOYPAD_R;
        case BTN_SELECT: return RETRO_DEVICE_ID_JOYPAD_SELECT;
        case BTN_START: return RETRO_DEVICE_ID_JOYPAD_START;
        case BTN_DPAD_UP: return RETRO_DEVICE_ID_JOYPAD_UP;
        case BTN_DPAD_DOWN: return RETRO_DEVICE_ID_JOYPAD_DOWN;
        case BTN_DPAD_LEFT: return RETRO_DEVICE_ID_JOYPAD_LEFT;
        case BTN_DPAD_RIGHT: return RETRO_DEVICE_ID_JOYPAD_RIGHT;
        default: return -1;
    }
}

// The keyboards go to us while the hub sees this file with our pid in it.
static void borrow_keyboards(void) {
    FILE *file = fopen(LENT_FILE ".tmp", "w");
    if (file) {
        fprintf(file, "%d\n", (int)getpid());
        fclose(file);
        lent = rename(LENT_FILE ".tmp", LENT_FILE) == 0;
    }
    if (lent) {
        usleep(300000);  // the hub lets them go within a tenth of a second
    }
}

// The keyboards and gamepads plugged in since the last look: at the start, then every second, so
// one plugged in during the game works (and no keyboard is left to the kernel's console).
static void open_inputs(void) {
    looked_us = now_us();
    DIR *dir = opendir("/dev/input");
    struct dirent *entry;
    uint64_t present = 0;
    while (dir && (entry = readdir(dir))) {
        char *end;
        if (strncmp(entry->d_name, "event", 5) || !entry->d_name[5]) {
            continue;
        }
        long number = strtol(entry->d_name + 5, &end, 10);
        if (*end || number < 0 || number >= 64) {
            continue;
        }
        present |= 1ULL << number;
        if ((looked & (1ULL << number)) || input_count >= MAX_INPUTS) {
            continue;
        }
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        looked |= 1ULL << number;
        unsigned long keys[KEY_CNT / BITS_PER_LONG + 1] = {0};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
        bool keyboard = TEST_BIT(KEY_A, keys) && TEST_BIT(KEY_Z, keys) && TEST_BIT(KEY_ENTER, keys);
        bool gamepad = TEST_BIT(BTN_SOUTH, keys) || TEST_BIT(BTN_JOYSTICK, keys);
        if (!keyboard && !gamepad) {
            close(fd);
            continue;
        }
        if (keyboard) {
            // Ours alone: neither the hub nor the kernel's console sees the keys. The hub may take
            // a moment to let go.
            for (int tries = 0; tries < 20 && ioctl(fd, EVIOCGRAB, 1) < 0; ++tries) {
                usleep(50000);
            }
        }
        inputs[input_count].fd = fd;
        inputs[input_count].number = (int)number;
        inputs[input_count].keyboard = keyboard;
        for (int axis = 0; axis < 2; ++axis) {
            struct input_absinfo info = {0};
            ioctl(fd, EVIOCGABS(axis == 0 ? ABS_X : ABS_Y), &info);
            inputs[input_count].axis_min[axis] = info.minimum;
            inputs[input_count].axis_max[axis] = info.maximum;
        }
        char name[128] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        fprintf(stderr, "mpy-retro: %s %s\n", keyboard ? "keyboard" : "gamepad", name);
        ++input_count;
    }
    if (dir) {
        closedir(dir);
    }
    looked &= present;  // a number that went away is looked at again when it comes back
}

static void close_inputs(void) {
    for (int i = 0; i < input_count; ++i) {
        close(inputs[i].fd);  // the grab goes with it
    }
    if (lent) {
        unlink(LENT_FILE);  // the hub takes the keyboards back
    }
}

static void forget_input(int i) {
    close(inputs[i].fd);  // unplugged
    looked &= ~(1ULL << inputs[i].number);
    inputs[i] = inputs[--input_count];
    memset(key_held, 0, sizeof(key_held));
    memset(button_held, 0, sizeof(button_held));
    hat[0] = hat[1] = stick[0] = stick[1] = 0;
}

static void input_poll(void) {
    memset(key_tapped, 0, sizeof(key_tapped));
    if (now_us() - looked_us > 1000000) {
        open_inputs();
    }
    for (int i = 0; i < input_count; ++i) {
        struct input_event events[32];
        ssize_t got;
        while ((got = read(inputs[i].fd, events, sizeof(events))) > 0 || (got < 0 && errno == EINTR)) {
            if (got < 0) {
                continue;
            }
            for (size_t e = 0; e < (size_t)got / sizeof(events[0]); ++e) {
                const struct input_event *ev = &events[e];
                if (ev->type == EV_KEY && ev->value != 2) {
                    bool down = ev->value != 0;
                    if (inputs[i].keyboard && ev->code < KEY_CNT) {
                        key_down[ev->code] = down;
                        key_tapped[ev->code] |= down;
                    }
                    if (down && ((ev->code == KEY_ESC && !core_keyboard) || ev->code == KEY_LEFTMETA
                            || ev->code == KEY_RIGHTMETA || ev->code == BTN_MODE)) {
                        quit = 1;
                    }
                    int button = inputs[i].keyboard ? keyboard_button(ev->code) : gamepad_button(ev->code);
                    if (button >= 0) {
                        (inputs[i].keyboard ? key_held : button_held)[button] = down;
                    }
                } else if (ev->type == EV_ABS) {
                    if (ev->code == ABS_HAT0X || ev->code == ABS_HAT0Y) {
                        hat[ev->code == ABS_HAT0Y] = ev->value;
                    } else if (ev->code == ABS_X || ev->code == ABS_Y) {
                        int axis = ev->code == ABS_Y;
                        int low = inputs[i].axis_min[axis], high = inputs[i].axis_max[axis];
                        int quarter = (high - low) / 4;
                        stick[axis] = ev->value < low + quarter ? -1 : ev->value > high - quarter ? 1 : 0;
                    }
                }
            }
        }
        if (got < 0 && errno == ENODEV) {
            forget_input(i--);
        }
    }
    for (int b = 0; b < 16; ++b) {
        pad[b] = (key_held[b] && !core_keyboard) || button_held[b];
    }
    pad[RETRO_DEVICE_ID_JOYPAD_LEFT] |= hat[0] < 0 || stick[0] < 0;
    pad[RETRO_DEVICE_ID_JOYPAD_RIGHT] |= hat[0] > 0 || stick[0] > 0;
    pad[RETRO_DEVICE_ID_JOYPAD_UP] |= hat[1] < 0 || stick[1] < 0;
    pad[RETRO_DEVICE_ID_JOYPAD_DOWN] |= hat[1] > 0 || stick[1] > 0;
    if (pad[RETRO_DEVICE_ID_JOYPAD_SELECT] && pad[RETRO_DEVICE_ID_JOYPAD_START]) {
        quit = 1;
    }
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (device == RETRO_DEVICE_KEYBOARD) {
        core_keyboard = true;
        for (size_t i = 0; i < sizeof(retro_keys) / sizeof(retro_keys[0]); ++i) {
            if (retro_keys[i].key == id) {
                return key_down[retro_keys[i].code] || key_tapped[retro_keys[i].code];
            }
        }
        return 0;
    }
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) {
        return 0;
    }
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
        int16_t mask = 0;
        for (int b = 0; b < 16; ++b) {
            mask |= pad[b] ? (int16_t)(1 << b) : 0;
        }
        return mask;
    }
    return id < 16 && pad[id];
}

// ---- The game ----

static void *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    void *data = length > 0 ? malloc((size_t)length) : NULL;
    if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(file);
    *size = data ? (size_t)length : 0;
    return data;
}

static void save_ram(const char *srm, bool load) {
    void *ram = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!ram || !size) {
        return;
    }
    FILE *file = fopen(srm, load ? "rb" : "wb");
    if (file) {
        size_t moved = load ? fread(ram, 1, size, file) : fwrite(ram, 1, size, file);
        (void)moved;
        fclose(file);
    }
}

static void on_signal(int signal_number) {
    (void)signal_number;
    quit = 1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mpy-retro CORE.so GAME [--frames N]\n");
        return 2;
    }
    unsigned long max_frames = 0;
    for (int i = 3; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "--frames")) {
            max_frames = strtoul(argv[++i], NULL, 10);
        }
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    const char *game_path = argv[2];
    const char *slash = strrchr(game_path, '/');
    snprintf(save_dir, sizeof(save_dir), "%.*s", slash ? (int)(slash - game_path) : 1, slash ? game_path : ".");
    if (getenv("MPY_RETRO_SYSTEM")) {
        snprintf(system_dir, sizeof(system_dir), "%s", getenv("MPY_RETRO_SYSTEM"));
    }

    load_core(argv[1]);
    core.set_environment(environment);
    core.init();
    core.set_video_refresh(video_refresh);
    core.set_audio_sample(audio_sample);
    core.set_audio_sample_batch(audio_batch);
    core.set_input_poll(input_poll);
    core.set_input_state(input_state);

    struct retro_system_info system = {0};
    core.get_system_info(&system);
    struct retro_game_info game = {.path = game_path};
    if (!system.need_fullpath) {
        game.data = read_file(game_path, &game.size);
        if (!game.data) {
            die("cannot read %s", game_path);
        }
    }
    if (!core.load_game(&game)) {
        die("%s does not load %s", system.library_name, game_path);
    }
    struct retro_system_av_info av = {0};
    core.get_system_av_info(&av);
    if (av.geometry.aspect_ratio > 0) {
        aspect = av.geometry.aspect_ratio;
    }
    fps = av.timing.fps > 0 ? av.timing.fps : 60;
    fprintf(stderr, "mpy-retro: %s %s, %.2f fps\n", system.library_name, system.library_version, fps);

    char srm[600];
    snprintf(srm, sizeof(srm), "%.*s.srm",
        (int)(strrchr(game_path, '.') && strrchr(game_path, '.') > (slash ? slash : game_path)
                  ? strrchr(game_path, '.') - game_path : (long)strlen(game_path)),
        game_path);
    save_ram(srm, true);

    open_screen();
    open_audio(av.timing.sample_rate);
    borrow_keyboards();
    open_inputs();
    pthread_t video;
    pthread_create(&video, NULL, video_thread, NULL);

    long long started = now_us();
    unsigned long frames = 0;
    while (!quit && (!max_frames || frames < max_frames)) {
        core.run();
        pace();
        ++frames;
    }
    double seconds = (now_us() - started) / 1e6;
    fprintf(stderr, "mpy-retro: %lu frames in %.1f s, %.2f fps, %.2f shown a second\n", frames, seconds,
        frames / seconds, frames_shown / seconds);
    fprintf(stderr, "mpy-retro: drawing %.2f ms a frame\n", draws ? draw_us / 1000.0 / draws : 0.0);

    quit = 1;
    pthread_cond_signal(&frame.ready);
    pthread_join(video, NULL);
    save_ram(srm, false);
    close_inputs();
    if (pcm) {
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }
    core.unload_game();
    core.deinit();
    close_screen();
    return 0;
}
