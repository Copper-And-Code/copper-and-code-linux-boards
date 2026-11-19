// Commodore 1541: see c1541.h.

#include "c1541.h"

#include <string.h>

#include "cpu6510.h"

/******************************************************************************/
// VIA 6522 (the parts the 1541 uses: ports, CA1/CA2/CB2, timers 1 and 2, IFR/IER)

enum { IFR_CA2 = 0x01, IFR_CA1 = 0x02, IFR_SR = 0x04, IFR_CB2 = 0x08, IFR_CB1 = 0x10, IFR_T2 = 0x20, IFR_T1 = 0x40 };

#define NEVER (0x7fffffffu)
#define TRACK_MAX (7936)        // GCR bytes of the longest track (7692 at speed 3), rounded up

typedef struct {
    uint8_t orb, ora, ddrb, ddra;
    uint8_t acr, pcr, ifr, ier, sr;
    uint8_t ca1;                // CA1 input level
    // Timer 1: counter `t1` at cycle `t1_base`, then -1 per cycle; at 0 it
    // shows $FFFF for a cycle, then the latch (period latch + 2). Its interrupt
    // comes at each $FFFF (one-shot mode: once per T1C-H write).
    uint16_t t1_latch, t1;
    uint32_t t1_base, t1_next;  // t1_next: cycle of the next interrupt, if armed
    bool t1_armed;
    // Timer 2 (interval mode): counter `t2` at `t2_base`, -1 per cycle; one
    // interrupt when it passes 0.
    uint8_t t2_latch_lo;
    uint16_t t2;
    uint32_t t2_base, t2_next;
    bool t2_armed;
} via_t;

static uint16_t t1_value(const via_t *v, uint32_t now) {
    int32_t e = (int32_t)(now - v->t1_base);
    if (e <= 0) {
        return v->t1;
    }
    if ((uint32_t)e <= v->t1) {
        return (uint16_t)(v->t1 - e);
    }
    uint32_t r = ((uint32_t)e - v->t1 - 1) % ((uint32_t)v->t1_latch + 2);
    return r == 0 ? 0xffff : (uint16_t)(v->t1_latch - (r - 1));
}

static uint16_t t2_value(const via_t *v, uint32_t now) {
    int32_t e = (int32_t)(now - v->t2_base);
    return e <= 0 ? v->t2 : (uint16_t)(v->t2 - e);
}

// Interrupts due up to `now`; *at becomes the earliest new one (callers set it
// to `now` first).
static bool via_events(via_t *v, uint32_t now, uint32_t *at) {
    bool any = false;
    if (v->t1_armed && (int32_t)(now - v->t1_next) >= 0) {
        if (!(v->ifr & IFR_T1)) {
            if ((int32_t)(v->t1_next - *at) < 0) {
                *at = v->t1_next;
            }
            any = true;
        }
        v->ifr |= IFR_T1;
        if (v->acr & 0x40) {    // free running
            uint32_t period = (uint32_t)v->t1_latch + 2;
            uint32_t n = (now - v->t1_next) / period + 1;
            v->t1_next += n * period;
        } else {
            v->t1_armed = false;
        }
    }
    if (v->t2_armed && (int32_t)(now - v->t2_next) >= 0) {
        if (!(v->ifr & IFR_T2)) {
            if ((int32_t)(v->t2_next - *at) < 0) {
                *at = v->t2_next;
            }
            any = true;
        }
        v->ifr |= IFR_T2;
        v->t2_armed = false;
    }
    return any;
}

static uint32_t via_next(const via_t *v) {
    uint32_t n = NEVER;
    if (v->t1_armed) {
        n = v->t1_next;
    }
    if (v->t2_armed && (n == NEVER || (int32_t)(v->t2_next - n) < 0)) {
        n = v->t2_next;
    }
    return n;
}

static bool via_irq(const via_t *v) {
    return (v->ifr & v->ier & 0x7f) != 0;
}

// CA1 input; the active edge is set by PCR bit 0 (1: rising).
static void via_set_ca1(via_t *v, uint8_t level) {
    if (level != v->ca1) {
        if ((level && (v->pcr & 1)) || (!level && !(v->pcr & 1))) {
            v->ifr |= IFR_CA1;
        }
        v->ca1 = level;
    }
}

static void via_reset(via_t *v) {
    memset(v, 0, sizeof(*v));
    v->t1_latch = v->t1 = 0xffff;
    v->t2 = 0xffff;
}

