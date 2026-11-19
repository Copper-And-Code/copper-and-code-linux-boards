// C64 (PAL) machine on core 0: see c64.h.

#include <string.h>

#include "c64.h"
#include "c1541.h"
#include "sid.h"

c64_t c64m;

#ifdef C64_DEBUG_TRACE
// Host debugging: the PC of the instruction doing each I/O write.
#include <stdio.h>
int c64_trace_frame = -1;
static uint16_t trace_pc;
#define TRACE_PC(p) (trace_pc = (p))
#else
#define TRACE_PC(p) ((void)0)
#endif


#define FAR_FUTURE(now) ((now) + 0x40000000u)

/******************************************************************************/
// Memory map (no cartridge: EXROM = GAME = 1)

static void map_update(void) {
    // Unused DDR bits read as inputs; bits 0-2 are pulled up.
    uint8_t p = (c64m.port_data & c64m.port_ddr) | (~c64m.port_ddr & 0x17);
    bool loram = p & 1, hiram = p & 2, charen = p & 4;
    for (unsigned i = 0; i < 16; ++i) {
        c64m.rd[i] = &c64m.ram[i << 12];
    }
    bool game = c64m.cart_game, exrom = c64m.cart_exrom;
    if (game && exrom && !loram && !hiram) {
        // 16 KB cartridge with LORAM = HIRAM = 0: RAM everywhere, also $D000.
        c64m.io_visible = false;
        goto port;
    }
    if (loram && hiram) {
        c64m.rd[0xa] = c64m.basic;
        c64m.rd[0xb] = c64m.basic + 0x1000;
    }
    if (hiram) {
        c64m.rd[0xe] = c64m.kernal;
        c64m.rd[0xf] = c64m.kernal + 0x1000;
    }
    // Cartridge ROMs (PLA): ROML with LORAM and HIRAM; ROMH (16 KB) with HIRAM,
    // replacing BASIC. Writes go to the RAM underneath.
    if (exrom && loram && hiram && c64m.cart_roml) {
        c64m.rd[0x8] = c64m.cart_roml;
        c64m.rd[0x9] = c64m.cart_roml + 0x1000;
    }
    if (exrom && game && hiram && c64m.cart_romh) {
        c64m.rd[0xa] = c64m.cart_romh;
        c64m.rd[0xb] = c64m.cart_romh + 0x1000;
    }
    c64m.io_visible = false;
    if (loram || hiram) {
        if (charen) {
            c64m.rd[0xd] = NULL;
            c64m.io_visible = true;
        } else {
            c64m.rd[0xd] = c64m.chargen;
        }
    }
port:
    // What the CPU reads at $00/$01 (also kept in RAM so the fast path finds it).
    c64m.ram[0] = c64m.port_ddr;
    c64m.ram[1] = p;
}

/******************************************************************************/
// CIA 6526: timers A and B counting phi2 (and B counting A underflows), ICR,
// ports. Timing as measured against VICE (tests/c64/t06_cia.asm):
// - a timer counts N, N-1, ..., 1; in the cycle it would reach 0 it underflows
//   and shows the latch, for two cycles: period latch + 1;
// - a start takes effect 2 cycles after the CRA write, 3 with a force load
//   (also on a running timer); a stop 2 cycles after it.
// Counter state: value `cnt` at cycle `base` (held before it), then -1 per cycle.

static uint16_t timer_value(uint16_t cnt, uint32_t base, uint32_t now) {
    if ((int32_t)(now - base) < 0) {
        return cnt;
    }
    return (uint16_t)(cnt - (now - base));
}

// Assert IRQ sources; `at` is the cycle the line goes active (the CPU takes
// the interrupt only from the second cycle on, see cpu6510.h).
static void irq_raise(uint8_t bits, uint32_t at) {
    if (!c64m.cpu.irq) {
        c64m.cpu.irq_since = at;
    }
    c64m.cpu.irq |= bits;
}

// `at`: cycle the interrupt condition arose.
static void cia_irq_update(c64_cia_t *c, uint32_t at) {
    bool on = (c->icr_data & c->icr_mask & 0x1f) != 0;
    bool was = c->icr_data & 0x80;
    if (on) {
        c->icr_data |= 0x80;
    }
    if (c->is_cia2) {
        if (on && !was) {
            c64m.cpu.nmi = 1;   // NMI is edge triggered
        }
    } else if (on && !was) {
        irq_raise(C64_IRQ_CIA1, at);
    }
}

static void timer_b_underflow(c64_cia_t *c, uint32_t at) {
    c->icr_data |= 2;
    if (c->crb & 8) {
        c->crb &= ~1;   // one-shot
        c->cnt_b = c->latch_b;
        c->next_b = FAR_FUTURE(at);
    } else {
        c->cnt_b = c->latch_b;
        c->base_b = at + 1;
        c->next_b = c->base_b + c->latch_b;
    }
}

