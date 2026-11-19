// VIC-II renderer (core 1): see vic_render.h.
//
// Pixel coordinates: px 0..383 of the visible line is sprite X -8..375. A
// write in cycle c of a line takes effect from px 8c - 100 (cycle 13 -> px 4,
// the display window starts at px 32 = X 24).
//
// A line is built as 384 bytes: colour index in bits 0-3, bit 4 = graphics
// foreground (sprite priority and sprite-background collisions), bit 5 =
// border. Then it is expanded to RGB565.

#include <string.h>

#include "c64.h"
#include "vic_render.h"

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

// Approximate PAL palette ("Pepto").
static const uint16_t palette[16] = {
    RGB565(0x00, 0x00, 0x00), RGB565(0xff, 0xff, 0xff), RGB565(0x68, 0x37, 0x2b), RGB565(0x70, 0xa4, 0xb2),
    RGB565(0x6f, 0x3d, 0x86), RGB565(0x58, 0x8d, 0x43), RGB565(0x35, 0x28, 0x79), RGB565(0xb8, 0xc7, 0x6f),
    RGB565(0x6f, 0x4f, 0x25), RGB565(0x43, 0x39, 0x00), RGB565(0x9a, 0x67, 0x59), RGB565(0x44, 0x44, 0x44),
    RGB565(0x6c, 0x6c, 0x6c), RGB565(0x9a, 0xd2, 0x84), RGB565(0x6c, 0x5e, 0xb5), RGB565(0x95, 0x95, 0x95),
};

void vicr_expand_native(const uint8_t *packed, uint16_t *out) {
    for (unsigned x = 0; x < VICR_W; x += 2) {
        uint8_t b = packed[x / 2];
        out[x] = palette[b & 15];
        out[x + 1] = palette[b >> 4];
    }
}

#define W (384)
#define FG (0x10)
#define BORDER (0x20)
#define PX_OF_CYCLE(c) (8 * (int)(c) - 100)

static uint8_t regs[64];

// bitmask8[b][h]: half h of the 8 pixels of byte b; byte k of the 8 (pixel k,
// from bit 7 down) is 0xff if that bit is set.
static uint32_t bitmask8[256][2];
// pal2[(a << 4) | b]: two RGB565 pixels, a first (lower address).
static uint32_t pal2[256];


// Raster lines fed so far (markers): the line being drawn is number `rlines`
// in core 0's count, which may already be running the next one.
static uint32_t rlines;
static uint16_t vbank;      // VIC bank base, from the ring


// Writes of the line being collected (before its marker).
#define MAX_WRITES (64)
static c64_vic_write_t wbuf[MAX_WRITES];
static unsigned wn;

// Video logic.
static unsigned vc, vcbase, rc;
static uint8_t display, line_display;   // display state; during this line's g-accesses
static uint8_t den_seen;                // DEN set in some cycle of line $30
static uint8_t main_ff = 1, vert_ff = 1;
static uint8_t cbuf[40], colbuf[40];    // c-accesses of the last badline
static unsigned line_vc;                // VC at the start of this line's g-accesses

// Graphics of the current line for the current registers: 320 pixels starting
// at the first graphics pixel (px 32 + XSCROLL).
static uint8_t gfx[320] __attribute__((aligned(8)));
static uint32_t gcache_addr[40];    // g-access cache of the current line (reset per line)
static uint8_t gcache[40];
static unsigned glocked;            // chars 0..glocked-1 fetched before a mid-line mode/base change
static uint8_t gfx_dirty;
static unsigned gfx_from;           // chars gfx_from..39 of gfx[] are up to date

// Sprites.
static uint8_t sp_dma, sp_ff = 0xff, sp_show, sp_show_next;
static uint8_t sp_mc[8], sp_mcbase[8];     // 6-bit sprite data counters
static uint8_t sp_data[8][3], sp_data_next[8][3];
static uint8_t sp_fetch;        // sprites whose data fetch (EV_Sn) is pending

static uint8_t line_buf[W];
static uint8_t sp_mask[W];         // sprites with a pixel there (cleared after the line)

// Sprite registers at the start of the line and the line's writes to them, in
// pixel order (sprites are drawn after the line, see draw_sprites()).
#define MAX_SPW (64)
static uint8_t sregs[0x30];
static int spw_px[MAX_SPW];
static uint8_t spw_reg[MAX_SPW], spw_val[MAX_SPW];
static unsigned spw_n;

static void sort_fixed_events(void);

