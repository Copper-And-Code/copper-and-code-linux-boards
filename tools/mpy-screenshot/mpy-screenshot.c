// SPDX-License-Identifier: MIT
// What the panel shows now, as a PNG, without disturbing the program that draws it.
//
//   mpy-screenshot [-r 0|90|180|270] [-o file.png]      (default: stdout)
//
// It reads the framebuffer the display controller scans out (DRM: connected connector, its CRTC,
// the CRTC's framebuffer), maps it and writes it as an 8-bit RGB PNG. -r turns the picture as
// display_driver turns the screen (MPY_DISPLAY_ROTATION), so it looks as the user sees it. Reading
// another program's framebuffer needs root (CAP_SYS_ADMIN). RGB565 and XRGB8888 are handled.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <zlib.h>

static void fail(const char *what) {
    fprintf(stderr, "mpy-screenshot: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void put32(unsigned char *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void chunk(FILE *out, const char *type, const unsigned char *data, uint32_t length) {
    unsigned char header[8];
    put32(header, length);
    memcpy(header + 4, type, 4);
    fwrite(header, 1, 8, out);
    if (length) {
        fwrite(data, 1, length, out);
    }
    uint32_t crc = crc32(0, (const unsigned char *)type, 4);
    crc = crc32(crc, data, length);
    unsigned char tail[4];
    put32(tail, crc);
    fwrite(tail, 1, 4, out);
}

int main(int argc, char **argv) {
    int rotation = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            rotation = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            path = argv[++i];
        } else {
            fprintf(stderr, "usage: mpy-screenshot [-r 0|90|180|270] [-o file.png]\n");
            return 2;
        }
    }
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
        fprintf(stderr, "mpy-screenshot: rotation must be 0, 90, 180 or 270\n");
        return 2;
    }

    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fail("/dev/dri/card0");
    }
    drmModeRes *resources = drmModeGetResources(fd);
    if (!resources) {
        fail("drmModeGetResources");
    }
    uint32_t fb_id = 0;
    for (int i = 0; i < resources->count_crtcs && !fb_id; ++i) {
        drmModeCrtc *crtc = drmModeGetCrtc(fd, resources->crtcs[i]);
        if (crtc && crtc->buffer_id && crtc->mode_valid) {
            fb_id = crtc->buffer_id;
        }
        drmModeFreeCrtc(crtc);
    }
    if (!fb_id) {
        fprintf(stderr, "mpy-screenshot: nothing on the screen (no active CRTC)\n");
        return 1;
    }
    drmModeFB *fb = drmModeGetFB(fd, fb_id);
    if (!fb || !fb->handle) {
        errno = fb ? EPERM : errno;
        fail("drmModeGetFB (root is needed to read another program's framebuffer)");
    }
    if (fb->bpp != 16 && fb->bpp != 32) {
        fprintf(stderr, "mpy-screenshot: %u bits per pixel are not handled\n", fb->bpp);
        return 1;
    }
    struct drm_mode_map_dumb map = {.handle = fb->handle};
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
        fail("DRM_IOCTL_MODE_MAP_DUMB");
    }
    size_t size = (size_t)fb->pitch * fb->height;
    const unsigned char *pixels = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, map.offset);
    if (pixels == MAP_FAILED) {
        fail("mmap");
    }

    uint32_t panel_w = fb->width, panel_h = fb->height;
    uint32_t out_w = (rotation == 90 || rotation == 270) ? panel_h : panel_w;
    uint32_t out_h = (rotation == 90 || rotation == 270) ? panel_w : panel_h;
    size_t row = 1 + (size_t)out_w * 3;  // a filter byte, then RGB
    unsigned char *image = malloc(row * out_h);
    if (!image) {
        fail("malloc");
    }
    for (uint32_t ly = 0; ly < out_h; ++ly) {
        unsigned char *line = image + (size_t)ly * row;
        line[0] = 0;
        for (uint32_t lx = 0; lx < out_w; ++lx) {
            // The same mapping as linuxfb.blit_rotated(): a logical pixel and where it is on the panel.
            uint32_t px, py;
            if (rotation == 90) {
                px = ly;
                py = panel_h - 1 - lx;
            } else if (rotation == 180) {
                px = panel_w - 1 - lx;
                py = panel_h - 1 - ly;
            } else if (rotation == 270) {
                px = panel_w - 1 - ly;
                py = lx;
            } else {
                px = lx;
                py = ly;
            }
            unsigned char *rgb = line + 1 + (size_t)lx * 3;
            const unsigned char *p = pixels + (size_t)py * fb->pitch + (size_t)px * (fb->bpp / 8);
            if (fb->bpp == 16) {
                uint16_t v = p[0] | (p[1] << 8);
                rgb[0] = ((v >> 11) & 0x1F) * 255 / 31;
                rgb[1] = ((v >> 5) & 0x3F) * 255 / 63;
                rgb[2] = (v & 0x1F) * 255 / 31;
            } else {
                rgb[0] = p[2];
                rgb[1] = p[1];
                rgb[2] = p[0];
            }
        }
    }

    uLongf packed_size = compressBound(row * out_h);
    unsigned char *packed = malloc(packed_size);
    if (!packed || compress2(packed, &packed_size, image, row * out_h, 6) != Z_OK) {
        fprintf(stderr, "mpy-screenshot: compression failed\n");
        return 1;
    }
    FILE *out = path ? fopen(path, "wb") : stdout;
    if (!out) {
        fail(path);
    }
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(signature, 1, 8, out);
    unsigned char ihdr[13];
    put32(ihdr, out_w);
    put32(ihdr + 4, out_h);
    ihdr[8] = 8;  // bits per channel
    ihdr[9] = 2;  // RGB
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(out, "IHDR", ihdr, sizeof(ihdr));
    chunk(out, "IDAT", packed, packed_size);
    chunk(out, "IEND", NULL, 0);
    if (fflush(out) || (path && fclose(out))) {
        fail("write");
    }
    return 0;
}