static bool timer_b_counts_a(const c64_cia_t *c) {
    return (c->crb & 0x61) == 0x41;
}

static bool timer_b_counts_phi2(const c64_cia_t *c) {
    return (c->crb & 0x61) == 0x01;
}

// Process all underflows up to cycle `now`.
static void cia_update(c64_cia_t *c, uint32_t now) {
    uint32_t last = now;    // last underflow; the IRQ output follows one cycle later
    bool underflow = false;
    while ((c->cra & 1) && (int32_t)(now - c->next_a) >= 0) {
        uint32_t at = c->next_a;
        last = at;
        underflow = true;
        c->icr_data |= 1;
        if (c->cra & 8) {
            c->cra &= ~1;
            c->cnt_a = c->latch_a;
            c->next_a = FAR_FUTURE(at);
        } else {
            c->cnt_a = c->latch_a;
            c->base_a = at + 1;
            c->next_a = c->base_a + c->latch_a;
        }
        if (timer_b_counts_a(c)) {
            if (c->cnt_b == 0 || --c->cnt_b == 0) {
                timer_b_underflow(c, at);
            }
        }
    }
    while (timer_b_counts_phi2(c) && (int32_t)(now - c->next_b) >= 0) {
        last = c->next_b;
        underflow = true;
        timer_b_underflow(c, c->next_b);
    }
    cia_irq_update(c, underflow ? last + 1 : now);
}

static uint32_t cia_next(const c64_cia_t *c, uint32_t now) {
    uint32_t n = FAR_FUTURE(now);
    if ((c->cra & 1) && (int32_t)(c->next_a - n) < 0) {
        n = c->next_a;
    }
    if (timer_b_counts_phi2(c) && (int32_t)(c->next_b - n) < 0) {
        n = c->next_b;
    }
    return n;
}

static void schedule(uint32_t now) {
    uint32_t a = cia_next(&c64m.cia1, now), b = cia_next(&c64m.cia2, now);
    c64m.next_event = (int32_t)(a - b) < 0 ? a : b;
}

static uint8_t cia1_port_b_in(void) {
    // Keyboard: a column driven low pulls the rows of its pressed keys low.
    // Joystick 2 is on port A: an active direction pulls that column low.
    uint8_t pa = (c64m.cia1.pra | (uint8_t)~c64m.cia1.ddra) & (uint8_t)~(c64m.joy2 & 0x1f);
    uint8_t rows = 0xff;
    for (unsigned col = 0; col < 8; ++col) {
        if (!(pa & (1u << col))) {
            rows &= ~c64m.kbd[col];
        }
    }
    return rows & ~(c64m.joy1 & 0x1f);
}

static uint8_t cia1_port_a_in(void) {
    // Rows driven low (port B outputs) pull the columns of pressed keys low.
    uint8_t pb = c64m.cia1.prb | (uint8_t)~c64m.cia1.ddrb;
    uint8_t cols = 0xff;
    for (unsigned col = 0; col < 8; ++col) {
        if (c64m.kbd[col] & ~pb) {
            cols &= ~(1u << col);
        }
    }
    return cols & ~(c64m.joy2 & 0x1f);
}

// CIA 2 port A bits 3-5 drive ATN, CLK and DATA through inverters (a 1, or an
// input, pulls the line low): bits 1 ATN, 2 CLK, 4 DATA for c1541_bus_c64().
static uint8_t cia2_bus_low(void) {
    uint8_t out = c64m.cia2.pra | (uint8_t)~c64m.cia2.ddra;
    return (uint8_t)((out >> 3) & 7);
}

static uint8_t cia2_port_a_in(void) {
    if (c1541_powered()) {
        return 0x3f | c1541_bus_c64_in();   // the caller ran the drive up to now
    }
    // Serial bus with no device: CLK IN (bit 6) and DATA IN (bit 7) show the
    // inverted CLK OUT (bit 4) and DATA OUT (bit 5) of the C64 itself.
    uint8_t out = c64m.cia2.pra | (uint8_t)~c64m.cia2.ddra;
    uint8_t in = 0x3f | ((out & 0x10) ? 0 : 0x40) | ((out & 0x20) ? 0 : 0x80);
    return in;
}