void vicr_init(void) {
    memset(regs, 0, sizeof(regs));
    for (unsigned b = 0; b < 256; ++b) {
        uint64_t m = 0;
        for (unsigned k = 0; k < 8; ++k) {
            if (b & (0x80 >> k)) {
                m |= 0xffull << (8 * k);
            }
        }
        bitmask8[b][0] = (uint32_t)m;
        bitmask8[b][1] = (uint32_t)(m >> 32);
        pal2[b] = palette[b >> 4] | ((uint32_t)palette[b & 15] << 16);
    }
    rlines = 0;
    vbank = c64_vic_bank();
    sort_fixed_events();
    wn = 0;
    vc = vcbase = rc = 0;
    display = line_display = 0;
    den_seen = 0;
    main_ff = vert_ff = 1;
    sp_dma = sp_show = sp_show_next = sp_fetch = 0;
    memset(sp_mc, 0, sizeof(sp_mc));
    memset(sp_mcbase, 0, sizeof(sp_mcbase));
    sp_ff = 0xff;
    gfx_dirty = 1;
    memset(sp_mask, 0, sizeof(sp_mask));
}

uint16_t vicr_border_rgb565(void) {
    return palette[regs[0x20] & 15];
}

// RAM (0x10000 + n: colour RAM) as it was at the end of the line being drawn:
// if core 0 already changed the byte in the next line, its log has the old
// value (the first entry for the address).
static uint8_t ram_at_slow(uint32_t a, uint8_t v);

static inline uint8_t ram_at(uint32_t a) {
    uint8_t v = (a & 0x10000) ? c64m.color_ram[a & 0x3ff] : c64m.ram[a];
    __asm__ volatile("" ::: "memory");
    const c64_ahead_t *ah = &c64m.ahead[(rlines + 1) & 1];
    unsigned h = c64_ahead_hash(a);
    if (ah->filter[h >> 5] & (1u << (h & 31))) {
        return ram_at_slow(a, v);
    }
    return v;
}

// The filter says the address may be in the log of the next line.
static uint8_t ram_at_slow(uint32_t a, uint8_t v) {
    const c64_ahead_t *ah = &c64m.ahead[(rlines + 1) & 1];
    if (ah->line == rlines + 1) {
        uint32_t n = ah->n;
        __asm__ volatile("" ::: "memory");
        for (uint32_t i = 0; i < n; ++i) {
            if (ah->addr[i] == a) {
                return ah->old[i];
            }
        }
    }
    return v;
}

// RAM as it was in cycle `cycle` of the line being drawn: a write of this line
// in that cycle or later (CPU writes come after the VIC's access of the same
// cycle) has the value of that moment as its old value. One filter test for
// both logs (this line's and the next one's).
static uint8_t ram_at_cycle_slow(uint32_t a, uint8_t v, unsigned cycle);

static inline uint8_t ram_at_cycle(uint32_t a, unsigned cycle) {
    uint8_t v = (a & 0x10000) ? c64m.color_ram[a & 0x3ff] : c64m.ram[a];
    __asm__ volatile("" ::: "memory");
    unsigned h = c64_ahead_hash(a);
    uint32_t f = c64m.ahead[0].filter[h >> 5] | c64m.ahead[1].filter[h >> 5];
    if (f & (1u << (h & 31))) {
        return ram_at_cycle_slow(a, v, cycle);
    }
    return v;
}

static uint8_t ram_at_cycle_slow(uint32_t a, uint8_t v, unsigned cycle) {
    v = ram_at_slow(a, v);
    const c64_ahead_t *ah = &c64m.ahead[rlines & 1];
    if (ah->line == rlines) {
        uint32_t n = ah->n;
        __asm__ volatile("" ::: "memory");
        for (uint32_t i = 0; i < n; ++i) {
            if (ah->addr[i] == a && ah->cycle[i] >= cycle) {
                return ah->old[i];
            }
        }
    }
    return v;
}

// A byte as the VIC sees it: 14-bit address in the current bank; the character
// ROM appears at $1000-$1FFF of banks 0 and 2.
static inline uint8_t vic_mem(uint16_t bank, uint16_t addr) {
    addr &= 0x3fff;
    if (!(bank & 0x4000) && (addr & 0x3000) == 0x1000) {
        return c64m.chargen[addr & 0x0fff];
    }
    return ram_at(bank | addr);
}

// The same, in cycle `cycle` of the line.
static inline uint8_t vic_mem_at(uint16_t bank, uint16_t addr, unsigned cycle) {
    addr &= 0x3fff;
    if (!(bank & 0x4000) && (addr & 0x3000) == 0x1000) {
        return c64m.chargen[addr & 0x0fff];
    }
    return ram_at_cycle(bank | addr, cycle);
}

/******************************************************************************/
// Graphics sequencer

// A colour index in all 4 bytes of a word.
#define BYTES4(c) ((uint32_t)(c) * 0x01010101u)

// 8 pixels: c0x / c1x = background / foreground colour in all 4 bytes.
static inline void put8(uint8_t *o, uint8_t bits, uint32_t c0x, uint32_t c1x) {
    uint32_t d = c0x ^ c1x;
    uint32_t v[2] = { c0x ^ (d & bitmask8[bits][0]), c0x ^ (d & bitmask8[bits][1]) };
    memcpy(o, v, 8);
}

