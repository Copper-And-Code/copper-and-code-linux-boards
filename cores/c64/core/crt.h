// CRT cartridge images (the VICE format): header, then CHIP packets.
// Supported: hardware type 0 ("normal cartridge") with 8 KB at $8000, or
// 16 KB at $8000 (one 16 KB chip or two 8 KB chips at $8000 and $A000).

#ifndef C64_CRT_H
#define C64_CRT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[33];
    uint16_t hw_type;
    bool game, exrom;               // true: line pulled low
    const uint8_t *roml, *romh;     // 8 KB each, inside the parsed image
} crt_t;

// Parse an image in memory (the chip data are referenced, not copied).
// Returns NULL on success, else a short reason.
const char *crt_parse(const uint8_t *d, size_t n, crt_t *out);

#endif
