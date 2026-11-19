// C64 (PAL) machine on core 0: 6510, memory map, CIA x2, VIC-II timing model,
// SID registers. Portable (host tests and pico-sdk).
//
// The machine runs one raster line (63 cycles) at a time. The VIC-II is only a
// timing model here (raster counter, raster IRQ, badline cycle stealing,
// registers); drawing is done elsewhere (core 1) from the register writes,
// which carry the cycle they happened at.
//
// Not modelled yet (see docs): mid-line badline changes (FLD/VSP), CIA TOD clock and serial port, the 2-cycle CIA timer start delay,
// light pen, cartridges. The SID is in sid.c (sid_init() sets its output).

#ifndef C64_C64_H
#define C64_C64_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu6510.h"

#define C64_LINES (312)
#define C64_CYCLES_PER_LINE (63)
#define C64_CYCLES_PER_FRAME (C64_LINES * C64_CYCLES_PER_LINE)

// IRQ sources (bits of cpu.irq).
#define C64_IRQ_CIA1 (1u << 0)
#define C64_IRQ_VIC (1u << 1)

typedef struct {
    uint8_t pra, prb, ddra, ddrb;   // output registers and directions
    uint8_t cra, crb;
    uint8_t icr_mask, icr_data;     // icr_data bit 7 = IRQ asserted
    uint16_t latch_a, latch_b;
    uint16_t cnt_a, cnt_b;          // counter value at cycle base_a / base_b
    uint32_t base_a, base_b;
    uint32_t next_a, next_b;        // cycle of the next underflow (if running, phi2)
    uint8_t tod[4], sdr;            // stored only
    bool is_cia2;                   // drives NMI instead of IRQ
} c64_cia_t;

// Memory barrier between the cores.
#ifndef C64_DMB
#define C64_DMB() __sync_synchronize()
#endif

// One VIC-II register write, for the renderer on core 1. reg C64_RING_LINE:
// raster line `cycle` is complete (all its writes and RAM changes are before).
typedef struct {
    uint32_t cycle;     // write: cycle of the write; line marker: cycle 0 of the line
    uint8_t reg, value;
    uint16_t line;      // line marker: raster line
} c64_vic_write_t;

#define C64_VIC_RING (256) // power of two
#define C64_RING_LINE (0xff)
#define C64_RING_BANK (0x40)    // VIC bank changed (CIA2): value = bank base >> 14

// RAM writes of the line core 0 is running, with the values they replaced:
// core 1, drawing the line before, reads RAM as it was at that line's end;
// with the cycles, it reads RAM as it was at a VIC access of its own line.
// At most one CPU write per cycle: a line (with overshoot) fits in 80.
#define C64_AHEAD_MAX (80)
#define C64_AHEAD_FILTER_WORDS (32)
typedef struct {
    volatile uint32_t line;     // raster line count (c64m.lines) of these writes
    volatile uint32_t n;
    volatile uint32_t filter[C64_AHEAD_FILTER_WORDS];   // bit c64_ahead_hash(addr): an entry may exist
    uint32_t addr[C64_AHEAD_MAX];   // RAM address; 0x10000 + n: colour RAM
    uint8_t old[C64_AHEAD_MAX];
    uint8_t cycle[C64_AHEAD_MAX];   // cycle of the write in the line (255: 255 or later)
} c64_ahead_t;

// 1024-bit filter: with ~60 writes in a line about 6 % of the renderer's reads
// take the slow path for nothing (a 64-bit one: about 60 %).
static inline unsigned c64_ahead_hash(uint32_t a) {
    return (a ^ (a >> 10) ^ (a >> 7)) & 1023;
}