// 4 double-width pixels from 2-bit codes.
static inline void put_mc(uint8_t *o, uint8_t bits, const uint8_t c[4]) {
    uint32_t v[2] = {
        (c[bits >> 6] * 0x0101u) | ((uint32_t)(c[(bits >> 4) & 3] * 0x0101u) << 16),
        (c[(bits >> 2) & 3] * 0x0101u) | ((uint32_t)(c[bits & 3] * 0x0101u) << 16),
    };
    memcpy(o, v, 8);
}

// g-access of char i with the current registers: read once per line per
// address (gen_gfx() runs again after each write that changes the picture,
// mostly with the same addresses).
static inline uint8_t g_access(unsigned i) {
    uint8_t d011 = regs[0x11], d018 = regs[0x18];
    uint32_t ga;
    if (line_display) {
        uint8_t c = cbuf[i];
        if (d011 & 0x20) {
            ga = ((d018 & 8) << 10) | (((line_vc + i) & 0x3ff) << 3) | rc;
        } else {
            ga = (((d018 >> 1) & 7) << 11) | ((d011 & 0x40 ? c & 0x3f : c) << 3) | rc;
        }
    } else {
        ga = (d011 & 0x40) ? 0x39ff : 0x3fff;
    }
    ga |= (uint32_t)vbank << 16;
    if (gcache_addr[i] != ga) {
        gcache_addr[i] = ga;
        gcache[i] = vic_mem_at(vbank, ga & 0xffff, 15 + i);
    }
    return gcache[i];
}

// A write in cycle `cycle` that changes g-access addresses: chars whose
// g-access (cycle 15 + i) came before that cycle keep the byte read with the
// old values (the access in the write's own cycle already sees the new value,
// as in VICE: Nine, frame 6000, $D011 ECM off in cycle 28).
static void g_lock(int cycle) {
    int n = cycle - 15;
    if (n > 40) {
        n = 40;
    }
    for (; (int)glocked < n; ++glocked) {
        g_access(glocked);
    }
}

// Chars `from`..39 (those to the left are already drawn: a write that changes
// the picture only matters from its pixel on).
static void gen_gfx(unsigned from) {
    uint8_t d011 = regs[0x11], d016 = regs[0x16];
    unsigned mode = ((d011 >> 4) & 6) | ((d016 >> 4) & 1);  // ECM BMM MCM
    uint8_t invalid = mode >= 5;
    uint8_t b0 = regs[0x21] & 15;
    uint32_t b0x = BYTES4(b0);
    for (unsigned i = from; i < 40; ++i) {
        uint8_t c = 0, col = 0;
        if (line_display) {
            c = cbuf[i];
            col = colbuf[i];
        }
        uint8_t g = i < glocked ? gcache[i] : g_access(i);
        uint8_t *o = &gfx[i * 8];
        switch (mode & 3) {
            case 0:     // standard text / ECM text (mode 4)
                if (mode & 4) {
                    put8(o, g, BYTES4(regs[0x21 + (c >> 6)] & 15), BYTES4((col & 15) | FG));
                } else {
                    put8(o, g, b0x, BYTES4((col & 15) | FG));
                }
                break;
            case 1:     // multicolour text
                if (col & 8) {
                    const uint8_t cc[4] = { b0, regs[0x22] & 15, (regs[0x23] & 15) | FG, (col & 7) | FG };
                    put_mc(o, g, cc);
                } else {
                    put8(o, g, b0x, BYTES4((col & 7) | FG));
                }
                break;
            case 2:     // bitmap
                put8(o, g, BYTES4(c & 15), BYTES4((c >> 4) | FG));
                break;
            default: {  // multicolour bitmap
                const uint8_t cc[4] = { b0, c >> 4, (c & 15) | FG, (col & 15) | FG };
                put_mc(o, g, cc);
                break;
            }
        }
        if (invalid) {
            for (unsigned k = 0; k < 8; ++k) {
                o[k] &= FG;     // black, the foreground bit is kept (collisions)
            }
        }
    }
    gfx_dirty = 0;
    gfx_from = from;
}

// Pixels [x0, x1) of the line with the current registers and border state.
static void render_span_(int x0, int x1);

// Profiling: events + background, sprites, expand, lines drawn, render_span
// (part of the first), gen_gfx (part of render_span).
uint32_t (*vicr_clock)(void);
uint32_t vicr_prof[6];

static void render_span(int x0, int x1) {
    uint32_t t = vicr_clock ? vicr_clock() : 0;
    render_span_(x0, x1);
    if (vicr_clock) {
        vicr_prof[4] += vicr_clock() - t;
    }
}

