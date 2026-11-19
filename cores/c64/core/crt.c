// CRT cartridge images: see crt.h.

#include "crt.h"

#include <string.h>

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

const char *crt_parse(const uint8_t *d, size_t n, crt_t *out) {
    memset(out, 0, sizeof(*out));
    if (n < 0x40 || memcmp(d, "C64 CARTRIDGE   ", 16) != 0) {
        return "not a CRT image";
    }
    uint32_t hlen = be32(d + 0x10);
    out->hw_type = be16(d + 0x16);
    // Header bytes: 0 = line low (active).
    out->exrom = d[0x18] == 0;
    out->game = d[0x19] == 0;
    memcpy(out->name, d + 0x20, 32);
    if (out->hw_type != 0) {
        return "cartridge type not supported (only normal, type 0)";
    }
    if (!out->exrom) {
        return "Ultimax / no EXROM: not supported";
    }
    for (size_t off = hlen; off + 0x10 <= n;) {
        if (memcmp(d + off, "CHIP", 4) != 0) {
            return "bad CHIP packet";
        }
        uint32_t plen = be32(d + off + 4);
        uint16_t bank = be16(d + off + 0x0a), addr = be16(d + off + 0x0c), size = be16(d + off + 0x0e);
        if (plen < 0x10 || off + 0x10 + size > n) {
            return "truncated CHIP packet";
        }
        const uint8_t *data = d + off + 0x10;
        if (bank != 0) {
            return "banked cartridge: not supported";
        }
        if (addr == 0x8000 && size == 0x2000) {
            out->roml = data;
        } else if (addr == 0x8000 && size == 0x4000) {
            out->roml = data;
            out->romh = data + 0x2000;
        } else if (addr == 0xa000 && size == 0x2000) {
            out->romh = data;
        } else {
            return "unsupported CHIP address/size";
        }
        off += plen;
    }
    if (!out->roml) {
        return "no ROM at $8000";
    }
    if (out->game && !out->romh) {
        return "16 KB cartridge without ROMH";
    }
    return NULL;
}
