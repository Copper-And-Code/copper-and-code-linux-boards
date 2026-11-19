// Commodore 1541 disk drive, emulated at the hardware level: its 6502 (the
// same core as the C64's) running the DOS ROM, 2 KB RAM, VIA 1 (serial bus)
// and VIA 2 (drive mechanics: stepper, motor, density, read head data, SYNC,
// BYTE READY on the CPU's SO pin), the disk turning at 300 rpm with each track
// GCR-encoded from a D64 image, and the IEC bus lines (ATN, CLK, DATA, open
// collector, with the 1541's automatic ATN acknowledge).
//
// The drive is kept in step with the C64: before every C64 access to the
// serial bus (CIA 2 port A) and at the end of each raster line it runs up to
// the C64's cycle (clock ratio 1 MHz / 985 248 Hz, exact); a bus change the
// drive made past that cycle (it runs whole instructions) is not seen until
// then. Fast loaders that run their own code in the drive work, as on the real
// hardware. D64 images with an error table (175 531 / 197 376 bytes): the
// sectors marked with an error read with it (20, 21, 22, 23, 27, 29: what copy
// protections check).
//
// Writing: the bytes the DOS writes in write mode (VIA 2 CB2 low) go into a
// copy of the track, read back from there at once (formatting measures the
// track), and when the head leaves the track, the motor stops or the disk
// changes, the track is decoded again and each written data block goes to
// the sector named by its header, through the function set with
// c1541_set_writer (none: write protected, the sensor says so). SAVE, files,
// scratch and format (N:) give the same image as VICE's true drive.

#ifndef C64_C1541_H
#define C64_C1541_H

#include <stdbool.h>
#include <stdint.h>

// rom: the 16 KB DOS ROM ($C000-$FFFF; not copied). Drive off until c1541_power.
void c1541_init(const uint8_t *rom);
// Power the drive on (reset) or off (it then leaves the bus alone).
void c1541_power(bool on, uint32_t c64_cycle);
bool c1541_powered(void);
bool c1541_has_disk(void);
// Reset line of the serial bus (the C64's reset also resets the drive).
void c1541_reset(uint32_t c64_cycle);
// Insert a D64 image (35 or 40 tracks, 174848 / 196608 bytes, error bytes
// ignored; not copied), or eject with NULL.
void c1541_insert(const uint8_t *d64, uint32_t size);
// The same, the image read through a function (bytes at offset; false on an
// error): the drive keeps the track under the head (up to 5.25 KB) in RAM.
typedef bool (*c1541_read_t)(uint32_t offset, uint8_t *buf, uint32_t len);
void c1541_insert_reader(c1541_read_t reader, uint32_t size);
// Where written sectors go (256 bytes at an offset of the image); NULL: the
// disk is write protected. Inserting a disk sets NULL: set it after.
typedef void (*c1541_write_t)(uint32_t offset, const uint8_t *data, uint32_t len);
void c1541_set_writer(c1541_write_t writer);
// Store at once what was written on the track under the head (else done when
// the head moves, the motor stops or the disk changes).
void c1541_commit(void);
// The read function of the disk in the drive (NULL: none, or a disk in memory).
c1541_read_t c1541_reader(void);

// Run the drive up to C64 cycle `c64_cycle`.
void c1541_run_to(uint32_t c64_cycle);
// The C64's side of the bus changed (call after c1541_run_to of that cycle):
// bits pulled low by the C64: 1 = ATN, 2 = CLK, 4 = DATA.
void c1541_bus_c64(uint8_t low);
// Lines as the C64 reads them: bit 6 = CLK high, bit 7 = DATA high.
uint8_t c1541_bus_c64_in(void);

// Profiling (optional): a cycle counter set by the firmware; cycles spent
// running the drive.
extern uint32_t (*c1541_clock)(void);
extern uint32_t c1541_prof_cycles;

// Diagnostics.
typedef struct {
    uint16_t pc;
    uint8_t track2;         // half track (2 = track 1)
    bool motor, led;
    uint32_t cycles;
} c1541_state_t;
void c1541_state(c1541_state_t *s);

#endif