static void render_span_(int x0, int x1) {
    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 > W) {
        x1 = W;
    }
    if (x0 >= x1) {
        return;
    }
    if (main_ff) {
        memset(&line_buf[x0], (regs[0x20] & 15) | BORDER, x1 - x0);
        return;
    }
    // Background colour, then the graphics, then background again.
    int g0 = 32 + (regs[0x16] & 7), g1 = g0 + 320;
    // First char this span shows (XSCROLL may have moved it left of the chars
    // regenerated last time).
    unsigned first = x0 <= g0 ? 0 : (unsigned)(x0 - g0) / 8;
    if (first < 40 && (gfx_dirty || first < gfx_from)) {
        uint32_t t = vicr_clock ? vicr_clock() : 0;
        gen_gfx(first);
        if (vicr_clock) {
            vicr_prof[5] += vicr_clock() - t;
        }
    }
    uint8_t bg = regs[0x21] & 15;
    int a = x0 < g0 ? (x1 < g0 ? x1 : g0) : x0;     // end of the left background part
    if (a > x0) {
        memset(&line_buf[x0], bg, a - x0);
    }
    int b = x1 < g1 ? x1 : g1;                      // end of the graphics part
    if (b > a) {
        memcpy(&line_buf[a], &gfx[a - g0], b - a);
    }
    int c = b > a ? b : a;
    if (x1 > c) {
        memset(&line_buf[c], bg, x1 - c);
    }
}

/******************************************************************************/
// Fixed events of a line (vic-ii.txt section 3)

// EV_Sn: p- and s-accesses of sprite n: cycles 58, 60, 62 (sprites 0-2, for
// the next line) and 1, 3, 5, 7, 9 of the line itself (sprites 3-7).
enum { EV_S3, EV_S4, EV_S5, EV_S6, EV_S7, EV_C14, EV_C16, EV_LEFT, EV_C55, EV_RIGHT, EV_C58, EV_S0, EV_S1, EV_S2,
       EV_C63, EV_COUNT };

static int event_px(unsigned ev) {
    uint8_t csel = regs[0x16] & 8;
    switch (ev) {
        case EV_C14: return PX_OF_CYCLE(14);
        case EV_C16: return PX_OF_CYCLE(16);
        case EV_LEFT: return csel ? 32 : 39;        // X 24 / 31
        case EV_C55: return PX_OF_CYCLE(55);
        case EV_RIGHT: return csel ? 352 : 343;     // X 344 / 335
        case EV_C58: return PX_OF_CYCLE(58);
        case EV_S0: return PX_OF_CYCLE(58);
        case EV_S1: return PX_OF_CYCLE(60);
        case EV_S2: return PX_OF_CYCLE(62);
        case EV_S3: return PX_OF_CYCLE(1);
        case EV_S4: return PX_OF_CYCLE(3);
        case EV_S5: return PX_OF_CYCLE(5);
        case EV_S6: return PX_OF_CYCLE(7);
        case EV_S7: return PX_OF_CYCLE(9);
        default: return PX_OF_CYCLE(63);
    }
}

// The fixed events (all but EV_LEFT and EV_RIGHT) sorted by pixel position,
// ties in enum order (built by vicr_init()).
#define N_FIXED (EV_COUNT - 2)
static uint8_t fixed_ev[N_FIXED];
static int fixed_px[N_FIXED];

static void sort_fixed_events(void) {
    unsigned n = 0;
    for (unsigned e = 0; e < EV_COUNT; ++e) {
        if (e == EV_LEFT || e == EV_RIGHT) {
            continue;
        }
        int p = event_px(e);
        unsigned i = n++;
        while (i > 0 && (fixed_px[i - 1] > p || (fixed_px[i - 1] == p && fixed_ev[i - 1] > e))) {
            fixed_px[i] = fixed_px[i - 1];
            fixed_ev[i] = fixed_ev[i - 1];
            i--;
        }
        fixed_px[i] = p;
        fixed_ev[i] = e;
    }
}

static unsigned border_top(void) {
    return (regs[0x11] & 8) ? 0x33 : 0x37;
}

static unsigned border_bottom(void) {
    return (regs[0x11] & 8) ? 0xfb : 0xf7;
}

// p- and s-accesses of sprite n (pointer and three bytes at MC), if its DMA
// was on at cycle 58.
static void sprite_fetch(unsigned n, uint8_t (*into)[3]) {
    uint8_t bit = 1u << n;
    if (!(sp_fetch & bit)) {
        return;
    }
    sp_fetch &= ~bit;
    uint16_t bank = vbank;
    uint16_t base = vic_mem(bank, (((regs[0x18] >> 4) << 10) | 0x3f8) + n) << 6;
    for (unsigned k = 0; k < 3; ++k) {
        into[n][k] = vic_mem(bank, base + sp_mc[n]);
        sp_mc[n] = (sp_mc[n] + 1) & 63;
    }
}