typedef struct {
    cpu6510_t cpu;

    uint8_t ram[65536];
    uint8_t color_ram[1024];        // low nibble used
    uint8_t basic[0x2000], kernal[0x2000], chargen[0x1000];

    // Memory map, rebuilt when the CPU port changes: 16 pages of 4 KB. A NULL
    // read page is I/O ($D000 with I/O visible).
    const uint8_t *rd[16];
    bool io_visible;
    // Cartridge (expansion port): ROML ($8000) and ROMH ($A000) images of
    // 8 KB, and the GAME / EXROM lines (true: pulled low by the cartridge).
    const uint8_t *cart_roml, *cart_romh;
    bool cart_game, cart_exrom;
    uint8_t port_ddr, port_data;

    // VIC-II
    uint8_t vic[64];
    uint8_t vic_irq;                // $D019 latched bits 0..3
    uint16_t raster_cmp;
    unsigned line;                  // current raster line
    uint32_t line_start;            // cycle of cycle 0 of the current line
    uint32_t frame;                 // frames completed
    bool den_seen;                  // DEN was set in line $30 (badlines enabled)
    uint32_t ba_start, ba_len;      // CPU reads in [ba_start, ba_start + ba_len) stall
    uint32_t stolen;                // cycles stolen by badlines (statistics)
    volatile uint32_t ring_head;    // written by core 0
    volatile uint32_t ring_tail;    // written by the consumer
    bool ring_enabled;
    c64_vic_write_t ring[C64_VIC_RING];
    uint32_t lines;                 // raster lines run since reset
    c64_ahead_t ahead[2];           // by line parity (only with ring_enabled)

    c64_cia_t cia1, cia2;
    uint32_t next_event;            // earliest CIA underflow

    // Sprite DMA model (for the cycles it steals), as in the renderer: MC,
    // MCBASE, expansion flip-flops; state before the cycle-55 step of the
    // current line (redone if a sprite register changes before cycle 55).
    uint8_t sp_dma, sp_ff;
    uint8_t sp_mc[8], sp_mcbase[8];
    uint8_t sp55_dma, sp55_ff;
    uint8_t sp55_mc[8], sp55_mcbase[8];
    bool sp55_valid;
    // CPU read stall windows, absolute cycles: badline of this line, sprite DMA
    // of the previous line (spilling into this one) and of this line.
    uint32_t bad_start, bad_end;
    uint32_t spr_prev_start[8], spr_prev_end[8], spr_start[8], spr_end[8];
    unsigned spr_prev_n, spr_n;
    uint32_t win_start[24], win_end[24];
    unsigned win_n;
    bool win_dirty;                 // windows changed inside a CPU run

    // Collisions found by the renderer (core 1): $D01E, $D01F, and $D019 IRQ
    // bits to latch; accessed atomically by both cores.
    uint8_t coll_ss, coll_sb, coll_irq;


    // Keyboard: kbd[col] bit r set = key at column col (CIA1 port A bit),
    // row r (port B bit) pressed. joy1/joy2: bits 0-4 up, down, left, right,
    // fire, set = active.
    uint8_t kbd[8];
    uint8_t joy1, joy2;
} c64_t;

extern c64_t c64m;

// ROM images are copied (8 KB, 8 KB, 4 KB).
void c64_init(const uint8_t *basic, const uint8_t *kernal, const uint8_t *chargen);
void c64_reset(void);
// Power off and on: RAM back to the power-on pattern, then reset (cartridges
// are plugged in with the machine off).
void c64_power_cycle(void);
// Plug a cartridge in (images are not copied; NULL for a missing chip) or
// pull it out (all NULL / false); takes effect at once, normally followed by
// c64_power_cycle(). Normal cartridges: 8 KB (EXROM) and 16 KB (EXROM + GAME).
void c64_cart(const uint8_t *roml, const uint8_t *romh, bool game, bool exrom);
void c64_run_line(void);
void c64_run_frame(void);

// Push an entry into the ring (waits while it is full).
void c64_ring_push(uint32_t cycle, uint8_t reg, uint8_t value);
// Line marker: raster line `line`, which started at cycle `start`, is complete.
void c64_ring_line(unsigned line, uint32_t start);

// VIC bank base address (CIA2 port A bits 0-1, inverted).
static inline uint16_t c64_vic_bank(void) {
    uint8_t pa = c64m.cia2.pra | (uint8_t)~c64m.cia2.ddra;
    return (uint16_t)((3 - (pa & 3)) << 14);
}

#endif
