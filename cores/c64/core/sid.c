// SID (6581) emulation: see sid.h.

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "sid.h"

// Firmware: the combined-waveform tables (16 KB) stay in flash.
#ifdef SID_TABLES_IN_FLASH
#include "pico.h"
#define SID_TABLE __in_flash("sid")
#else
#define SID_TABLE
#endif
#include "sid_6581_combined.h"
#include "sid_8580_combined.h"

// Combined waveforms (two or three of triangle, sawtooth, pulse; no noise):
// the 6581's measured output at the accumulator position; 0 while the pulse
// is low.
static const uint8_t (*combined_tab)[4096] = sid_6581_combined;

static uint16_t combined(uint8_t sel, uint32_t acc, bool pulse_high) {
    if ((sel & 4) && !pulse_high) {
        return 0;
    }
    return (uint16_t)(combined_tab[sel == 3 ? 0 : sel - 4][acc >> 12] << 4);
}

// ADSR rate counter periods in cycles (attack; decay and release use the
// same periods and the exponential divider).
static const uint16_t rate_period[16] = {
    9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251,
};

enum { ATTACK, DECAY_SUSTAIN, RELEASE };

typedef struct {
    uint32_t acc;       // 24-bit phase accumulator
    uint32_t lfsr;      // 23-bit noise shift register
    uint16_t freq, pw;
    uint8_t ctrl, ad, sr;
    uint8_t env, state, gate;
    uint32_t rate_cnt;
    uint8_t exp_cnt;
    uint16_t wave;      // last 12-bit waveform output
    uint8_t msb_rose;   // MSB of acc went 0 -> 1 in the last step (sync source)
    uint32_t old_acc, delta;    // phase before the last step, and its advance
    uint32_t wsum, wcycles;     // waveform x cycles since the last sample (its mean)
} voice_t;

static struct {
    voice_t v[3];
    uint8_t regs[32];
    float ic1, ic2;     // filter state (integrator capacitors)
    float g, k;         // filter coefficients (from cutoff and resonance)
    float fgain;        // gain of the filtered path
    uint16_t coef_key;  // cutoff and resonance the coefficients belong to (0xffff: none)
    float dc_in, dc_out;    // DC blocker state
    void (*out)(int16_t);
    // Frame: samples spread over the frame's cycles.
    uint32_t frame_start, frame_cycles;
    unsigned frame_samples, done;
    uint32_t pos;       // cycle of the last sample
    uint32_t osc_pos;   // cycle up to which oscillators and envelopes have run
    bool osc_started;
} sid;

static uint16_t noise_out(uint32_t r) {
    return (uint16_t)((((r >> 22) & 1) << 11) | (((r >> 20) & 1) << 10) | (((r >> 16) & 1) << 9) |
                      (((r >> 13) & 1) << 8) | (((r >> 11) & 1) << 7) | (((r >> 7) & 1) << 6) |
                      (((r >> 4) & 1) << 5) | (((r >> 2) & 1) << 4));
}

static uint8_t exp_period(uint8_t env) {
    if (env >= 94) return 1;
    if (env >= 55) return 2;
    if (env >= 27) return 4;
    if (env >= 15) return 8;
    if (env >= 7) return 16;
    return 30;
}

// The rate counter is 15 bits and fires only when it equals the period: after
// a change to a shorter period it first runs round to $7FFF (and on to 1),
// which delays the envelope by up to 32768 cycles (the "ADSR delay bug" that
// hard-restart techniques rely on).
static uint32_t rate_wait(uint32_t cnt, uint32_t period) {
    return cnt < period ? period - cnt : 0x8000 - cnt + period - 1;
}

static void adsr_step(voice_t *v, uint32_t cycles) {
    for (;;) {
        unsigned rate = v->state == ATTACK ? v->ad >> 4 : v->state == DECAY_SUSTAIN ? v->ad & 15 : v->sr & 15;
        uint32_t period = rate_period[rate];
        uint32_t need = rate_wait(v->rate_cnt, period);
        if (cycles < need) {
            v->rate_cnt += cycles;
            if (v->rate_cnt >= 0x8000) {
                v->rate_cnt = v->rate_cnt - 0x8000 + 1;
            }
            return;
        }
        cycles -= need;
        v->rate_cnt = 0;
        if (v->state == ATTACK) {
            if (++v->env == 255) {
                v->state = DECAY_SUSTAIN;
            }
            continue;
        }
        if (++v->exp_cnt < exp_period(v->env)) {
            continue;
        }
        v->exp_cnt = 0;
        if (v->state == DECAY_SUSTAIN) {
            if (v->env > (v->sr >> 4) * 17) {
                v->env--;
            }
        } else if (v->env > 0) {
            v->env--;
        }
    }
}