static uint8_t cia_read(c64_cia_t *c, uint8_t reg, uint32_t now) {
    cia_update(c, now);
    switch (reg & 15) {
        // A read gives the pin levels: outputs at 1 (and inputs, pulled up) can be
        // pulled low from outside (keyboard, joysticks: how games read port 2).
        case 0x0:
            if (c->is_cia2) {
                c1541_run_to(now);
                return (c->pra & c->ddra) | (cia2_port_a_in() & ~c->ddra);
            }
            return (c->pra | (uint8_t)~c->ddra) & cia1_port_a_in();
        case 0x1:
            if (c->is_cia2) {
                return c->prb | (uint8_t)~c->ddrb;
            }
            return (c->prb | (uint8_t)~c->ddrb) & cia1_port_b_in();
        case 0x2: return c->ddra;
        case 0x3: return c->ddrb;
        case 0x4: return (c->cra & 1) ? timer_value(c->cnt_a, c->base_a, now) & 0xff : c->cnt_a & 0xff;
        case 0x5: return (c->cra & 1) ? timer_value(c->cnt_a, c->base_a, now) >> 8 : c->cnt_a >> 8;
        case 0x6: return timer_b_counts_phi2(c) ? timer_value(c->cnt_b, c->base_b, now) & 0xff : c->cnt_b & 0xff;
        case 0x7: return timer_b_counts_phi2(c) ? timer_value(c->cnt_b, c->base_b, now) >> 8 : c->cnt_b >> 8;
        case 0x8: case 0x9: case 0xa: case 0xb: return c->tod[(reg & 15) - 8];
        case 0xc: return c->sdr;
        case 0xd: {
            uint8_t v = c->icr_data;
            c->icr_data = 0;
            if (!c->is_cia2) {
                c64m.cpu.irq &= ~C64_IRQ_CIA1;
            }
            return v;
        }
        case 0xe: return c->cra & ~0x10;
        default: return c->crb & ~0x10;
    }
}

// A CRA/CRB write for a timer: `was`/`run` = counting before/after, `load` =
// force load. Updates cnt/base/next.
static void timer_control(uint16_t *cnt, uint32_t *base, uint32_t *next, uint16_t latch, bool was, bool run,
                          bool load, uint32_t now) {
    if (was && !run) {
        *cnt = timer_value(*cnt, *base, now + 2);   // stops 2 cycles later
    } else if (!was && run) {
        if (load) {
            *cnt = latch;
        }
        *base = now + (load ? 3 : 2);
    } else if (was && run && load) {
        *cnt = latch;
        *base = now + 3;    // as a start with force load (measured against VICE)
    } else if (load) {
        *cnt = latch;
    }
    *next = *base + *cnt;
}

static void cia_write(c64_cia_t *c, uint8_t reg, uint8_t v, uint32_t now) {
    cia_update(c, now);
    if (c->is_cia2 && ((reg & 15) == 0x0 || (reg & 15) == 0x2)) {
        // Serial bus: the drive runs up to now, then sees the new lines.
        c1541_run_to(now);
        if ((reg & 15) == 0x0) {
            c->pra = v;
        } else {
            c->ddra = v;
        }
        c1541_bus_c64(cia2_bus_low());
        return;
    }
    switch (reg & 15) {
        case 0x0: c->pra = v; break;
        case 0x1: c->prb = v; break;
        case 0x2: c->ddra = v; break;
        case 0x3: c->ddrb = v; break;
        case 0x4: c->latch_a = (c->latch_a & 0xff00) | v; break;
        case 0x5:
            c->latch_a = (c->latch_a & 0x00ff) | (v << 8);
            if (!(c->cra & 1)) {
                c->cnt_a = c->latch_a;  // a stopped timer loads the counter
            }
            break;
        case 0x6: c->latch_b = (c->latch_b & 0xff00) | v; break;
        case 0x7:
            c->latch_b = (c->latch_b & 0x00ff) | (v << 8);
            if (!(c->crb & 1)) {
                c->cnt_b = c->latch_b;
            }
            break;
        case 0x8: case 0x9: case 0xa: case 0xb: c->tod[(reg & 15) - 8] = v; break;
        case 0xc: c->sdr = v; break;
        case 0xd:
            if (v & 0x80) {
                c->icr_mask |= v & 0x1f;
            } else {
                c->icr_mask &= ~v;
            }
            break;
        case 0xe: {
            bool was = c->cra & 1;
            timer_control(&c->cnt_a, &c->base_a, &c->next_a, c->latch_a, was, v & 1, v & 0x10, now);
            c->cra = v & ~0x10;
            break;
        }
        default: {
            bool was = timer_b_counts_phi2(c);
            c->crb = v & ~0x10;
            timer_control(&c->cnt_b, &c->base_b, &c->next_b, c->latch_b, was, timer_b_counts_phi2(c), v & 0x10, now);
            break;
        }
    }
    cia_irq_update(c, now);
}

/******************************************************************************/
// VIC-II timing model and registers

// The raster counter ($D011/$D012) advances in cycle 1 of a line.
static unsigned current_line(uint32_t now) {
    uint32_t k = now - c64m.line_start;
    unsigned l = c64m.line + k / C64_CYCLES_PER_LINE;
    if (k % C64_CYCLES_PER_LINE == 0) {
        l += C64_LINES - 1;     // cycle 0: still the previous line
    }
    return l % C64_LINES;
}