static uint8_t via_read(via_t *v, uint8_t reg, uint32_t now, uint8_t pb_pins, uint8_t pa_pins) {
    uint32_t at = now;
    via_events(v, now, &at);
    switch (reg & 15) {
        case 0x0:
            v->ifr &= ~(IFR_CB1 | IFR_CB2);
            return (v->orb & v->ddrb) | (pb_pins & ~v->ddrb);
        case 0x1:
            v->ifr &= ~(IFR_CA1 | IFR_CA2);
            return (v->ora & v->ddra) | (pa_pins & ~v->ddra);
        case 0xf:
            return (v->ora & v->ddra) | (pa_pins & ~v->ddra);
        case 0x2: return v->ddrb;
        case 0x3: return v->ddra;
        case 0x4:
            v->ifr &= ~IFR_T1;
            return (uint8_t)t1_value(v, now);
        case 0x5: return (uint8_t)(t1_value(v, now) >> 8);
        case 0x6: return (uint8_t)v->t1_latch;
        case 0x7: return (uint8_t)(v->t1_latch >> 8);
        case 0x8:
            v->ifr &= ~IFR_T2;
            return (uint8_t)t2_value(v, now);
        case 0x9: return (uint8_t)(t2_value(v, now) >> 8);
        case 0xa: return v->sr;
        case 0xb: return v->acr;
        case 0xc: return v->pcr;
        case 0xd: return (v->ifr & 0x7f) | (via_irq(v) ? 0x80 : 0);
        default: return v->ier | 0x80;
    }
}

static void via_write(via_t *v, uint8_t reg, uint8_t val, uint32_t now) {
    uint32_t at = now;
    via_events(v, now, &at);
    switch (reg & 15) {
        case 0x0:
            v->ifr &= ~(IFR_CB1 | IFR_CB2);
            v->orb = val;
            break;
        case 0x1:
            v->ifr &= ~(IFR_CA1 | IFR_CA2);
            v->ora = val;
            break;
        case 0xf: v->ora = val; break;
        case 0x2: v->ddrb = val; break;
        case 0x3: v->ddra = val; break;
        case 0x4: case 0x6:
            v->t1_latch = (v->t1_latch & 0xff00) | val;
            break;
        case 0x5:
            // Load the counter from the latch (one cycle later) and start.
            v->t1_latch = (uint16_t)((v->t1_latch & 0x00ff) | (val << 8));
            v->ifr &= ~IFR_T1;
            v->t1 = v->t1_latch;
            v->t1_base = now + 1;
            v->t1_next = v->t1_base + v->t1 + 1;
            v->t1_armed = true;
            break;
        case 0x7:
            v->t1_latch = (uint16_t)((v->t1_latch & 0x00ff) | (val << 8));
            v->ifr &= ~IFR_T1;
            break;
        case 0x8: v->t2_latch_lo = val; break;
        case 0x9:
            v->t2 = (uint16_t)(v->t2_latch_lo | (val << 8));
            v->ifr &= ~IFR_T2;
            v->t2_base = now + 1;
            v->t2_next = v->t2_base + v->t2 + 1;
            v->t2_armed = true;
            break;
        case 0xa: v->sr = val; break;
        case 0xb:
            // Switching T1 between one-shot and free-running keeps its schedule.
            v->acr = val;
            break;
        case 0xc: v->pcr = val; break;
        case 0xd: v->ifr &= ~(val & 0x7f); break;
        default:
            if (val & 0x80) {
                v->ier |= val & 0x7f;
            } else {
                v->ier &= ~(val & 0x7f);
            }
            break;
    }
}

/******************************************************************************/
// Disk: GCR tracks from a D64 image

static const uint8_t gcr5[16] = {
    0x0a, 0x0b, 0x12, 0x13, 0x0e, 0x0f, 0x16, 0x17, 0x09, 0x19, 0x1a, 0x1b, 0x0d, 0x1d, 0x1e, 0x15,
};

static unsigned sectors_of(unsigned track) {
    return track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17;
}

// Track length in bytes at 300 rpm for the standard speed zone of the track.
static unsigned track_bytes(unsigned track) {
    return track <= 17 ? 7692 : track <= 24 ? 7142 : track <= 30 ? 6666 : 6250;
}

// Cycles per byte for density setting d (VIA 2 PB5-6): 32, 30, 28, 26.
static unsigned byte_cycles(unsigned d) {
    return 32 - 2 * d;
}

static struct {
    cpu6510_t cpu;
    uint8_t ram[0x800];
    const uint8_t *rom;
    via_t via1, via2;
    bool powered;
    bool idle_trap;             // DOS 2.6 idle loop recognised in the ROM
    bool idle_hit;              // the CPU is parked at the idle loop start

