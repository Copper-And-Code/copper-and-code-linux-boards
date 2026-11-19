// 6510 CPU core (NMOS 6502 instruction set, undocumented opcodes included),
// instruction-stepped and written for speed on the Cortex-M33.
//
// Every 6502 cycle is exactly one bus access (reads, writes and the dummy
// reads the chip makes). The core counts one cycle per access, so the cycle
// counter seen by a bus hook is the exact cycle of that access. The bus is
// supplied at compile time: a C file defines the hooks below and then includes
// cpu6510_impl.h, which defines the run function.
//
//   C6510_FN               name of the run function
//   C6510_READ(addr)       expression: read a byte (uint16_t addr)
//   C6510_WRITE(addr, v)   statement: write a byte
//   C6510_DUMMY(addr)      statement: a dummy read (default: nothing)
//
// The hooks may read and change the local `cyc` (uint32_t): cycle number of
// the access; adding to it stalls the CPU (VIC-II badlines, BA low). They may
// also lower the parameter `until`, to stop the run earlier (a new event).
//
// IRQ: taken at an instruction boundary if the line was active at least 2
// cycles before it (the 6502 polls during the last cycle), CLI/SEI/PLP delay
// included. Not modelled yet: the branch-taken interrupt delay, the same
// timing for NMI, NMI hijacking of BRK/IRQ.

#ifndef C64_CPU6510_H
#define C64_CPU6510_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t cycles;        // cycle counter (wraps)
    uint16_t pc;
    uint8_t a, x, y, s;
    // Flags, unpacked: N = fn bit 7, Z = (fz == 0), the others 0 or 1.
    uint8_t fn, fz, fc, fv, fd, fi;
    uint8_t idelay, iold;   // CLI/SEI/PLP: poll interrupts with the old I once
    volatile uint8_t irq;   // IRQ line: non-zero = asserted (bit per source)
    uint32_t irq_since;     // cycle the IRQ line went active (taken only if it was
                            // active before the last cycle of an instruction)
    volatile uint8_t nmi;   // set to 1 on an NMI edge; cleared when taken
    uint8_t jammed;         // a JAM opcode stopped the CPU
} cpu6510_t;

// Status register value (B and bit 5 set as pushed by PHP/BRK).
static inline uint8_t cpu6510_get_p(const cpu6510_t *c) {
    return (c->fn & 0x80) | (c->fv << 6) | 0x30 | (c->fd << 3) | (c->fi << 2) | (c->fz ? 0 : 2) | c->fc;
}

static inline void cpu6510_set_p(cpu6510_t *c, uint8_t p) {
    c->fn = p;
    c->fv = (p >> 6) & 1;
    c->fd = (p >> 3) & 1;
    c->fi = (p >> 2) & 1;
    c->fz = !(p & 2);
    c->fc = p & 1;
}

#endif