static void vic_irq_update(uint32_t at) {
    if (c64m.vic_irq & c64m.vic[0x1a] & 0x0f) {
        irq_raise(C64_IRQ_VIC, at);
    } else {
        c64m.cpu.irq &= ~C64_IRQ_VIC;
    }
}

static void raster_match(uint32_t at) {
    c64m.vic_irq |= 1;
    vic_irq_update(at);
}

/******************************************************************************/
// Sprite DMA model (vic-ii.txt 3.8.1, sprite crunch as in linusakesson.net
// "Lunatico" notes), for the cycles the DMA steals:
// - cycle 16: if the expansion flip-flop is set, MCBASE = MC; DMA off at 63;
// - cycle 55: flip-flop toggled (Y-expanded) or set; DMA on when Y matches;
// - cycle 58: MC = MCBASE, then MC += 3 (mod 64) while fetching;
// - clearing a $D017 bit in cycle 15 with the flip-flop reset (crunch):
//   MC = ((MC | MCBASE) & 0x15) | ((MC & MCBASE) & 0x2a).
// A sprite with DMA in line L steals cycles 55 + 2n .. 59 + 2n of L.

static void build_windows(void);

static void sprite_c16(void) {
    for (unsigned n = 0; n < 8; ++n) {
        uint8_t bit = 1u << n;
        if (c64m.sp_ff & bit) {
            c64m.sp_mcbase[n] = c64m.sp_mc[n];
            if (c64m.sp_mcbase[n] == 63) {
                c64m.sp_dma &= ~bit;
            }
        }
    }
}

// Cycles 55 and 58 of `line` (from the saved state before them), and this
// line's stall windows.
static void sprite_c55_c58(unsigned line, uint32_t start) {
    uint8_t yexp = c64m.vic[0x17], en = c64m.vic[0x15];
    if (!en && !c64m.sp55_dma) {
        // No sprite fetching or able to start: only the expansion flip-flops
        // move (they matter again when a sprite starts, which sets them).
        c64m.sp_dma = 0;
        c64m.sp_ff = (uint8_t)((c64m.sp55_ff ^ yexp) | ~yexp);
        memcpy(c64m.sp_mc, c64m.sp55_mcbase, 8);
        memcpy(c64m.sp_mcbase, c64m.sp55_mcbase, 8);
        c64m.spr_n = 0;
        build_windows();
        return;
    }
    c64m.sp_dma = c64m.sp55_dma;
    c64m.sp_ff = c64m.sp55_ff;
    memcpy(c64m.sp_mc, c64m.sp55_mc, 8);
    memcpy(c64m.sp_mcbase, c64m.sp55_mcbase, 8);
    for (unsigned n = 0; n < 8; ++n) {
        uint8_t bit = 1u << n;
        if (yexp & bit) {
            c64m.sp_ff ^= bit;
        } else {
            c64m.sp_ff |= bit;
        }
        if ((en & bit) && c64m.vic[1 + 2 * n] == (line & 0xff) && !(c64m.sp_dma & bit)) {
            c64m.sp_dma |= bit;
            c64m.sp_mcbase[n] = 0;
            if (yexp & bit) {
                c64m.sp_ff &= ~bit;
            }
        }
    }
    c64m.spr_n = 0;
    for (unsigned n = 0; n < 8; ++n) {
        c64m.sp_mc[n] = c64m.sp_mcbase[n];
        if (c64m.sp_dma & (1u << n)) {
            c64m.sp_mc[n] = (c64m.sp_mc[n] + 3) & 63;
            uint32_t a = start + 55 + 2 * n, b = start + 60 + 2 * n;
            if (c64m.spr_n && (int32_t)(a - c64m.spr_end[c64m.spr_n - 1]) <= 0) {
                c64m.spr_end[c64m.spr_n - 1] = b;
            } else {
                c64m.spr_start[c64m.spr_n] = a;
                c64m.spr_end[c64m.spr_n] = b;
                c64m.spr_n++;
            }
        }
    }
    build_windows();
}

// After cycle 16 and before 55 of the current line, a change of Y, $D015 or
// $D017 redoes the cycle-55/58 step.
static void sprite_regs_changed(uint32_t now) {
    uint32_t k = now - c64m.line_start;
    if (c64m.sp55_valid && k <= 55) {
        sprite_c55_c58(c64m.line, c64m.line_start);
        c64m.win_dirty = true;
    }
}