    // Clock: drive cycles for C64 cycles, exactly 1 000 000 / 985 248.
    uint32_t c64_ref, drive_ref, rem;
    // The drive runs whole instructions, so it may stop a few cycles past the
    // C64's time: a bus write it made after that time is not seen yet (the
    // port as it was before, and when it changed).
    uint32_t synced_to;         // drive cycle the C64 last synchronised to
    uint32_t port_changed_at;   // drive cycle of the last VIA 1 port B change
    uint8_t port_before;        // VIA 1 port B outputs before it

    // Bus: lines pulled low by the C64 (1 ATN, 2 CLK, 4 DATA).
    uint8_t c64_low;

    // Mechanics.
    const uint8_t *d64;         // image in memory, or NULL with `reader`
    c1541_read_t reader;        // image read through a function (SD card)
    unsigned tracks;            // in the image (0: no disk)
    uint32_t err_base;          // offset of the error table (one byte per sector; 0: none)
    uint8_t disk_id1, disk_id2; // from the BAM, read at insertion
    int cache_track;            // track in track_cache (reader images; -1: none)
    uint8_t track_cache[21 * 256];
    uint8_t half_track;         // 2..84 (2 = track 1)
    uint8_t phase;              // stepper phase written last
    bool motor;
    uint32_t next_byte;         // drive cycle of the next byte under the head
    unsigned pos;               // byte position in the track
    uint8_t prev_byte, cur_byte;
    bool sync;
    uint8_t latch;              // last byte read (VIA 2 port A)

    // Track under the head, GCR-encoded byte by byte (no track buffer: RAM is
    // scarce on the board). Per sector: sync, header, gap, sync, data, gap.
    int gcr_track;              // -1: none
    unsigned gcr_len;           // bytes per revolution
    unsigned sectors, sector_len;
    uint8_t id1, id2;
    uint8_t err[21];            // error code of each sector of this track (1: none)
    int cache_sector;           // data block checksum of this sector
    uint8_t cache_chk;
    int cache_group;            // 5 GCR bytes of this group (sector * 128 + group)
    uint8_t cache_gcr[5];

    // Writing: the bytes the DOS puts under the head while VIA 2 CB2 is low
    // (write mode) go into a copy of the track, read back from there at once
    // (formatting measures the track that way), and turn back into sectors
    // when the head leaves the track, the motor stops or the disk goes out.
    c1541_write_t writer;       // NULL: write protected
    bool writing, track_written;
    uint8_t wtrack[TRACK_MAX];
    uint8_t wmask[TRACK_MAX / 8];   // positions written
    uint32_t wps_until;         // drive cycle: the write-protect sensor reads covered until then
} d;

static uint32_t sector_offset(unsigned track, unsigned sector) {
    unsigned off = 0;
    for (unsigned t = 1; t < track; ++t) {
        off += sectors_of(t);
    }
    return (off + sector) * 256u;
}

// Sector data: from the image in memory, or from the cached track of an image
// read through a function (the whole track is read when the head arrives).
static const uint8_t *sector_data(unsigned track, unsigned sector) {
    if (d.d64) {
        return d.d64 + sector_offset(track, sector);
    }
    if (d.cache_track != (int)track) {
        if (!d.reader || !d.reader(sector_offset(track, 0), d.track_cache, sectors_of(track) * 256u)) {
            memset(d.track_cache, 0, sizeof(d.track_cache));
        }
        d.cache_track = (int)track;
    }
    return d.track_cache + sector * 256u;
}

static void gcr_put(uint8_t *o, const uint8_t in[4]) {
    uint64_t bits = 0;
    for (unsigned i = 0; i < 4; ++i) {
        bits = (bits << 10) | ((uint64_t)gcr5[in[i] >> 4] << 5) | gcr5[in[i] & 15];
    }
    for (int i = 4; i >= 0; --i) {
        *o++ = (uint8_t)(bits >> (8 * i));
    }
}

#define SECTOR_BYTES (354)      // 5 sync + 10 header + 9 gap + 5 sync + 325 data