// Anti-aliasing: for a single sawtooth, triangle or pulse waveform the sample
// is the waveform's mean over the cycles since the last sample (a box filter,
// exact): integrals of the waveform over the 24-bit phase, A(phase) from 0.
#define PHASE_END (1u << 24)

static int64_t wave_integral(unsigned sel, uint32_t phase, uint32_t pw) {
    int64_t p = phase;
    switch (sel) {
        case 2:     // sawtooth: phase >> 12
            return (p * p) >> 13;
        case 1: {   // triangle: up to 4095 in the first half, down in the second
            const int64_t h = PHASE_END / 2;
            if (p <= h) {
                return (p * p) >> 12;
            }
            int64_t q = PHASE_END - p;      // mirrored
            return ((h * h) >> 11) - ((q * q) >> 12);
        }
        default: {  // pulse: 4095 from pw << 12
            int64_t t = (int64_t)pw << 12;
            return p > t ? (p - t) * 4095 : 0;
        }
    }
}

// Mean over the phase interval [old, old + delta) (delta > 0).
static uint16_t wave_mean(unsigned sel, uint32_t old, uint32_t delta, uint32_t pw) {
    uint32_t end = old + delta;
    int64_t sum = wave_integral(sel, end % PHASE_END, pw) - wave_integral(sel, old, pw) +
                  (int64_t)(end / PHASE_END) * wave_integral(sel, PHASE_END, pw);
    float m = (float)sum / (float)delta;
    return m < 0 ? 0 : m > 4095.0f ? 4095 : (uint16_t)m;
}

// Advance the oscillators by `cycles` and update the waveform outputs.
static void osc_step(uint32_t cycles) {
    for (unsigned i = 0; i < 3; ++i) {
        voice_t *v = &sid.v[i];
        voice_t *src = &sid.v[(i + 2) % 3];  // sync/ring source: 1<-3, 2<-1, 3<-2
        if (v->ctrl & 8) {  // test bit
            v->acc = 0;
            v->lfsr = 0x7fffff;
            v->msb_rose = 0;
            v->delta = 0;
        } else {
            uint32_t old = v->acc;
            uint32_t delta = v->freq * cycles;
            uint32_t now = old + delta;
            // Noise: clocked on each 0 -> 1 edge of accumulator bit 19 (edges
            // at k * 2^20 + 2^19); the MSB rises at k * 2^24 + 2^23.
            uint32_t edges = ((now + 0x80000) >> 20) - ((old + 0x80000) >> 20);
            for (uint32_t e = 0; e < edges && e < 24; ++e) {
                uint32_t bit = ((v->lfsr >> 22) ^ (v->lfsr >> 17)) & 1;
                v->lfsr = ((v->lfsr << 1) | bit) & 0x7fffff;
            }
            v->msb_rose = ((now + 0x800000) >> 24) != ((old + 0x800000) >> 24);
            v->acc = now & 0xffffff;
            v->old_acc = old;
            v->delta = delta;
        }
        // Hard sync: reset when the source's MSB rose (source updated first
        // for voices 2 and 3; voice 1 uses voice 3 of the previous step).
        bool synced = (v->ctrl & 2) && src->msb_rose;
        if (synced) {
            v->acc = 0;
        }
        uint8_t sel1 = v->ctrl >> 4;
        if (!(v->ctrl & 8) && !synced && v->delta && (sel1 == 2 || sel1 == 4 || (sel1 == 1 && !(v->ctrl & 4)))) {
            v->wave = wave_mean(sel1, v->old_acc, v->delta, v->pw);
            v->wsum += v->wave * cycles;
            v->wcycles += cycles;
            continue;
        }
        uint16_t w = 0xfff;
        uint8_t sel = v->ctrl >> 4;
        if (sel == 3 || (sel >= 5 && sel <= 7)) {
            v->wave = combined(sel, v->acc, (v->ctrl & 8) || (v->acc >> 12) >= v->pw);
            v->wsum += v->wave * cycles;
            v->wcycles += cycles;
            continue;
        }
        if (sel & 1) {      // triangle
            uint32_t a = v->acc;
            uint32_t msb = a & 0x800000;
            if (v->ctrl & 4) {
                msb ^= src->acc & 0x800000;     // ring modulation
            }
            w &= (uint16_t)(((msb ? ~a : a) >> 11) & 0xfff);
        }
        if (sel & 2) {      // sawtooth
            w &= (uint16_t)(v->acc >> 12);
        }
        if (sel & 4) {      // pulse
            w &= ((v->ctrl & 8) || (v->acc >> 12) >= v->pw) ? 0xfff : 0;
        }
        if (sel & 8) {      // noise
            w &= noise_out(v->lfsr);
        }
        v->wave = sel ? w : 0;
        v->wsum += v->wave * cycles;
        v->wcycles += cycles;
    }
}