static void sprite_yexp_write(uint8_t old, uint8_t v, uint32_t now) {
    uint8_t cleared = old & ~v;
    uint32_t k = now - c64m.line_start;
    uint8_t *ff = c64m.sp55_valid ? &c64m.sp55_ff : &c64m.sp_ff;
    uint8_t *mc = c64m.sp55_valid ? c64m.sp55_mc : c64m.sp_mc;
    uint8_t *mcbase = c64m.sp55_valid ? c64m.sp55_mcbase : c64m.sp_mcbase;
    for (unsigned n = 0; n < 8; ++n) {
        uint8_t bit = 1u << n;
        if (!(cleared & bit)) {
            continue;
        }
        if (k == 15 && !(*ff & bit)) {
            mc[n] = ((mc[n] | mcbase[n]) & 0x15) | ((mc[n] & mcbase[n]) & 0x2a);
        }
        *ff |= bit;     // the flip-flop is set while the Y-expand bit is clear
    }
    sprite_regs_changed(now);
}

static uint8_t vic_read(uint8_t reg, uint32_t now) {
    reg &= 0x3f;
    switch (reg) {
        case 0x11: return (c64m.vic[0x11] & 0x7f) | ((current_line(now) & 0x100) >> 1);
        case 0x12: return current_line(now) & 0xff;
        case 0x16: return c64m.vic[0x16] | 0xc0;
        case 0x18: return c64m.vic[0x18] | 0x01;
        case 0x19: return c64m.vic_irq | ((c64m.vic_irq & c64m.vic[0x1a] & 0x0f) ? 0x80 : 0) | 0x70;
        case 0x1a: return c64m.vic[0x1a] | 0xf0;
        case 0x1e: return __atomic_exchange_n(&c64m.coll_ss, 0, __ATOMIC_SEQ_CST);
        case 0x1f: return __atomic_exchange_n(&c64m.coll_sb, 0, __ATOMIC_SEQ_CST);
        default:
            if (reg >= 0x20 && reg <= 0x2e) {
                return c64m.vic[reg] | 0xf0;
            }
            return reg >= 0x2f ? 0xff : c64m.vic[reg];
    }
}

static void vic_write(uint8_t reg, uint8_t v, uint32_t now) {
    reg &= 0x3f;
#ifdef C64_DEBUG_TRACE
    if ((int)c64m.frame == c64_trace_frame) {
        uint32_t k = now - c64m.line_start;
        printf("trace: line=%u cycle=%u reg=$%02x val=$%02x pc_after_opcode=$%04x\n", c64m.line + k / 63, k % 63, reg, v,
               trace_pc);
    }
#endif
    if (reg >= 0x2f) {
        return;
    }
    if (c64m.ring_enabled) {
        c64_ring_push(now, reg, v);
    }
    if (reg == 0x19) {
        c64m.vic_irq &= ~(v & 0x0f);    // acknowledge
        vic_irq_update(now);
        return;
    }
    uint8_t old_d017 = c64m.vic[0x17];
    c64m.vic[reg] = v;
    if (reg == 0x11 || reg == 0x12) {
        unsigned old = c64m.raster_cmp;
        c64m.raster_cmp = c64m.vic[0x12] | ((c64m.vic[0x11] & 0x80) << 1);
        // A new compare value equal to the current line matches at once.
        if (c64m.raster_cmp != old && c64m.raster_cmp == current_line(now)) {
            raster_match(now);
        }
        if (reg == 0x11 && c64m.line == 0x30 && (v & 0x10)) {
            c64m.den_seen = true;
        }
    } else if (reg == 0x1a) {
        vic_irq_update(now);
    }
    if (reg == 0x17) {
        sprite_yexp_write(old_d017, v, now);
    } else if ((reg < 0x10 && (reg & 1)) || reg == 0x15) {
        sprite_regs_changed(now);
    }
}

/******************************************************************************/
// Ring of VIC register writes to core 1 (single producer, single consumer).

void c64_ring_push(uint32_t cycle, uint8_t reg, uint8_t value) {
    uint32_t h = c64m.ring_head;
    while (h - c64m.ring_tail >= C64_VIC_RING) {
        // Full: core 1 is behind; wait for it.
    }
    c64m.ring[h & (C64_VIC_RING - 1)] = (c64_vic_write_t) { cycle, reg, value, 0 };
    C64_DMB();  // entry (and the RAM it describes) before the new head
    c64m.ring_head = h + 1;
}

void c64_ring_line(unsigned line, uint32_t start) {
    uint32_t h = c64m.ring_head;
    while (h - c64m.ring_tail >= C64_VIC_RING) {
    }
    c64m.ring[h & (C64_VIC_RING - 1)] = (c64_vic_write_t) { start, C64_RING_LINE, 0, (uint16_t)line };
    C64_DMB();
    c64m.ring_head = h + 1;
}

