// VIC-II renderer (core 1), after Christian Bauer's "The MOS 6567/6569 video
// controller (VIC-II) and its application in the Commodore 64" (vic-ii.txt).
//
// Fed with the ring entries of core 0 (register writes with their cycle, and
// one marker per raster line). It keeps its own copy of the registers and
// applies each write at the pixel that corresponds to its cycle, so mid-line
// changes (raster bars, split colours, border tricks) land where they should.
// It reads C64 RAM, colour RAM and the character ROM directly.
//
// Per line: VC/VCBASE/RC and display/idle state, badlines, all graphics modes
// (text, multicolour text, ECM, bitmap, multicolour bitmap, the invalid ones),
// XSCROLL, 38/40 columns, 24/25 rows, the main and vertical border flip-flops,
// 8 sprites (expansion, multicolour, priority, collisions, reported to core 0).
//
// Picture: raster lines VICR_FIRST_LINE.. (272 lines) x 384 pixels (sprite X
// -8..375), written as 4-bit colour indices; vicr_expand() turns a line into
// 480 RGB565 pixels (1.25x).
//
// Sprites: MC/MCBASE as on the chip (DMA ends when MCBASE is 63), sprite crunch
// ($D017 cleared in cycle 15).
// Not modelled: mid-line YSCROLL changes (FLD/VSP/linecrunch), the grey dots,
// light pen.

#ifndef C64_VIC_RENDER_H
#define C64_VIC_RENDER_H

#include <stdint.h>

#include "c64.h"

#define VICR_FIRST_LINE (15)
#define VICR_LINES (272)
#define VICR_W (384)                // C64 pixels per line
#define VICR_PACKED (VICR_W / 2)    // bytes per line: 4-bit colour indices, first pixel in the low nibble
#define VICR_OUT_W (480)            // RGB565 pixels per line after expansion (1.25x)

void vicr_init(void);
// One ring entry. Returns the raster line just completed, or -1.
// out_for_line(line) gives the buffer (VICR_PACKED bytes) for a visible line.
int vicr_feed(const c64_vic_write_t *e, uint8_t *(*out_for_line)(unsigned line));
// A packed line to VICR_OUT_W RGB565 pixels (each group of 4 C64 pixels
// becomes 5, the first doubled); out 4-byte aligned.
void vicr_expand(const uint8_t *packed, uint16_t *out);
// STM32 extension: palette conversion before panel-specific scaling.
void vicr_expand_native(const uint8_t *packed, uint16_t *out);
uint16_t vicr_border_rgb565(void);

// Optional profiling: if vicr_clock is set, vicr_prof[] accumulates its ticks
// per stage of the visible lines (0: events and background, 1: sprites,
// 2: packing to 4 bits) and [3] counts the lines.
extern uint32_t (*vicr_clock)(void);
extern uint32_t vicr_prof[6];   // [4] render_span (in [0]), [5] gen_gfx (in [4])

#endif