// 6581 filter cutoff against the 11-bit cutoff register, every 64 steps
// ($D416 every 8): the parameter of our filter, calibrated so that our output
// matches the cutoff measured on VICE 3.7.1's reSID 6581 (tests/c64/
// s01_cutoff.asm, tests/tools/sid_cutoff.py; docs/log/m7-sid.txt). Real
// 6581s differ from chip to chip.
static const float cutoff_6581[33] = {
    430.0f, 526.0f, 654.0f, 755.0f, 966.0f, 1307.0f, 1670.0f, 2329.0f,
    2892.0f, 3916.0f, 5154.0f, 6587.0f, 7721.0f, 9050.0f, 9879.0f, 10784.0f,
    11772.0f, 12851.0f, 14028.0f, 25000.0f, 25000.0f, 25000.0f, 25000.0f, 25000.0f,
    25000.0f, 25000.0f, 25000.0f, 25000.0f, 25000.0f, 25000.0f, 25000.0f, 25000.0f,
    25000.0f
};

// Gain of the filtered path against the same points (the 6581's filter output
// is weaker, more so at low cutoffs): measured on VICE in the same way.
static const float filter_gain_6581[33] = {
    0.575f, 0.495f, 0.427f, 0.427f, 0.403f, 0.447f, 0.589f, 0.700f,
    0.733f, 0.785f, 0.804f, 0.813f, 0.822f, 0.832f, 0.832f, 0.832f,
    0.841f, 0.861f, 0.871f, 0.832f, 0.841f, 0.832f, 0.832f, 0.851f,
    0.822f, 0.813f, 0.841f, 0.841f, 0.841f, 0.832f, 0.841f, 0.832f,
    0.832f
};

// The same for the 8580 (measured on VICE's reSID 8580 in the same way): an
// almost linear cutoff, no loss in the filtered path.
static const float cutoff_8580[33] = {
    30.0f, 734.0f, 1511.0f, 2329.0f, 3110.0f, 4032.0f, 4864.0f, 5620.0f,
    6587.0f, 7395.0f, 8300.0f, 8921.0f, 9544.0f, 10211.0f, 10924.0f, 11688.0f,
    12504.0f, 13378.0f, 14313.0f, 25000.0f, 25000.0f, 25000.0f, 25000.0f, 25000.0f,
    25000.0f, 25000.0f, 25000.0f, 25000.0f, 25000.0f, 25000.0f, 25000.0f, 25000.0f,
    25000.0f
};

static const float filter_gain_8580[33] = {
    1.000f, 0.913f, 0.951f, 0.964f, 1.003f, 0.976f, 1.012f, 1.020f,
    1.013f, 1.008f, 0.999f, 0.991f, 1.000f, 1.005f, 1.008f, 0.998f,
    1.018f, 1.029f, 1.035f, 1.005f, 1.013f, 1.013f, 1.025f, 1.051f,
    1.017f, 0.992f, 1.026f, 1.031f, 1.023f, 1.012f, 1.029f, 1.013f,
    1.013f
};

#define SAMPLE_RATE (31250.0f)

static const float *cutoff_tab = cutoff_6581, *gain_tab = filter_gain_6581;
static float mixer_dc = 0.35f;      // 6581: the volume also scales a DC offset
static int model = 6581;

void sid_set_model(int m) {
    model = m == 8580 ? 8580 : 6581;
    cutoff_tab = model == 8580 ? cutoff_8580 : cutoff_6581;
    gain_tab = model == 8580 ? filter_gain_8580 : filter_gain_6581;
    combined_tab = model == 8580 ? sid_8580_combined : sid_6581_combined;
    mixer_dc = model == 8580 ? 0.0f : 0.35f;    // 8580: no DC, so volume "digis" are nearly silent
    sid.coef_key = 0xffff;
}