// Before a RAM write: log the old value (entry first, then the RAM write, so
// core 1 seeing the new value also sees the entry).
static void ahead_log(uint32_t addr, uint32_t now) {
    c64_ahead_t *a = &c64m.ahead[c64m.lines & 1];
    uint32_t n = a->n;
    if (n < C64_AHEAD_MAX) {
        uint32_t k = now - c64m.line_start;
        a->addr[n] = addr;
        a->cycle[n] = k < 255 ? (uint8_t)k : 255;
        a->old[n] = addr & 0x10000 ? c64m.color_ram[addr & 0x3ff] : c64m.ram[addr];
        unsigned h = c64_ahead_hash(addr);
        a->filter[h >> 5] |= 1u << (h & 31);
        C64_DMB();
        a->n = n + 1;
        C64_DMB();
    }
}

/******************************************************************************/
// I/O area $D000-$DFFF

static uint8_t io_read(uint16_t addr, uint32_t now) {
    switch ((addr >> 8) & 0xf) {
        case 0x0: case 0x1: case 0x2: case 0x3:
            return vic_read(addr, now);
        case 0x4: case 0x5: case 0x6: case 0x7:
            return sid_read(addr, now);
        case 0x8: case 0x9: case 0xa: case 0xb:
            return (c64m.color_ram[addr & 0x3ff] & 0x0f) | 0xf0;
        case 0xc: return cia_read(&c64m.cia1, addr, now);
        case 0xd: return cia_read(&c64m.cia2, addr, now);
        default: return 0xff;   // I/O 1 and 2: no cartridge
    }
}

static void io_write(uint16_t addr, uint8_t v, uint32_t now) {
    switch ((addr >> 8) & 0xf) {
        case 0x0: case 0x1: case 0x2: case 0x3: vic_write(addr, v, now); break;
        case 0x4: case 0x5: case 0x6: case 0x7: sid_write(addr, v, now); break;
        case 0x8: case 0x9: case 0xa: case 0xb:
            if (c64m.ring_enabled) {
                ahead_log(0x10000 | (addr & 0x3ff), now);
            }
            c64m.color_ram[addr & 0x3ff] = v & 0x0f;
            break;
        case 0xc: cia_write(&c64m.cia1, addr, v, now); break;
        case 0xd: {
            uint16_t bank = c64_vic_bank();
            cia_write(&c64m.cia2, addr, v, now);
            if (c64m.ring_enabled && c64_vic_bank() != bank) {
                c64_ring_push(now, C64_RING_BANK, c64_vic_bank() >> 14);
            }
            break;
        }
        default: break;
    }
}

/******************************************************************************/
// CPU bus

static inline void ba_stall(uint32_t *cyc) {
    if (*cyc - c64m.ba_start < c64m.ba_len) {
        c64m.stolen += c64m.ba_start + c64m.ba_len - *cyc;
        *cyc = c64m.ba_start + c64m.ba_len;
    }
}

static inline uint8_t bus_read(uint16_t addr, uint32_t *cyc) {
    ba_stall(cyc);
    const uint8_t *p = c64m.rd[addr >> 12];
    if (p) {
        return p[addr & 0xfff];
    }
    return io_read(addr, *cyc);
}

static inline void bus_write(uint16_t addr, uint8_t v, uint32_t cyc, uint32_t *until) {
    if ((addr >> 12) == 0xd && c64m.io_visible) {
        io_write(addr, v, cyc);
        // A CIA write may have moved the next event earlier; a sprite register
        // write may have changed the stall windows.
        schedule(cyc);
        if ((int32_t)(c64m.next_event - *until) < 0) {
            *until = c64m.next_event;
        }
        if (c64m.win_dirty) {
            *until = cyc + 1;
        }
        return;
    }
    if (c64m.ring_enabled) {
        ahead_log(addr, cyc);
    }
    c64m.ram[addr] = v;
    if (addr < 2) {
        if (addr == 0) {
            c64m.port_ddr = v;
        } else {
            c64m.port_data = v;
        }
        map_update();
    }
}

#define C6510_FN cpu6510_run_c64
#define C6510_READ(addr) bus_read(addr, &cyc)
#define C6510_WRITE(addr, v) do { TRACE_PC(pc); bus_write(addr, v, cyc, &until); } while (0)
/* Dummy reads are real bus reads: indexed stores can acknowledge CIA ICR
 * at the uncorrected address. Omitting that read traps Billiards in IRQ. */
#define C6510_DUMMY(addr) ((void)bus_read((addr), &cyc))
#include "cpu6510_impl.h"

/******************************************************************************/
// Machine

// Power-on RAM pattern (as on many C64s: blocks of $00 and $ff); colour RAM 0.
static void power_on_ram(void) {
    for (unsigned i = 0; i < sizeof(c64m.ram); ++i) {
        c64m.ram[i] = (i & 0x40) ? 0xff : 0x00;
    }
    memset(c64m.color_ram, 0, sizeof(c64m.color_ram));
}

void c64_power_cycle(void) {
    power_on_ram();
    c64_reset();
}