static void build_track(int track) {
    d.gcr_track = track;
    d.gcr_len = track >= 1 ? track_bytes((unsigned)track) : 6250;
    d.cache_sector = d.cache_group = -1;
    if (track < 1 || (unsigned)track > d.tracks) {
        d.sectors = 0;      // no disk / unformatted: no sync, no data
        return;
    }
    d.id1 = d.disk_id1;
    d.id2 = d.disk_id2;
    d.sectors = sectors_of((unsigned)track);
    d.sector_len = SECTOR_BYTES + (d.gcr_len - d.sectors * SECTOR_BYTES) / d.sectors;
    // Error table of images that have one (copy protections check that a
    // sector reads with an error).
    memset(d.err, 1, sizeof(d.err));
    if (d.err_base) {
        uint32_t off = d.err_base + sector_offset((unsigned)track, 0) / 256u;
        if (d.d64) {
            memcpy(d.err, d.d64 + off, d.sectors);
        } else if (!d.reader || !d.reader(off, d.err, d.sectors)) {
            memset(d.err, 1, sizeof(d.err));
        }
    }
}

// GCR byte at position `pos` of the current track.
// The GCR byte at pos made from the image's sectors.
static uint8_t gcr_image(unsigned pos) {
    if (!d.sectors) {
        return 0x00;
    }
    unsigned s = pos / d.sector_len, o = pos % d.sector_len;
    if (s >= d.sectors || o >= SECTOR_BYTES) {
        return 0x55;            // gaps
    }
    // D64 error codes (as VICE writes them into the track): 2 = 20 header not
    // found, 3 = 21 no sync, 4 = 22 data block not found, 5 = 23 data checksum,
    // 8 = 27 header checksum, 10/11 = 29 disk ID mismatch.
    uint8_t err = d.err[s];
    if (o < 5 || (o >= 24 && o < 29)) {
        return err == 3 ? 0x55 : 0xff;      // sync
    }
    if (o >= 15 && o < 24) {
        return 0x55;            // header gap
    }
    unsigned track = (unsigned)d.gcr_track;
    int group;
    uint8_t in[4];
    if (o < 15) {               // header: 2 groups
        group = (int)(s * 128 + (o - 5) / 5);
        if (group != d.cache_group) {
            uint8_t id1 = err == 10 || err == 11 ? (uint8_t)~d.id1 : d.id1;
            uint8_t h[8] = { err == 2 ? 0x00 : 0x08, (uint8_t)(s ^ track ^ d.id2 ^ id1), (uint8_t)s, (uint8_t)track,
                             d.id2, id1, 0x0f, 0x0f };
            if (err == 8) {
                h[1] ^= 0xff;
            }
            memcpy(in, h + 4 * ((o - 5) / 5), 4);
            gcr_put(d.cache_gcr, in);
            d.cache_group = group;
        }
        return d.cache_gcr[(o - 5) % 5];
    }
    // Data block: $07, 256 bytes, checksum, $00 $00 (65 groups).
    unsigned k = o - 29, g = k / 5;
    group = (int)(s * 128 + 2 + g);
    if (group != d.cache_group) {
        const uint8_t *data = sector_data(track, s);
        if ((int)s != d.cache_sector) {
            uint8_t chk = 0;
            for (unsigned i = 0; i < 256; ++i) {
                chk ^= data[i];
            }
            d.cache_chk = err == 5 ? (uint8_t)~chk : chk;
            d.cache_sector = (int)s;
        }
        for (unsigned i = 0; i < 4; ++i) {
            unsigned j = g * 4 + i;     // index in the 260-byte block
            in[i] = j == 0 ? (err == 4 ? 0x00 : 0x07) : j <= 256 ? data[j - 1] : j == 257 ? d.cache_chk : 0x00;
        }
        gcr_put(d.cache_gcr, in);
        d.cache_group = group;
    }
    return d.cache_gcr[k % 5];
}

static bool written(unsigned pos) {
    return d.wmask[pos >> 3] & (1u << (pos & 7));
}

// The byte under the head: what was written on this track, else the image.
static uint8_t gcr_byte(unsigned pos) {
    return d.track_written && written(pos) ? d.wtrack[pos] : gcr_image(pos);
}

static void commit_write(void);

static void head_moved(void) {
    int track = d.half_track / 2;     // odd half tracks read as the track below
    if (track != d.gcr_track) {
        commit_write();
        unsigned old_len = d.gcr_len ? d.gcr_len : 1;
        build_track(track);
        d.pos = (unsigned)((uint64_t)d.pos * d.gcr_len / old_len) % d.gcr_len;
    }
}

/******************************************************************************/
// Bus

static uint8_t drive_low_of(uint8_t out) {
    // VIA 1 PB1 DATA OUT, PB3 CLK OUT (through inverters: 1 pulls the line
    // low), PB4 ATNA: DATA is also pulled low while ATN differs from ATNA.
    uint8_t low = 0;
    if (out & 0x02) {
        low |= 4;
    }
    if (out & 0x08) {
        low |= 2;
    }
    if (((d.c64_low & 1) != 0) != ((out & 0x10) != 0)) {
        low |= 4;
    }
    return low;
}