int sid_model(void) {
    return model;
}

static void filter_coefs(unsigned key) {
    unsigned fc = key & 0x7ff, res = key >> 11;
    float x = fc * (1.0f / 64.0f);
    unsigned i = (unsigned)x;
    float hz = cutoff_tab[i] + (cutoff_tab[i + 1] - cutoff_tab[i]) * (x - (float)i);
    // Near the Nyquist frequency the filter is as good as open.
    if (hz > 0.49f * SAMPLE_RATE) {
        hz = 0.49f * SAMPLE_RATE;
    }
    sid.fgain = gain_tab[i] + (gain_tab[i + 1] - gain_tab[i]) * (x - (float)i);
    sid.g = tanf(3.14159265f * hz / SAMPLE_RATE);
    // Resonance: Q from 0.707 to 1.707 (1/Q = damping k).
    sid.k = 1.0f / (0.707f + res / 15.0f);
    sid.coef_key = (uint16_t)key;
}

// Oscillators and envelopes up to cycle `t` (at every sample, and before
// every register access, so that short pulses of the test bit or reads of
// voice 3 see the exact cycle).
static void sync_to(uint32_t t) {
    uint32_t cycles = t - sid.osc_pos;
    if ((int32_t)cycles <= 0) {
        return;
    }
    osc_step(cycles);
    for (unsigned i = 0; i < 3; ++i) {
        adsr_step(&sid.v[i], cycles);
    }
    sid.osc_pos = t;
}

static int16_t make_sample(uint32_t at) {
    sync_to(at);
    uint8_t filt = sid.regs[0x17] & 7, mode = sid.regs[0x18];
    float vol = (mode & 15) / 15.0f;
    float direct = 0, filtered = 0;
    for (unsigned i = 0; i < 3; ++i) {
        voice_t *v = &sid.v[i];
        // The waveform's mean over the sample's cycles (register writes in
        // between split the oscillator steps).
        int wave = v->wcycles ? (int)(v->wsum / v->wcycles) : v->wave;
        v->wsum = v->wcycles = 0;
        float s = (wave - 0x800) * (float)v->env * (1.0f / (2048.0f * 255.0f));
        if (filt & (1u << i)) {
            filtered += s;
        } else if (i != 2 || !(mode & 0x80)) {  // 3OFF mutes voice 3 when not filtered
            direct += s;
        }
    }
    // State-variable filter (trapezoidal integration: stable at any cutoff).
    unsigned key = (sid.regs[0x16] << 3) | (sid.regs[0x15] & 7) | ((sid.regs[0x17] & 0xf0) << 7);
    if (key != sid.coef_key) {
        filter_coefs(key);
    }
    float g = sid.g, k = sid.k;
    // The SID filter integrates with inverted signs: it is a standard
    // state-variable filter fed with the inverted input, so low-pass and
    // high-pass come out inverted and band-pass not (as reSID models it).
    float a1 = 1.0f / (1.0f + g * (g + k));
    float v3 = -filtered - sid.ic2;
    float v1 = a1 * (sid.ic1 + g * v3);
    float v2 = sid.ic2 + g * v1;
    sid.ic1 = 2.0f * v1 - sid.ic1;
    sid.ic2 = 2.0f * v2 - sid.ic2;
    float lp = v2, bp = -v1, hp = -filtered - k * v1 - v2;
    float fout = 0;
    if (mode & 0x10) fout += lp;
    if (mode & 0x20) fout += bp;
    if (mode & 0x40) fout += hp;
    fout *= sid.fgain;
    // 6581: the volume also scales a DC offset of the mixer, so writes to the
    // volume alone make sound ("digis").
    float mix = (direct + fout + mixer_dc) * vol;
    // Output capacitor of the C64 audio stage: DC blocked (1-pole high-pass,
    // about 10 Hz at 31.25 kHz).
    float hpo = mix - sid.dc_in + 0.998f * sid.dc_out;
    sid.dc_in = mix;
    sid.dc_out = hpo;
    float o = hpo * (32767.0f / 16.0f);
    if (o > 32767.0f) o = 32767.0f;
    if (o < -32768.0f) o = -32768.0f;
    return (int16_t)o;
}

uint32_t (*sid_clock)(void);
uint32_t sid_prof_cycles, sid_prof_samples;