void c64_init(const uint8_t *basic, const uint8_t *kernal, const uint8_t *chargen) {
    memset(&c64m, 0, sizeof(c64m));
    memcpy(c64m.basic, basic, sizeof(c64m.basic));
    memcpy(c64m.kernal, kernal, sizeof(c64m.kernal));
    memcpy(c64m.chargen, chargen, sizeof(c64m.chargen));
    power_on_ram();
    c64m.cia2.is_cia2 = true;
    c64_reset();
}

void c64_cart(const uint8_t *roml, const uint8_t *romh, bool game, bool exrom) {
    c64m.cart_roml = roml;
    c64m.cart_romh = romh;
    c64m.cart_game = game;
    c64m.cart_exrom = exrom;
    map_update();
}

void c64_reset(void) {
    uint32_t now = c64m.cpu.cycles;
    c64_cia_t *cias[2] = { &c64m.cia1, &c64m.cia2 };
    for (unsigned i = 0; i < 2; ++i) {
        bool is2 = cias[i]->is_cia2;
        memset(cias[i], 0, sizeof(c64_cia_t));
        cias[i]->is_cia2 = is2;
        cias[i]->latch_a = cias[i]->latch_b = 0xffff;
        cias[i]->cnt_a = cias[i]->cnt_b = 0xffff;
    }
    // The serial bus RESET line resets the drive too.
    c1541_reset(now);
    memset(c64m.vic, 0, sizeof(c64m.vic));
    if (c64m.ring_enabled) {
        // The renderer keeps its own copy of the registers: reset it too
        // (a program may rely on the power-on values of registers it never writes).
        for (uint8_t r = 0; r < 0x2f; ++r) {
            c64_ring_push(now, r, 0);
        }
        // And its VIC bank: the CIAs were cleared without a CIA2 write, so a
        // bank chosen by the program would stay (the KERNAL's own $DD00 write
        // then sees no change to report).
        c64_ring_push(now, C64_RING_BANK, c64_vic_bank() >> 14);
    }
    sid_reset();
    c64m.vic_irq = 0;
    c64m.sp_dma = 0;
    c64m.sp_ff = 0xff;
    memset(c64m.sp_mc, 0, sizeof(c64m.sp_mc));
    memset(c64m.sp_mcbase, 0, sizeof(c64m.sp_mcbase));
    c64m.sp55_valid = false;
    c64m.spr_n = c64m.spr_prev_n = 0;
    c64m.bad_start = c64m.bad_end = 0;
    c64m.win_n = 0;
    c64m.ba_len = 0;
    c64m.raster_cmp = 0;
    c64m.port_ddr = 0;
    c64m.port_data = 0;
    map_update();

    cpu6510_t *c = &c64m.cpu;
    uint32_t cycles = now;
    memset(c, 0, sizeof(*c));
    c->cycles = cycles + 7;  // reset sequence
    c->s = 0xfd;
    cpu6510_set_p(c, 0x24);
    c->pc = c64m.rd[0xf][0xffc] | (c64m.rd[0xf][0xffd] << 8);
    c64m.line = 0;
    c64m.line_start = cycles;
    schedule(c->cycles);
}

// Stall windows: add in increasing order; overlapping ones are merged.
static void win_add(uint32_t start, uint32_t end) {
    unsigned n = c64m.win_n;
    if (n && (int32_t)(start - c64m.win_end[n - 1]) <= 0) {
        if ((int32_t)(end - c64m.win_end[n - 1]) > 0) {
            c64m.win_end[n - 1] = end;
        }
        return;
    }
    if (n < 24) {
        c64m.win_start[n] = start;
        c64m.win_end[n] = end;
        c64m.win_n = n + 1;
    }
}

// The windows of the current line, in order: sprite DMA of the previous line
// (cycles 0..10), badline (12..54), sprite DMA of this line (55..).
static void build_windows(void) {
    c64m.win_n = 0;
    for (unsigned i = 0; i < c64m.spr_prev_n; ++i) {
        win_add(c64m.spr_prev_start[i], c64m.spr_prev_end[i]);
    }
    if (c64m.bad_end != c64m.bad_start) {
        win_add(c64m.bad_start, c64m.bad_end);
    }
    for (unsigned i = 0; i < c64m.spr_n; ++i) {
        win_add(c64m.spr_start[i], c64m.spr_end[i]);
    }
}