static void event(unsigned ev, unsigned line) {
    switch (ev) {
        case EV_C14: {
            if (line == 0x30 && (regs[0x11] & 0x10)) {
                den_seen = 1;
            }
            int badline = den_seen && line >= 0x30 && line <= 0xf7 && (line & 7) == (regs[0x11] & 7u);
            vc = vcbase;
            line_vc = vc;
            if (badline) {
                rc = 0;
                display = 1;
                uint16_t bank = vbank;
                uint16_t vm = (regs[0x18] >> 4) << 10;
                for (unsigned i = 0; i < 40; ++i) {
                    unsigned v = (vc + i) & 0x3ff;
                    cbuf[i] = vic_mem(bank, vm | v);
                    colbuf[i] = ram_at(0x10000 | v) & 15;
                }
            }
            line_display = display;
            if (display) {
                vc = (vc + 40) & 0x3ff;
            }
            gfx_dirty = 1;
            break;
        }
        case EV_C16:
            for (unsigned n = 0; n < 8; ++n) {
                uint8_t bit = 1u << n;
                if (sp_ff & bit) {
                    sp_mcbase[n] = sp_mc[n];
                    if (sp_mcbase[n] == 63) {
                        sp_dma &= ~bit;
                    }
                }
            }
            break;
        case EV_LEFT:
            if (line == border_bottom()) {
                vert_ff = 1;
            } else if (line == border_top() && (regs[0x11] & 0x10)) {
                vert_ff = 0;
            }
            if (!vert_ff) {
                main_ff = 0;
            }
            break;
        case EV_C55:
            for (unsigned n = 0; n < 8; ++n) {
                uint8_t bit = 1u << n;
                if (regs[0x17] & bit) {
                    sp_ff ^= bit;
                } else {
                    sp_ff |= bit;
                }
                if ((regs[0x15] & bit) && regs[1 + 2 * n] == (line & 0xff) && !(sp_dma & bit)) {
                    sp_dma |= bit;
                    sp_mcbase[n] = 0;
                    if (regs[0x17] & bit) {
                        sp_ff &= ~bit;
                    }
                }
            }
            break;
        case EV_RIGHT:
            main_ff = 1;
            break;
        case EV_C58: {
            if (rc == 7) {
                display = 0;
                vcbase = vc;
            }
            int badline = den_seen && line >= 0x30 && line <= 0xf7 && (line & 7) == (regs[0x11] & 7u);
            if (badline) {
                display = 1;
            }
            if (display) {
                rc = (rc + 1) & 7;
            }
            // Sprites shown in the next line; their data is fetched at EV_Sn.
            sp_show_next = 0;
            for (unsigned n = 0; n < 8; ++n) {
                sp_mc[n] = sp_mcbase[n];
                if (sp_dma & (1u << n)) {
                    sp_show_next |= 1u << n;
                }
            }
            sp_fetch = sp_show_next;
            break;
        }
        case EV_S0: case EV_S1: case EV_S2:
            sprite_fetch(ev - EV_S0, sp_data_next);
            break;
        case EV_S3: case EV_S4: case EV_S5: case EV_S6: case EV_S7:
            sprite_fetch(3 + ev - EV_S3, sp_data);
            break;
        default:    // cycle 63
            if (line == border_bottom()) {
                vert_ff = 1;
            } else if (line == border_top() && (regs[0x11] & 0x10)) {
                vert_ff = 0;
            }
            break;
    }
}

// cycle: cycle of the write within the line.
static void apply_write(const c64_vic_write_t *w, unsigned line, int cycle) {
    if (w->reg == C64_RING_BANK) {
        g_lock(cycle);
        vbank = (uint16_t)(w->value << 14);
        gfx_dirty = 1;
        return;
    }
    uint8_t reg = w->reg & 0x3f;
    if (reg == 0x17) {
        // Clearing a Y-expand bit sets the flip-flop; in cycle 15 with the
        // flip-flop reset it mixes MC and MCBASE (sprite crunch).
        uint8_t cleared = regs[0x17] & ~w->value;
        for (unsigned n = 0; n < 8; ++n) {
            uint8_t bit = 1u << n;
            if (cleared & bit) {
                if (cycle == 15 && !(sp_ff & bit)) {
                    sp_mc[n] = ((sp_mc[n] | sp_mcbase[n]) & 0x15) | ((sp_mc[n] & sp_mcbase[n]) & 0x2a);
                }
                sp_ff |= bit;
            }
        }
    }
    if ((reg <= 0x10 || (reg >= 0x1b && reg <= 0x1d) || (reg >= 0x25 && reg <= 0x2e)) && spw_n < MAX_SPW) {
        spw_px[spw_n] = PX_OF_CYCLE(cycle);
        spw_reg[spw_n] = reg;
        spw_val[spw_n] = w->value;
        spw_n++;
    }
    uint8_t old = regs[reg];
    if ((reg == 0x11 && ((old ^ w->value) & 0x60)) || (reg == 0x18 && old != w->value)) {
        g_lock(cycle);
    }
    regs[reg] = w->value;
    if (reg == 0x11 && line == 0x30 && (w->value & 0x10)) {
        den_seen = 1;
    }
    // The graphics depend on ECM/BMM, MCM, $D018 and the background colours
    // (not on CSEL, RSEL, scroll or DEN).
    if ((reg == 0x11 && ((old ^ w->value) & 0x60)) || (reg == 0x16 && ((old ^ w->value) & 0x10)) ||
        (reg == 0x18 && old != w->value) || (reg >= 0x21 && reg <= 0x24 && old != w->value)) {
        gfx_dirty = 1;
    }
}

