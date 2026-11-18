// SPDX-License-Identifier: MIT
// RGB565 scan-out through a Linux DRM dumb buffer.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "py/runtime.h"

static struct {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t handle;
    uint32_t fb_id;
    uint8_t *map;
    uint64_t map_size;
    drmModeRes *resources;
    drmModeConnector *connector;
} fb = {.fd = -1, .map = MAP_FAILED};

static void linuxfb_cleanup(void) {
    if (fb.map != MAP_FAILED) {
        munmap(fb.map, fb.map_size);
        fb.map = MAP_FAILED;
    }
    if (fb.fd >= 0) {
        if (fb.fb_id) {
            drmModeRmFB(fb.fd, fb.fb_id);
            fb.fb_id = 0;
        }
        if (fb.handle) {
            drmModeDestroyDumbBuffer(fb.fd, fb.handle);
            fb.handle = 0;
        }
    }
    if (fb.connector) {
        drmModeFreeConnector(fb.connector);
        fb.connector = NULL;
    }
    if (fb.resources) {
        drmModeFreeResources(fb.resources);
        fb.resources = NULL;
    }
    if (fb.fd >= 0) {
        drmDropMaster(fb.fd);
        close(fb.fd);
        fb.fd = -1;
    }
}

static void linuxfb_raise(int result) {
    int error = result == -1 ? errno : -result;
    if (error <= 0) {
        error = EIO;
    }
    linuxfb_cleanup();
    mp_raise_OSError(error);
}

static void linuxfb_check_open(void) {
    if (fb.fd < 0 || fb.map == MAP_FAILED) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("linuxfb: open() first"));
    }
}

static mp_obj_t linuxfb_open(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_path };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_path, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    linuxfb_cleanup();
    const char *path = args[ARG_path].u_obj == mp_const_none ? "/dev/dri/card0" : mp_obj_str_get_str(args[ARG_path].u_obj);
    fb.fd = open(path, O_RDWR | O_CLOEXEC);
    if (fb.fd < 0) {
        linuxfb_raise(-1);
    }
    int result = drmSetMaster(fb.fd);
    if (result) {
        linuxfb_raise(result);
    }
    fb.resources = drmModeGetResources(fb.fd);
    if (!fb.resources) {
        linuxfb_raise(-1);
    }
    for (int i = 0; i < fb.resources->count_connectors; ++i) {
        drmModeConnector *connector = drmModeGetConnector(fb.fd, fb.resources->connectors[i]);
        if (connector && connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
            fb.connector = connector;
            break;
        }
        if (connector) {
            drmModeFreeConnector(connector);
        }
    }
    if (!fb.connector) {
        linuxfb_raise(-ENODEV);
    }
    drmModeModeInfo mode = fb.connector->modes[0];
    uint32_t crtc_id = 0;
    if (fb.connector->encoder_id) {
        drmModeEncoder *encoder = drmModeGetEncoder(fb.fd, fb.connector->encoder_id);
        if (encoder) {
            crtc_id = encoder->crtc_id;
            drmModeFreeEncoder(encoder);
        }
    }
    if (!crtc_id && fb.resources->count_crtcs > 0) {
        crtc_id = fb.resources->crtcs[0];
    }
    if (!crtc_id) {
        linuxfb_raise(-ENODEV);
    }

    fb.width = mode.hdisplay;
    fb.height = mode.vdisplay;
    result = drmModeCreateDumbBuffer(fb.fd, fb.width, fb.height, 16, 0, &fb.handle, &fb.pitch, &fb.map_size);
    if (result) {
        linuxfb_raise(result);
    }
    result = drmModeAddFB(fb.fd, fb.width, fb.height, 16, 16, fb.pitch, fb.handle, &fb.fb_id);
    if (result) {
        linuxfb_raise(result);
    }
    uint64_t offset = 0;
    result = drmModeMapDumbBuffer(fb.fd, fb.handle, &offset);
    if (result) {
        linuxfb_raise(result);
    }
    fb.map = mmap(NULL, fb.map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, offset);
    if (fb.map == MAP_FAILED) {
        linuxfb_raise(-1);
    }
    memset(fb.map, 0, fb.map_size);
    uint32_t connector_id = fb.connector->connector_id;
    result = drmModeSetCrtc(fb.fd, crtc_id, fb.fb_id, 0, 0, &connector_id, 1, &mode);
    if (result) {
        linuxfb_raise(result);
    }
    mp_obj_t dimensions[] = {
        mp_obj_new_int_from_uint(fb.width),
        mp_obj_new_int_from_uint(fb.height),
        MP_OBJ_NEW_SMALL_INT(16),
        mp_obj_new_int_from_uint(fb.pitch),
    };
    return mp_obj_new_tuple(MP_ARRAY_SIZE(dimensions), dimensions);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(linuxfb_open_obj, 0, linuxfb_open);