// Run the CPU (and the CIA events) until cycle `end`.
static void run_until(uint32_t end) {
    while ((int32_t)(c64m.cpu.cycles - end) < 0) {
        uint32_t now = c64m.cpu.cycles;
        uint32_t until = (int32_t)(c64m.next_event - end) < 0 ? c64m.next_event : end;
        // The first window that has not ended: the bus hooks stall in it; stop
        // at its end so the next one can be loaded.
        c64m.ba_len = 0;
        for (unsigned i = 0; i < c64m.win_n; ++i) {
            if ((int32_t)(c64m.win_end[i] - now) > 0) {
                c64m.ba_start = c64m.win_start[i];
                c64m.ba_len = c64m.win_end[i] - c64m.win_start[i];
                if ((int32_t)(c64m.win_end[i] - until) < 0) {
                    until = c64m.win_end[i];
                }
                break;
            }
        }
        if ((int32_t)(until - now) <= 0) {
            until = now + 1;
        }
        c64m.win_dirty = false;
        cpu6510_run_c64(&c64m.cpu, until);
        now = c64m.cpu.cycles;
        if ((int32_t)(now - c64m.next_event) >= 0) {
            cia_update(&c64m.cia1, now);
            cia_update(&c64m.cia2, now);
            schedule(now);
        }
    }
}

void c64_run_line(void) {
    unsigned line = c64m.line;
    uint32_t start = c64m.line_start;

    // A new log for this line (core 1 is at most one line behind: the slot of
    // two lines ago is no longer read).
    c64_ahead_t *a = &c64m.ahead[c64m.lines & 1];
    a->n = 0;
    memset((void *)a->filter, 0, sizeof(a->filter));
    C64_DMB();
    a->line = c64m.lines;
    C64_DMB();

    uint8_t irq = __atomic_exchange_n(&c64m.coll_irq, 0, __ATOMIC_SEQ_CST);
    if (irq) {
        c64m.vic_irq |= irq;
        vic_irq_update(start);
    }
    if (line == 0x30) {
        c64m.den_seen = c64m.vic[0x11] & 0x10;
    }
    // Windows: last line's sprite DMA spills into this one; badline: BA low
    // from cycle 12, the CPU's reads stall until cycle 55.
    memcpy(c64m.spr_prev_start, c64m.spr_start, sizeof(c64m.spr_start));
    memcpy(c64m.spr_prev_end, c64m.spr_end, sizeof(c64m.spr_end));
    c64m.spr_prev_n = c64m.spr_n;
    c64m.spr_n = 0;
    c64m.bad_start = c64m.bad_end = 0;
    if (c64m.den_seen && line >= 0x30 && line <= 0xf7 && (line & 7) == (c64m.vic[0x11] & 7)) {
        c64m.bad_start = start + 12;
        c64m.bad_end = start + 55;
    }
    // Provisional sprite windows of this line from the current state: on a
    // badline the CPU stalls from cycle 12 to 55, past the cycle-16 step below,
    // and its reads from 55 on must already see the sprite DMA. Elsewhere the
    // CPU reaches cycle 16 first (last line's sprite windows end by cycle 10),
    // and with no sprite enabled or fetching there are no windows to see.
    if (c64m.bad_end == c64m.bad_start || !(c64m.sp_dma | c64m.vic[0x15])) {
        build_windows();
    } else {
        uint8_t dma = c64m.sp_dma, ff = c64m.sp_ff, mc[8], mcbase[8];
        memcpy(mc, c64m.sp_mc, 8);
        memcpy(mcbase, c64m.sp_mcbase, 8);
        sprite_c16();
        c64m.sp55_dma = c64m.sp_dma;
        c64m.sp55_ff = c64m.sp_ff;
        memcpy(c64m.sp55_mc, c64m.sp_mc, 8);
        memcpy(c64m.sp55_mcbase, c64m.sp_mcbase, 8);
        sprite_c55_c58(line, start);
        c64m.sp_dma = dma;
        c64m.sp_ff = ff;
        memcpy(c64m.sp_mc, mc, 8);
        memcpy(c64m.sp_mcbase, mcbase, 8);
    }
    c64m.sp55_valid = false;

    // Raster compare in cycle 1 (cycle 2 in line 0).
    run_until(start + (line == 0 ? 2 : 1));
    if (line == c64m.raster_cmp) {
        raster_match(start + (line == 0 ? 2 : 1));
    }
    // Sprites: cycle 16, then the cycle-55/58 step (redone on later changes).
    run_until(start + 16);
    sprite_c16();
    c64m.sp55_dma = c64m.sp_dma;
    c64m.sp55_ff = c64m.sp_ff;
    memcpy(c64m.sp55_mc, c64m.sp_mc, 8);
    memcpy(c64m.sp55_mcbase, c64m.sp_mcbase, 8);
    c64m.sp55_valid = true;
    sprite_c55_c58(line, start);
    run_until(start + C64_CYCLES_PER_LINE);
    c64m.sp55_valid = false;
    c1541_run_to(start + C64_CYCLES_PER_LINE);

    c64m.line_start = start + C64_CYCLES_PER_LINE;
    c64m.lines++;
    if (++c64m.line == C64_LINES) {
        c64m.line = 0;
        c64m.frame++;
    }
}

void c64_run_frame(void) {
    do {
        c64_run_line();
    } while (c64m.line != 0);
}