/******************************************************************************/
// Sprites (data fetched in the previous line), over the line

// Pixels [pa, pb) of sprite n starting at pixel t, with registers r (fixed
// over that range); pixel width 1 << xexp. Sprites are drawn 0 first: a pixel
// taken by a lower-numbered sprite is only a collision (the lower number wins,
// its priority bit decides against the background). Drawn pixels keep the FG
// flag for the background collisions of the sprites after.
#define SPRITE_PIXEL(p, col) do {                                       \
        uint8_t m_ = sp_mask[p], px_ = line_buf[p];                     \
        if (!(px_ & BORDER) && (px_ & FG)) {                            \
            *coll_sb |= bit;                                            \
        }                                                               \
        if (m_) {                                                       \
            *coll_ss |= m_ | bit;                                       \
            sp_mask[p] = m_ | bit;                                      \
        } else {                                                        \
            sp_mask[p] = bit;                                           \
            if (!(px_ & BORDER) && !((px_ & FG) && pri)) {              \
                line_buf[p] = (col) | (px_ & FG);                       \
            }                                                           \
        }                                                               \
    } while (0)

static void draw_sprite_seg(int n, const uint8_t *r, int t, unsigned xexp, uint32_t data, int pa, int pb,
                            uint8_t *coll_ss, uint8_t *coll_sb) {
    uint8_t bit = 1u << n;
    if (pa < 0) pa = 0;
    if (pb > W) pb = W;
    uint8_t pri = r[0x1b] & bit;
    uint8_t cols[4] = { 0, r[0x25] & 15, r[0x27 + n] & 15, r[0x26] & 15 };
    if (pa >= pb) {
        return;
    }
    // Sprite pixels (of 24) that meet [pa, pb).
    unsigned i0 = pa > t ? (unsigned)(pa - t) >> xexp : 0;
    unsigned i1 = ((unsigned)(pb - t) + (1u << xexp) - 1) >> xexp;
    if (i1 > 24) i1 = 24;
    if (r[0x1c] & bit) {
        for (unsigned k = i0 >> 1; k < (i1 + 1) >> 1; ++k) {
            unsigned c = (data >> (22 - 2 * k)) & 3;
            if (!c) {
                continue;
            }
            int p0 = t + (int)((2 * k) << xexp), p1 = p0 + (2 << xexp);
            if (p0 < pa) p0 = pa;
            if (p1 > pb) p1 = pb;
            for (int p = p0; p < p1; ++p) {
                SPRITE_PIXEL(p, cols[c]);
            }
        }
    } else {
        uint32_t d = data << 8;     // bit 31 = leftmost pixel
        d &= (0xffffffffu >> i0) & ~(0xffffffffu >> i1);
        while (d) {
            unsigned i = __builtin_clz(d);
            d &= ~(0x80000000u >> i);
            int p0 = t + (int)(i << xexp), p1 = p0 + (1 << xexp);
            if (p0 < pa) p0 = pa;
            if (p1 > pb) p1 = pb;
            for (int p = p0; p < p1; ++p) {
                SPRITE_PIXEL(p, cols[2]);
            }
        }
    }
}