static uint8_t drive_low(void) {
    return drive_low_of(d.via1.orb & d.via1.ddrb);
}

static uint8_t bus_low(void) {
    return d.powered ? (uint8_t)(d.c64_low | drive_low()) : d.c64_low;
}

uint8_t c1541_bus_c64_in(void) {
    uint8_t low = bus_low();
    if (d.powered && (int32_t)(d.port_changed_at - d.synced_to) > 0) {
        low = (uint8_t)(d.c64_low | drive_low_of(d.port_before));
    }
    return (uint8_t)(((low & 2) ? 0 : 0x40) | ((low & 4) ? 0 : 0x80));
}

/******************************************************************************/
// Drive CPU and memory

static void irq_update(uint32_t at) {
    bool on = via_irq(&d.via1) || via_irq(&d.via2);
    if (on && !d.cpu.irq) {
        d.cpu.irq_since = at;
    }
    d.cpu.irq = on;
}

static uint8_t via1_pb_pins(void) {
    uint8_t low = bus_low();
    // PB0 DATA IN, PB2 CLK IN, PB7 ATN IN (1 = line low); PB5-6 device
    // number jumpers (0: device 8); unconnected inputs read 1.
    return (uint8_t)(0x1a | ((low & 4) ? 0x01 : 0) | ((low & 2) ? 0x04 : 0) | ((low & 1) ? 0x80 : 0));
}

static uint8_t via2_pb_pins(uint32_t now) {
    // PB4 write protect (0: protected, or the sensor covered while a disk goes
    // in or out: that change is how the DOS notices a new disk), PB7 SYNC (0:
    // in a sync mark).
    bool wp = !d.writer || !d.tracks || (int32_t)(now - d.wps_until) < 0;
    return (uint8_t)(0x6f | (wp ? 0 : 0x10) | ((d.motor && d.sync && !d.writing) ? 0 : 0x80));
}

// DOS 2.6 idle loop: $EBFF CLI ... $EC9B JMP $EBFF. With no ATN flag ($7C),
// no error blinking ($026C) and no interrupt pending, each pass leaves the
// drive as it found it: from $EBFF the drive can skip to its next event (a VIA
// interrupt, or the cycle it is being run to), as VICE's idle trap does.
#define IDLE_LOOP (0xebff)
#define IDLE_JMP (0xec9b)

static uint8_t drive_read(uint16_t addr, uint32_t now, uint32_t *until) {
    if (addr & 0x8000) {
        if (addr == IDLE_JMP && d.idle_trap) {
            d.idle_hit = true;      // stop after this JMP: run_drive() decides
            *until = now;
        }
        return d.rom[addr & 0x3fff];
    }
    switch (addr & 0x1c00) {
        case 0x1800: {
            uint8_t v = via_read(&d.via1, (uint8_t)addr, now, via1_pb_pins(), 0xff);
            irq_update(now);
            return v;
        }
        case 0x1c00: {
            uint8_t v = via_read(&d.via2, (uint8_t)addr, now, via2_pb_pins(now), d.latch);
            irq_update(now);
            return v;
        }
        default:
            if ((addr & 0x1fff) < 0x0800) {
                return d.ram[addr & 0x7ff];
            }
            return (uint8_t)(addr >> 8);    // open bus
    }
}

// GCR 5 bits -> 4 bits (0xff: not a GCR code).
static uint8_t gcr_nibble(unsigned code) {
    for (unsigned i = 0; i < 16; ++i) {
        if (gcr5[i] == code) {
            return (uint8_t)i;
        }
    }
    return 0xff;
}

