// SID (6581 or 8580, sid_set_model) emulation on core 0, sample-based (not cycle-exact inside a
// sample, except that oscillators and envelopes are brought up to the exact
// cycle of every register access): 3 voices (triangle, sawtooth, pulse, noise;
// combined waveforms from tables measured on VICE's reSID 6581 / 8580, so
// programs that tell the two apart by reading them back see the right one), ring
// modulation, sync, ADSR with the real rate periods, the 15-bit
// rate counter (ADSR delay bug) and the exponential decay, a state-variable
// filter (LP/BP/HP, resonance; trapezoidal, stable at any cutoff) with the
// cutoff curve and filtered-path gain measured on VICE's reSID 6581 / 8580,
// volume with the 6581 DC offset (so $D418 "digis" are audible; none on the
// 8580), and the voice 3
// read-back registers ($D41B, $D41C). Single sawtooth, triangle and pulse
// waveforms are anti-aliased (each sample is the waveform's exact mean since
// the previous one).
//
// Register writes take effect at their cycle: the SID is first brought up to
// that cycle. The machine produces a given number of samples per C64 frame,
// spread evenly over the frame (sid_frame_begin / sid_frame_end).
//
// Approximations: no filter distortion (reSID 1.0 models the 6581's
// transistors), combined waveforms with noise (AND), pulse + sawtooth (VICE's
// own output for it is not repeatable), audio from 8-bit table values,
// sync/noise resolved per sample (~31 cycles) between register accesses, one
// cutoff curve (real 6581s differ from chip to chip).

#ifndef C64_SID_H
#define C64_SID_H

#include <stdint.h>

// out: receives each sample (signed 16-bit).
void sid_init(void (*out)(int16_t sample));
void sid_reset(void);

// Start a frame of `cycles` C64 cycles at cycle `start`, producing `samples`
// samples during it.
void sid_frame_begin(uint32_t start, uint32_t cycles, unsigned samples);
// Produce all the samples of the frame not produced yet.
void sid_frame_end(void);
// Produce the samples due up to `cycle` (called once per raster line, so that
// the work is spread over the frame instead of coming in bursts at writes).
void sid_advance(uint32_t cycle);

void sid_write(uint8_t reg, uint8_t value, uint32_t cycle);
// Chip model: 6581 (default) or 8580; takes effect at once.
void sid_set_model(int model);
int sid_model(void);
uint8_t sid_read(uint8_t reg, uint32_t cycle);

// Profiling (optional): a cycle counter set by the firmware; cycles spent
// making samples, and the number of samples.
extern uint32_t (*sid_clock)(void);
extern uint32_t sid_prof_cycles, sid_prof_samples;

#endif