static void draw_sprites(uint8_t *coll_ss, uint8_t *coll_sb) {
    uint8_t show = sp_show;
    if (!show) {
        return;
    }
    int lo = W, hi = 0;
    // Lowest number has priority: drawn first, see draw_sprite_seg(). Each
    // sprite sees the register writes of the line at their pixel (colours,
    // multicolour, priority): a local copy of the registers is advanced through
    // the line's sprite-register writes as the sprite's pixels go by.
    for (int n = 0; n < 8; ++n) {
        uint8_t bit = 1u << n;
        if (!(show & bit)) {
            continue;
        }
        uint32_t data = (sp_data[n][0] << 16) | (sp_data[n][1] << 8) | sp_data[n][2];
        if (!data) {
            continue;
        }
        if (spw_n == 0) {
            // Registers fixed for the whole line.
            unsigned x = regs[2 * n] | ((regs[0x10] & bit) ? 0x100 : 0);
            int t = x < 404 ? (int)x + 8 : x < 504 ? (int)x + 8 - 504 : 1 << 20;
            if (t >= W) {
                continue;
            }
            unsigned xexp = (regs[0x1d] & bit) ? 1 : 0;
            int end = t + (24 << xexp);
            draw_sprite_seg(n, regs, t, xexp, data, t, end, coll_ss, coll_sb);
            if (t < lo) lo = t < 0 ? 0 : t;
            if (end - 1 > hi) hi = end - 1 >= W ? W - 1 : end - 1;
            continue;
        }
        uint8_t r[0x30];
        memcpy(r, sregs, sizeof(r));
        unsigned wi = 0;
#define ADVANCE(t) while (wi < spw_n && spw_px[wi] <= (t)) { r[spw_reg[wi]] = spw_val[wi]; wi++; }
        ADVANCE(0);
        // Start: the X counter reaching the X register; a write before that
        // point moves the start. PAL: the counter runs 404..503 at the start of
        // a line, then 0..403, so a sprite at X >= 404 starts before the
        // picture (px 0 = X 496) and its pixels come in on the left; X 504-511
        // is never reached.
        int t = 0;
        for (unsigned k = 0; k < 4; ++k) {
            unsigned x = r[2 * n] | ((r[0x10] & bit) ? 0x100 : 0);
            t = x < 404 ? (int)x + 8 : x < 504 ? (int)x + 8 - 504 : 1 << 20;
            unsigned before = wi;
            ADVANCE(t);
            if (wi == before) {
                break;
            }
        }
        if (t >= W) {
            continue;   // starts right of the picture, or never (X 504-511)
        }
        // Segments between the writes that fall inside the sprite: registers
        // fixed in each (X expansion is taken at the start).
        unsigned xexp = (r[0x1d] & bit) ? 1 : 0;
        int end = t + (24 << xexp);
        for (int pa = t; pa < end;) {
            ADVANCE(pa);
            int pb = wi < spw_n && spw_px[wi] < end ? spw_px[wi] : end;
            draw_sprite_seg(n, r, t, xexp, data, pa, pb, coll_ss, coll_sb);
            pa = pb;
        }
        if (t < lo) lo = t < 0 ? 0 : t;
        if (end - 1 > hi) hi = end - 1 >= W ? W - 1 : end - 1;
#undef ADVANCE
    }
    if (lo <= hi) {
        memset(&sp_mask[lo], 0, hi - lo + 1);
    }
}

#ifdef VICR_DEBUG
#include <stdio.h>
int vicr_debug_line = -1;
#endif

// The line's colours (low nibbles of line_buf) as 4-bit indices, two pixels
// per byte, first pixel in the low nibble.
static void pack(uint8_t *out8) {
    const uint8_t *s = line_buf;
    uint32_t *out = (uint32_t *)out8;
    for (unsigned g = 0; g < W / 8; ++g, s += 8) {
        uint32_t a, b;
        memcpy(&a, s, 4);
        memcpy(&b, s + 4, 4);
        a &= 0x0f0f0f0fu;
        b &= 0x0f0f0f0fu;
        a |= a >> 4;    // byte 0: p0 | p1 << 4, byte 2: p2 | p3 << 4
        b |= b >> 4;
        out[g] = (a & 0xffu) | ((a >> 8) & 0xff00u) | ((b & 0xffu) << 16) | ((b << 8) & 0xff000000u);
    }
}

// 8 C64 pixels -> 10 panel pixels (each group of 4 starts with a doubled
// pixel), written as 5 words of two pixels; pal2[(a << 4) | b]: pixels a, b.
void vicr_expand(const uint8_t *packed, uint16_t *out16) {
    const uint32_t *in = (const uint32_t *)packed;
    uint32_t *out = (uint32_t *)out16;
    for (unsigned g = 0; g < W / 8; ++g, out += 5) {
        uint32_t w = in[g];     // nibble k = pixel k
        out[0] = pal2[((w & 0x0fu) << 4) | (w & 0x0fu)];
        out[1] = pal2[(w & 0xf0u) | ((w >> 8) & 0x0fu)];
        out[2] = pal2[((w >> 8) & 0xf0u) | ((w >> 16) & 0x0fu)];
        out[3] = pal2[((w >> 12) & 0xf0u) | ((w >> 20) & 0x0fu)];
        out[4] = pal2[((w >> 20) & 0xf0u) | (w >> 28)];
    }
}

/******************************************************************************/
// One complete line

// Profiling (optional): cycles per stage, with a clock set by the firmware
// (declared before render_span()).

#define PROF_MARK(i) do { if (vicr_clock) { uint32_t t_ = vicr_clock(); vicr_prof[i] += t_ - prof_t; prof_t = t_; } } while (0)