// Produce the samples due up to `cycle`.
static void run_to(uint32_t cycle) {
    if (!sid.frame_samples) {
        return;
    }
    uint32_t into = cycle - sid.frame_start;
    if ((int32_t)into < 0) {
        return;
    }
    if (into > sid.frame_cycles) {
        into = sid.frame_cycles;
    }
    unsigned due = (unsigned)((uint64_t)into * sid.frame_samples / sid.frame_cycles);
    while (sid.done < due) {
        sid.done++;
        uint32_t at = sid.frame_start + (uint32_t)((uint64_t)sid.done * sid.frame_cycles / sid.frame_samples);
        uint32_t t0 = sid_clock ? sid_clock() : 0;
        int16_t s = make_sample(at);
        if (sid_clock) {
            sid_prof_cycles += sid_clock() - t0;
            sid_prof_samples++;
        }
        sid.pos = at;
        if (sid.out) {
            sid.out(s);
        }
    }
}

void sid_init(void (*out)(int16_t)) {
    memset(&sid, 0, sizeof(sid));
    sid.out = out;
    sid_reset();
}

void sid_reset(void) {
    void (*out)(int16_t) = sid.out;
    memset(&sid, 0, sizeof(sid));
    sid.out = out;
    for (unsigned i = 0; i < 3; ++i) {
        sid.v[i].lfsr = 0x7fffff;
        sid.v[i].state = RELEASE;
    }
    sid.coef_key = 0xffff;
}

void sid_frame_begin(uint32_t start, uint32_t cycles, unsigned samples) {
    sid.frame_start = start;
    sid.frame_cycles = cycles;
    sid.frame_samples = samples;
    sid.done = 0;
    sid.pos = start;
    if (!sid.osc_started) {
        sid.osc_pos = start;
        sid.osc_started = true;
    }
}

void sid_advance(uint32_t cycle) {
    run_to(cycle);
}

void sid_frame_end(void) {
    run_to(sid.frame_start + sid.frame_cycles);
    sid.frame_samples = 0;
}

void sid_write(uint8_t reg, uint8_t value, uint32_t cycle) {
    reg &= 0x1f;
    run_to(cycle);
    if (sid.osc_started) {
        sync_to(cycle);
    }
    sid.regs[reg] = value;
    if (reg < 21) {
        voice_t *v = &sid.v[reg / 7];
        switch (reg % 7) {
            case 0: v->freq = (v->freq & 0xff00) | value; break;
            case 1: v->freq = (v->freq & 0x00ff) | (value << 8); break;
            case 2: v->pw = (v->pw & 0xf00) | value; break;
            case 3: v->pw = (v->pw & 0x0ff) | ((value & 15) << 8); break;
            case 4: {
                uint8_t gate = value & 1;
                if (gate && !v->gate) {
                    v->state = ATTACK;
                } else if (!gate && v->gate) {
                    v->state = RELEASE;
                }
                v->gate = gate;
                if (value & 8) {    // test bit: accumulator and noise reset at once
                    v->acc = 0;
                    v->lfsr = 0x7fffff;
                }
                v->ctrl = value;
                break;
            }
            case 5: v->ad = value; break;
            default: v->sr = value; break;
        }
    }
}

uint8_t sid_read(uint8_t reg, uint32_t cycle) {
    reg &= 0x1f;
    run_to(cycle);
    switch (reg) {
        case 0x19: case 0x1a: return 0xff;      // paddles: none
        case 0x1b: {
            // Voice 3 waveform at `cycle` (oscillators brought up to it).
            if (sid.osc_started) {
                sync_to(cycle);
            }
            const voice_t *v = &sid.v[2];
            uint32_t acc = v->acc, lfsr = v->lfsr;
            uint8_t sel = v->ctrl >> 4;
            if (sel == 3 || (sel >= 5 && sel <= 7)) {
                return (uint8_t)(combined(sel, acc, (v->ctrl & 8) || (acc >> 12) >= v->pw) >> 4);
            }
            uint16_t w = 0xfff;
            if (sel & 1) w &= (uint16_t)((((acc & 0x800000) ? ~acc : acc) >> 11) & 0xfff);
            if (sel & 2) w &= (uint16_t)(acc >> 12);
            if (sel & 4) w &= ((v->ctrl & 8) || (acc >> 12) >= v->pw) ? 0xfff : 0;
            if (sel & 8) w &= noise_out(lfsr);
            return sel ? (uint8_t)(w >> 4) : 0;
        }
        case 0x1c:
            if (sid.osc_started) {
                sync_to(cycle);
            }
            return sid.v[2].env;
        default: return 0;                      // write-only registers
    }
}