// 5 GCR bytes at in -> 4 bytes; false if a code is invalid.
static bool gcr_get(const uint8_t *in, uint8_t out[4]) {
    uint64_t bits = 0;
    for (unsigned i = 0; i < 5; ++i) {
        bits = (bits << 8) | in[i];
    }
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t hi = gcr_nibble((unsigned)(bits >> (35 - 10 * i)) & 31);
        uint8_t lo = gcr_nibble((unsigned)(bits >> (30 - 10 * i)) & 31);
        if (hi == 0xff || lo == 0xff) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// A written track back into sectors: the whole track (written bytes, the
// image's elsewhere) is decoded from its syncs; each data block ($07) that
// was written goes to the sector named by the header block ($08) before it.
static void commit_write(void) {
    if (!d.track_written) {
        return;
    }
    d.track_written = false;
    int track = d.gcr_track;
    unsigned n = d.gcr_len;
    if (!d.writer || track < 1 || (unsigned)track > d.tracks || !d.sectors || n > TRACK_MAX) {
        memset(d.wmask, 0, sizeof(d.wmask));
        return;
    }
    // The image's bytes where nothing was written (before the cache changes).
    for (unsigned i = 0; i < n; ++i) {
        if (!written(i)) {
            d.wtrack[i] = gcr_image(i);
        }
    }
    // Start at the sync of a header block ($52, the first GCR byte of $08),
    // else of any block: no data block is then parted from its header at the
    // end of the pass.
    unsigned start = n;
    for (unsigned pass = 0; pass < 2 && start == n; ++pass) {
        for (unsigned p = 0; p < n; ++p) {
            if (d.wtrack[p] != 0xff || d.wtrack[(p + n - 1) % n] == 0xff) {
                continue;
            }
            unsigned q = p;
            while (d.wtrack[q % n] == 0xff && q < p + n) {
                ++q;
            }
            if (pass == 1 || d.wtrack[q % n] == 0x52) {
                start = p;
                break;
            }
        }
    }
    if (start == n) {
        memset(d.wmask, 0, sizeof(d.wmask));
        return;
    }
    int header_sector = -1;
    unsigned i = 0;
    while (i < n) {
        if (d.wtrack[(start + i) % n] != 0xff) {
            ++i;
            continue;
        }
        while (i < 2 * n && d.wtrack[(start + i) % n] == 0xff) {
            ++i;
        }
        uint8_t raw[325], b[260];
        unsigned len = d.wtrack[(start + i) % n] == 0x52 ? 10 : 325;   // GCR of $08 starts $52, of $07 $55
        bool touched = false;
        for (unsigned k = 0; k < len; ++k) {
            unsigned at = (start + i + k) % n;
            raw[k] = d.wtrack[at];
            touched |= written(at);
        }
        bool ok = gcr_get(raw, b);
        if (ok && b[0] == 0x08 && len == 10 && gcr_get(raw + 5, b + 4)) {
            header_sector = b[3] == track && b[2] < d.sectors ? b[2] : -1;
            if (header_sector >= 0 && touched) {
                d.id2 = d.disk_id2 = b[4];
                d.id1 = d.disk_id1 = b[5];
            }
            i += 10;
            continue;
        }
        if (ok && b[0] == 0x07 && len == 325) {
            for (unsigned g = 1; g < 65 && ok; ++g) {
                ok = gcr_get(raw + 5 * g, b + 4 * g);
            }
            if (ok && touched && header_sector >= 0) {
                d.writer(sector_offset((unsigned)track, (unsigned)header_sector), b + 1, 256);
                if (d.cache_track == track) {
                    memcpy(d.track_cache + (unsigned)header_sector * 256u, b + 1, 256);
                }
            }
            header_sector = -1;
            i += 325;
            continue;
        }
        header_sector = -1;
        ++i;
    }
    memset(d.wmask, 0, sizeof(d.wmask));
    // The GCR of this track is made again from the new data.
    d.cache_sector = d.cache_group = -1;
}

static void motor_and_head(uint32_t now) {
    uint8_t out = d.via2.orb & d.via2.ddrb;     // inputs drive nothing
    bool motor = out & 0x04;
    if (motor && !d.motor) {
        d.next_byte = now + byte_cycles((out >> 5) & 3);
    }
    if (!motor && d.motor) {
        commit_write();
    }
    d.motor = motor;
    // Stepper: phase +1 steps in (towards higher tracks), -1 out.
    uint8_t ph = out & 3;
    if (ph != d.phase) {
        commit_write();
        if (ph == ((d.phase + 1) & 3) && d.half_track < 84) {
            d.half_track++;
        } else if (ph == ((d.phase - 1) & 3) && d.half_track > 2) {
            d.half_track--;
        }
        d.phase = ph;
        head_moved();
    }
}

static void drive_write(uint16_t addr, uint8_t v, uint32_t now, uint32_t *until) {
    if (addr & 0x8000) {
        return;
    }
    switch (addr & 0x1c00) {
        case 0x1800: {
            uint8_t before = d.via1.orb & d.via1.ddrb;
            via_write(&d.via1, (uint8_t)addr, v, now);
            if ((d.via1.orb & d.via1.ddrb) != before) {
                d.port_before = before;
                d.port_changed_at = now;
            }
            irq_update(now);
            *until = now;   // bus or timers may have changed: re-plan
            return;
        }
        case 0x1c00: {
            via_write(&d.via2, (uint8_t)addr, v, now);
            // CB2 low (PCR bits 7-5 = 110): write mode.
            bool writing = (d.via2.pcr & 0xe0) == 0xc0;
            d.writing = writing;
            motor_and_head(now);
            irq_update(now);
            *until = now;
            return;
        }
        default:
            if ((addr & 0x1fff) < 0x0800) {
                d.ram[addr & 0x7ff] = v;
            }
            return;
    }
}

#define C6510_FN cpu6510_run_1541
#define C6510_READ(addr) drive_read(addr, cyc, &until)
#define C6510_WRITE(addr, v) drive_write(addr, v, cyc, &until)
#include "cpu6510_impl.h"

// A byte comes under the head at drive cycle `at`.
static void disk_byte(uint32_t at) {
    if (d.writing) {
        // The byte on port A goes under the head.
        if (d.pos < TRACK_MAX) {
            d.wtrack[d.pos] = d.via2.ora & d.via2.ddra;
            d.wmask[d.pos >> 3] |= (uint8_t)(1u << (d.pos & 7));
            d.track_written = true;
        }
        if (++d.pos >= d.gcr_len) {
            d.pos = 0;
        }
        d.sync = false;
        if ((d.via2.pcr & 0x0e) == 0x0e) {
            d.cpu.fv = 1;
        }
        d.via2.ifr |= IFR_CA1;
        irq_update(at);
        return;
    }
    d.prev_byte = d.cur_byte;
    d.cur_byte = gcr_byte(d.pos);
    if (++d.pos >= d.gcr_len) {
        d.pos = 0;
    }
    // SYNC: ten 1 bits or more (here: two $FF bytes in a row); no BYTE READY
    // inside it.
    d.sync = d.cur_byte == 0xff && d.prev_byte == 0xff;
    if (d.sync) {
        return;
    }
    d.latch = d.cur_byte;
    // BYTE READY: to the CPU's SO pin (sets V) when CA2 (SOE) is high, and to
    // VIA 2 CA1.
    if ((d.via2.pcr & 0x0e) == 0x0e) {
        d.cpu.fv = 1;
    }
    d.via2.ifr |= IFR_CA1;
    irq_update(at);
}

static void run_drive(uint32_t target) {
    while ((int32_t)(d.cpu.cycles - target) < 0) {
        uint32_t until = target;
        if (d.motor && (int32_t)(d.next_byte - until) < 0) {
            until = d.next_byte;
        }
        uint32_t n1 = via_next(&d.via1), n2 = via_next(&d.via2);
        if (n1 != NEVER && (int32_t)(n1 - until) < 0) {
            until = n1;
        }
        if (n2 != NEVER && (int32_t)(n2 - until) < 0) {
            until = n2;
        }
        if ((int32_t)(until - d.cpu.cycles) <= 0) {
            until = d.cpu.cycles + 1;
        }
        // Parked at the idle loop start while nothing can change: skip to the
        // next event (also across calls, until an interrupt or the ATN flag).
        if (d.idle_hit && d.cpu.pc == IDLE_LOOP && !d.cpu.irq && !d.cpu.nmi && d.ram[0x7c] == 0 &&
            d.ram[0x26c] == 0) {
            d.cpu.cycles = until;
        } else {
            d.idle_hit = false;
            cpu6510_run_1541(&d.cpu, until);
        }
        uint32_t now = d.cpu.cycles;
        uint32_t at = now;
        if (via_events(&d.via1, now, &at) | via_events(&d.via2, now, &at)) {
            irq_update(at);
        }
        while (d.motor && (int32_t)(now - d.next_byte) >= 0) {
            uint32_t t = d.next_byte;
            uint8_t out = d.via2.orb & d.via2.ddrb;
            d.next_byte += byte_cycles((out >> 5) & 3);
            disk_byte(t);
        }
    }
}

/******************************************************************************/
// Interface

static uint32_t drive_cycle_of(uint32_t c64_cycle) {
    uint64_t total = (uint64_t)(c64_cycle - d.c64_ref) * 1000000u + d.rem;
    return d.drive_ref + (uint32_t)(total / 985248u);
}

static void rebase(uint32_t c64_cycle) {
    uint64_t total = (uint64_t)(c64_cycle - d.c64_ref) * 1000000u + d.rem;
    d.drive_ref += (uint32_t)(total / 985248u);
    d.rem = (uint32_t)(total % 985248u);
    d.c64_ref = c64_cycle;
}

uint32_t (*c1541_clock)(void);
uint32_t c1541_prof_cycles;

void c1541_run_to(uint32_t c64_cycle) {
    if (!d.powered) {
        return;
    }
    if ((int32_t)(c64_cycle - d.c64_ref) < 0) {
        return;
    }
    uint32_t t0 = c1541_clock ? c1541_clock() : 0;
    d.synced_to = drive_cycle_of(c64_cycle);
    run_drive(d.synced_to);
    if (c1541_clock) {
        c1541_prof_cycles += c1541_clock() - t0;
    }
    if (c64_cycle - d.c64_ref > 100000) {
        rebase(c64_cycle);
    }
}

void c1541_bus_c64(uint8_t low) {
    d.c64_low = low & 7;
    if (d.powered) {
        // ATN to VIA 1 CA1 (1 = ATN asserted, through an inverter).
        via_set_ca1(&d.via1, d.c64_low & 1);
        irq_update(d.cpu.cycles);
    }
}

void c1541_init(const uint8_t *rom) {
    memset(&d, 0, sizeof(d));
    d.rom = rom;
    // The idle loop as in DOS 2.6: CLI at $EBFF, JMP $EBFF at $EC9B.
    d.idle_trap = rom[IDLE_LOOP & 0x3fff] == 0x58 && rom[IDLE_JMP & 0x3fff] == 0x4c &&
                  rom[(IDLE_JMP + 1) & 0x3fff] == 0xff && rom[(IDLE_JMP + 2) & 0x3fff] == 0xeb;
    d.gcr_track = -1;
    d.half_track = 36;
}

void c1541_reset(uint32_t c64_cycle) {
    if (!d.powered) {
        return;
    }
    c1541_run_to(c64_cycle);
    uint32_t now = d.cpu.cycles;
    via_reset(&d.via1);
    via_reset(&d.via2);
    memset(&d.cpu, 0, sizeof(d.cpu));
    d.cpu.cycles = now;
    d.cpu.s = 0xfd;
    d.cpu.fi = 1;
    d.cpu.pc = (uint16_t)(d.rom[0x3ffc] | (d.rom[0x3ffd] << 8));
    d.motor = false;
    d.phase = 0;
    via_set_ca1(&d.via1, d.c64_low & 1);
}

void c1541_power(bool on, uint32_t c64_cycle) {
    if (on == d.powered) {
        return;
    }
    d.powered = on;
    if (on) {
        memset(d.ram, 0, sizeof(d.ram));
        d.c64_ref = c64_cycle;
        d.drive_ref = 0;
        d.rem = 0;
        d.cpu.cycles = 0;
        d.next_byte = 0;
        c1541_reset(c64_cycle);
        head_moved();
    }
}

bool c1541_powered(void) {
    return d.powered;
}

bool c1541_has_disk(void) {
    return d.tracks != 0;
}

static void inserted(uint32_t size) {
    // A disk going in or out covers the write-protect sensor for a moment.
    d.wps_until = d.cpu.cycles + 500000;
    d.tracks = size >= 196608 ? 40 : size >= 174848 ? 35 : 0;
    // 683 / 768 error bytes after the sectors.
    d.err_base = size == 174848 + 683 ? 174848 : size == 196608 + 768 ? 196608 : 0;
    d.cache_track = -1;
    if (d.tracks) {
        const uint8_t *bam = sector_data(18, 0);
        d.disk_id1 = bam[0xa2];
        d.disk_id2 = bam[0xa3];
    }
    d.gcr_track = -1;
    head_moved();
}

void c1541_insert(const uint8_t *d64, uint32_t size) {
    commit_write();
    d.writer = NULL;
    d.d64 = d64;
    d.reader = NULL;
    inserted(d64 ? size : 0);
}

void c1541_set_writer(c1541_write_t writer) {
    d.writer = writer;
}

c1541_read_t c1541_reader(void) {
    return d.tracks ? d.reader : NULL;
}

void c1541_commit(void) {
    commit_write();
}

void c1541_insert_reader(c1541_read_t reader, uint32_t size) {
    commit_write();
    d.writer = NULL;
    d.d64 = NULL;
    d.reader = reader;
    inserted(reader ? size : 0);
}

void c1541_state(c1541_state_t *s) {
    s->pc = d.cpu.pc;
    s->track2 = d.half_track;
    s->motor = d.motor;
    s->led = (d.via2.orb & d.via2.ddrb) & 0x08;
    s->cycles = d.cpu.cycles;
}