static void do_line(unsigned line, uint32_t start, uint8_t *out) {
    uint32_t prof_t = vicr_clock ? vicr_clock() : 0;
    if (line == 0) {
        vcbase = 0;
    }
    if (line == 0x30) {
        den_seen = regs[0x11] & 0x10;
    }
    sp_show = sp_show_next;
    memcpy(sp_data, sp_data_next, sizeof(sp_data));
    memcpy(sregs, regs, sizeof(sregs));
    spw_n = 0;
    memset(gcache_addr, 0xff, sizeof(gcache_addr));
    glocked = 0;

    // Events in pixel order: the fixed ones from a sorted table; the two border
    // comparisons move with CSEL and are evaluated when due (a comparison whose
    // position is already behind, after a CSEL change, is missed).
    unsigned fi = 0;
    uint8_t left_done = 0, right_done = 0;
    unsigned wi = 0;
    int pos = -1000;    // before the line: events and writes of cycles 0..12
    int rpos = 0;       // pixels drawn so far (only writes and C14/LEFT/RIGHT change them)
    for (;;) {
        int wpx = wi < wn ? PX_OF_CYCLE(wbuf[wi].cycle - start) : 1 << 30;
        int epx = fi < N_FIXED ? fixed_px[fi] : 1 << 30;
        unsigned ev = fi < N_FIXED ? fixed_ev[fi] : EV_COUNT;
        if (!left_done) {
            int p = event_px(EV_LEFT);
            if (p < pos) {
                left_done = 1;
            } else if (p < epx || (p == epx && EV_LEFT < ev)) {
                epx = p;
                ev = EV_LEFT;
            }
        }
        if (!right_done) {
            int p = event_px(EV_RIGHT);
            if (p < pos) {
                right_done = 1;
            } else if (p < epx || (p == epx && EV_RIGHT < ev)) {
                epx = p;
                ev = EV_RIGHT;
            }
        }
        if (wi >= wn && ev == EV_COUNT) {
            break;
        }
        int next = wpx <= epx ? wpx : epx;
        bool visual = wpx <= epx || ev == EV_C14 || ev == EV_LEFT || ev == EV_RIGHT;
        if (out && visual && next > rpos) {
            render_span(rpos, next);
            rpos = next;
        }
        if (next > pos) {
            pos = next;
        }
        if (wpx <= epx) {
            apply_write(&wbuf[wi], line, (int)(wbuf[wi].cycle - start));
            wi++;
        } else {
            if (ev == EV_LEFT) {
                left_done = 1;
            } else if (ev == EV_RIGHT) {
                right_done = 1;
            } else {
                fi++;
            }
            event(ev, line);
        }
    }
    if (out) {
        if (rpos < W) {
            render_span(rpos, W);
        }
        PROF_MARK(0);
        uint8_t ss = 0, sb = 0;
        draw_sprites(&ss, &sb);
        PROF_MARK(1);
        uint8_t irq = 0;
        if (ss && __atomic_fetch_or(&c64m.coll_ss, ss, __ATOMIC_SEQ_CST) == 0) {
            irq |= 4;
        }
        if (sb && __atomic_fetch_or(&c64m.coll_sb, sb, __ATOMIC_SEQ_CST) == 0) {
            irq |= 2;
        }
        if (irq) {
            __atomic_fetch_or(&c64m.coll_irq, irq, __ATOMIC_SEQ_CST);
        }
#ifdef VICR_DEBUG
        if ((int)line == vicr_debug_line) {
            printf("dbg line %u: d011=%02x d016=%02x d018=%02x d021-23=%02x %02x %02x display=%u rc=%u vc=%u bank=%04x\n",
                   line, regs[0x11], regs[0x16], regs[0x18], regs[0x21], regs[0x22], regs[0x23], line_display, rc, line_vc,
                   c64_vic_bank());
            printf("dbg cbuf:");
            for (unsigned i = 0; i < 40; ++i) printf(" %02x", cbuf[i]);
            printf("\ndbg col: ");
            for (unsigned i = 0; i < 40; ++i) printf(" %02x", colbuf[i]);
            printf("\ndbg px:  ");
            for (unsigned x = 32; x < 352; ++x) printf("%x", line_buf[x] & 15);
            printf("\n");
        }
#endif
        pack(out);
        PROF_MARK(2);
        vicr_prof[3]++;
    }
    wn = 0;
}

int vicr_feed(const c64_vic_write_t *e, uint8_t *(*out_for_line)(unsigned line)) {
    if (e->reg != C64_RING_LINE) {
        if (wn < MAX_WRITES) {
            wbuf[wn++] = *e;
        } else {
            if (e->reg == C64_RING_BANK) {      // too many in one line: apply now
                vbank = (uint16_t)(e->value << 14);
            } else {
                regs[e->reg & 0x3f] = e->value;
            }
            gfx_dirty = 1;
        }
        return -1;
    }
    unsigned line = e->line;
    uint8_t *out = NULL;
    if (line >= VICR_FIRST_LINE && line < VICR_FIRST_LINE + VICR_LINES) {
        out = out_for_line(line);
    }
    do_line(line, e->cycle, out);
    rlines++;
    return (int)line;
}