static mp_obj_t linuxfb_blit(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_x, ARG_y, ARG_width, ARG_height, ARG_buffer, ARG_stride };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_buffer, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_stride, MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    linuxfb_check_open();

    mp_int_t x = args[ARG_x].u_int;
    mp_int_t y = args[ARG_y].u_int;
    mp_int_t width = args[ARG_width].u_int;
    mp_int_t height = args[ARG_height].u_int;
    // Each bound is checked against the panel before any product, so nothing below can overflow.
    if (x < 0 || y < 0 || width <= 0 || height <= 0 || width > (mp_int_t)fb.width || height > (mp_int_t)fb.height
        || x > (mp_int_t)fb.width - width || y > (mp_int_t)fb.height - height) {
        mp_raise_ValueError(MP_ERROR_TEXT("linuxfb: invalid rectangle or stride"));
    }
    mp_int_t stride = args[ARG_stride].u_int ? args[ARG_stride].u_int : width * 2;
    if (stride < width * 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("linuxfb: invalid rectangle or stride"));
    }
    mp_buffer_info_t buffer;
    mp_get_buffer_raise(args[ARG_buffer].u_obj, &buffer, MP_BUFFER_READ);
    if ((uint64_t)buffer.len < (uint64_t)(height - 1) * (uint64_t)stride + (uint64_t)width * 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("linuxfb: buffer too small"));
    }
    const uint8_t *source = buffer.buf;
    for (mp_int_t row = 0; row < height; ++row) {
        memcpy(fb.map + (size_t)(y + row) * fb.pitch + (size_t)x * 2, source + (size_t)row * stride, (size_t)width * 2);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(linuxfb_blit_obj, 5, linuxfb_blit);

// blit_rotated(x, y, width, height, buffer, rotation): copy a rectangle LVGL rendered in the rotated
// (logical) orientation to the panel, turning it on the way, as lv_display_rotate_area() describes.
// rotation is 90, 180 or 270; the buffer holds width x height RGB565 pixels with no padding. A
// logical pixel (lx, ly) goes to the panel at (ly, H - 1 - lx) for 90, (W - 1 - lx, H - 1 - ly) for
// 180 and (W - 1 - ly, lx) for 270, where W x H is the panel.
static mp_obj_t linuxfb_blit_rotated(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    linuxfb_check_open();
    mp_int_t x = mp_obj_get_int(args[0]);
    mp_int_t y = mp_obj_get_int(args[1]);
    mp_int_t width = mp_obj_get_int(args[2]);
    mp_int_t height = mp_obj_get_int(args[3]);
    mp_int_t rotation = mp_obj_get_int(args[5]);
    if (rotation != 90 && rotation != 180 && rotation != 270) {
        mp_raise_ValueError(MP_ERROR_TEXT("linuxfb: rotation must be 90, 180 or 270"));
    }
    // The logical screen: turned a quarter, the panel's width and height swap.
    mp_int_t logical_w = rotation == 180 ? (mp_int_t)fb.width : (mp_int_t)fb.height;
    mp_int_t logical_h = rotation == 180 ? (mp_int_t)fb.height : (mp_int_t)fb.width;
    if (x < 0 || y < 0 || width <= 0 || height <= 0 || width > logical_w || height > logical_h
        || x > logical_w - width || y > logical_h - height) {
        mp_raise_ValueError(MP_ERROR_TEXT("linuxfb: invalid rectangle"));
    }
    mp_buffer_info_t buffer;
    mp_get_buffer_raise(args[4], &buffer, MP_BUFFER_READ);
    if ((uint64_t)buffer.len < (uint64_t)width * (uint64_t)height * 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("linuxfb: buffer too small"));
    }
    const uint16_t *source = buffer.buf;
    size_t pitch = fb.pitch / 2;  // in pixels
    uint16_t *panel = (uint16_t *)fb.map;
    mp_int_t panel_w = fb.width, panel_h = fb.height;
    for (mp_int_t row = 0; row < height; ++row) {
        mp_int_t ly = y + row;
        const uint16_t *line = source + (size_t)row * width;
        for (mp_int_t col = 0; col < width; ++col) {
            mp_int_t lx = x + col;
            mp_int_t px, py;
            if (rotation == 90) {
                px = ly;
                py = panel_h - 1 - lx;
            } else if (rotation == 180) {
                px = panel_w - 1 - lx;
                py = panel_h - 1 - ly;
            } else {
                px = panel_w - 1 - ly;
                py = lx;
            }
            panel[(size_t)py * pitch + (size_t)px] = line[col];
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(linuxfb_blit_rotated_obj, 6, 6, linuxfb_blit_rotated);

static mp_obj_t linuxfb_close(void) {
    linuxfb_cleanup();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(linuxfb_close_obj, linuxfb_close);

static const mp_rom_map_elem_t linuxfb_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_linuxfb) },
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&linuxfb_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_blit), MP_ROM_PTR(&linuxfb_blit_obj) },
    { MP_ROM_QSTR(MP_QSTR_blit_rotated), MP_ROM_PTR(&linuxfb_blit_rotated_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&linuxfb_close_obj) },
};
static MP_DEFINE_CONST_DICT(linuxfb_module_globals, linuxfb_module_globals_table);

const mp_obj_module_t linuxfb_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&linuxfb_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_linuxfb, linuxfb_user_cmodule);
