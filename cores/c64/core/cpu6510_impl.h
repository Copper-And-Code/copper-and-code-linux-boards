// 6510 CPU core: run function. Include once, after defining the bus hooks
// (see cpu6510.h). Defines
//
//   uint32_t C6510_FN(cpu6510_t *cpu, uint32_t until);
//
// which executes whole instructions while the cycle counter is before `until`
// (signed difference, so the counter may wrap) and returns the counter.
//
// Bus sequences follow the NMOS 6502 cycle by cycle, dummy accesses included
// (checked against floooh's m6502 by tests/host/cpu_diff.c).

#include "cpu6510.h"

#ifndef C6510_FN
#error "define C6510_FN"
#endif
#if !defined(C6510_READ) || !defined(C6510_WRITE)
#error "define C6510_READ and C6510_WRITE"
#endif
#ifndef C6510_DUMMY
#define C6510_DUMMY(addr) ((void)0)
#endif

// Magic constant of the unstable ANE ($8B) and LXA ($AB) opcodes.
#ifndef C6510_ANE_MAGIC
#define C6510_ANE_MAGIC (0xee)
#endif
#ifndef C6510_LXA_MAGIC
#define C6510_LXA_MAGIC (0xee)
#endif

// One bus cycle each.
#define RD(var, ad) do { uint16_t ad_ = (uint16_t)(ad); var = C6510_READ(ad_); cyc++; } while (0)
#define WR(ad, val) do { uint16_t ad_ = (uint16_t)(ad); uint8_t v_ = (uint8_t)(val); C6510_WRITE(ad_, v_); cyc++; } while (0)
#define DUM(ad) do { uint16_t ad_ = (uint16_t)(ad); C6510_DUMMY(ad_); (void)ad_; cyc++; } while (0)
#define PUSH(val) do { WR(0x100 | s, val); s--; } while (0)

#define SETNZ(r) (fn = fz = (uint8_t)(r))
#define GETP(b) ((uint8_t)((fn & 0x80) | (fv << 6) | 0x20 | ((b) ? 0x10 : 0) | (fd << 3) | (fi << 2) | (fz ? 0 : 2) | fc))
#define SETP(p) do { uint8_t p_ = (p); fn = p_; fv = (p_ >> 6) & 1; fd = (p_ >> 3) & 1; fi = (p_ >> 2) & 1; \
                     fz = !(p_ & 2); fc = p_ & 1; } while (0)

// Addressing modes: leave the effective address in ea, after the cycles that
// come before the data access.
#define AM_ZP()  do { RD(b, pc); pc++; ea = b; } while (0)
#define AM_ZPX() do { RD(b, pc); pc++; DUM(b); ea = (uint8_t)(b + x); } while (0)
#define AM_ZPY() do { RD(b, pc); pc++; DUM(b); ea = (uint8_t)(b + y); } while (0)
#define AM_ABS() do { RD(lo, pc); pc++; RD(hi, pc); pc++; ea = lo | (hi << 8); } while (0)
// Indexed, read: the dummy read at the uncorrected address only on a page crossing.
#define AM_ABI_R(r) do { RD(lo, pc); pc++; RD(hi, pc); pc++; t = lo + (r); ea = (uint16_t)((hi << 8) + t); \
                         if (t > 0xff) { DUM((hi << 8) | (t & 0xff)); } } while (0)
// Indexed, write or read-modify-write: always the dummy read.
#define AM_ABI_W(r) do { RD(lo, pc); pc++; RD(hi, pc); pc++; t = lo + (r); ea = (uint16_t)((hi << 8) + t); \
                         DUM((hi << 8) | (t & 0xff)); } while (0)
#define AM_IZX() do { RD(b, pc); pc++; DUM(b); b += x; RD(lo, b); RD(hi, (uint8_t)(b + 1)); ea = lo | (hi << 8); } while (0)
#define AM_IZY_R() do { RD(b, pc); pc++; RD(lo, b); RD(hi, (uint8_t)(b + 1)); t = lo + y; ea = (uint16_t)((hi << 8) + t); \
                        if (t > 0xff) { DUM((hi << 8) | (t & 0xff)); } } while (0)
#define AM_IZY_W() do { RD(b, pc); pc++; RD(lo, b); RD(hi, (uint8_t)(b + 1)); t = lo + y; ea = (uint16_t)((hi << 8) + t); \
                        DUM((hi << 8) | (t & 0xff)); } while (0)

// Operations on v (the operand) or on registers.
#define OP_ADC() do { \
    if (fd) { \
        unsigned lo_ = (a & 0x0f) + (v & 0x0f) + fc; \
        if (lo_ > 9) lo_ += 6; \
        unsigned r_ = (lo_ <= 0x0f) ? (lo_ & 0x0f) + (a & 0xf0) + (v & 0xf0) \
                                    : (lo_ & 0x0f) + (a & 0xf0) + (v & 0xf0) + 0x10; \
        fz = (uint8_t)(a + v + fc); \
        fn = (uint8_t)r_; \
        fv = ((a ^ r_) & 0x80) && !((a ^ v) & 0x80); \
        if ((r_ & 0x1f0) > 0x90) r_ += 0x60; \
        fc = (r_ & 0xff0) > 0xf0; \
        a = (uint8_t)r_; \
    } else { \
        unsigned r_ = a + v + fc; \
        fv = ((~(a ^ v) & (a ^ r_)) >> 7) & 1; \
        fc = r_ > 0xff; \
        a = (uint8_t)r_; \
        SETNZ(a); \
    } } while (0)
#define OP_SBC() do { \
    unsigned r_ = a - v - !fc; \
    fv = (((a ^ v) & (a ^ r_)) >> 7) & 1; \
    if (fd) { \
        unsigned lo_ = (a & 0x0f) - (v & 0x0f) - !fc; \
        unsigned d_; \
        if (lo_ & 0x10) d_ = ((lo_ - 6) & 0x0f) | ((a & 0xf0) - (v & 0xf0) - 0x10); \
        else d_ = (lo_ & 0x0f) | ((a & 0xf0) - (v & 0xf0)); \
        if (d_ & 0x100) d_ -= 0x60; \
        SETNZ(r_); \
        fc = r_ < 0x100; \
        a = (uint8_t)d_; \
    } else { \
        fc = r_ < 0x100; \
        a = (uint8_t)r_; \
        SETNZ(a); \
    } } while (0)
#define OP_CMP(r) do { unsigned r_ = (r) - v; fc = (r) >= v; SETNZ(r_); } while (0)
#define OP_ASL() do { fc = v >> 7; v <<= 1; SETNZ(v); } while (0)
#define OP_LSR() do { fc = v & 1; v >>= 1; SETNZ(v); } while (0)
#define OP_ROL() do { uint8_t c_ = v >> 7; v = (uint8_t)((v << 1) | fc); fc = c_; SETNZ(v); } while (0)
#define OP_ROR() do { uint8_t c_ = v & 1; v = (uint8_t)((v >> 1) | (fc << 7)); fc = c_; SETNZ(v); } while (0)

// Instruction shapes.
#define READ_OP(am, body) do { am; RD(v, ea); body; } while (0)
#define RMW_OP(am, body) do { am; RD(v, ea); WR(ea, v); body; WR(ea, v); } while (0)
#define IMPLIED(body) do { DUM(pc); body; } while (0)
#define BRANCH(cond) do { RD(b, pc); pc++; if (cond) { DUM(pc); uint16_t t_ = (uint16_t)(pc + (int8_t)b); \
                          if ((t_ ^ pc) & 0xff00) { DUM((pc & 0xff00) | (t_ & 0xff)); } pc = t_; } } while (0)
// SHA/SHX/SHY/TAS: store reg & (H + 1); on a page crossing the value also
// replaces the high byte of the address.
#define SH_STORE(val) do { uint8_t sv_ = (uint8_t)((val) & (hi + 1)); \
                           if (t > 0xff) { ea = (uint16_t)((sv_ << 8) | (ea & 0xff)); } WR(ea, sv_); } while (0)

// The usual opcode columns: (zp,x) zp #imm abs (zp),y zp,x abs,y abs,x.
#define READ_GROUP(o, body) \
    case (o) + 0x01: READ_OP(AM_IZX(), body); break; \
    case (o) + 0x05: READ_OP(AM_ZP(), body); break; \
    case (o) + 0x09: RD(v, pc); pc++; body; break; \
    case (o) + 0x0d: READ_OP(AM_ABS(), body); break; \
    case (o) + 0x11: READ_OP(AM_IZY_R(), body); break; \
    case (o) + 0x15: READ_OP(AM_ZPX(), body); break; \
    case (o) + 0x19: READ_OP(AM_ABI_R(y), body); break; \
    case (o) + 0x1d: READ_OP(AM_ABI_R(x), body); break;
// Shift/inc/dec: accumulator (if acc), zp, abs, zp,x, abs,x.
#define RMW_GROUP(o, body) \
    case (o) + 0x06: RMW_OP(AM_ZP(), body); break; \
    case (o) + 0x0e: RMW_OP(AM_ABS(), body); break; \
    case (o) + 0x16: RMW_OP(AM_ZPX(), body); break; \
    case (o) + 0x1e: RMW_OP(AM_ABI_W(x), body); break;
// Undocumented RMW + ALU: (zp,x) zp abs (zp),y zp,x abs,y abs,x.
#define URMW_GROUP(o, body) \
    case (o) + 0x03: RMW_OP(AM_IZX(), body); break; \
    case (o) + 0x07: RMW_OP(AM_ZP(), body); break; \
    case (o) + 0x0f: RMW_OP(AM_ABS(), body); break; \
    case (o) + 0x13: RMW_OP(AM_IZY_W(), body); break; \
    case (o) + 0x17: RMW_OP(AM_ZPX(), body); break; \
    case (o) + 0x1b: RMW_OP(AM_ABI_W(y), body); break; \
    case (o) + 0x1f: RMW_OP(AM_ABI_W(x), body); break;

uint32_t C6510_FN(cpu6510_t *cpu, uint32_t until) {
    uint32_t cyc = cpu->cycles;
    uint16_t pc = cpu->pc;
    uint8_t a = cpu->a, x = cpu->x, y = cpu->y, s = cpu->s;
    uint8_t fn = cpu->fn, fz = cpu->fz, fc = cpu->fc, fv = cpu->fv, fd = cpu->fd, fi = cpu->fi;
    uint8_t idelay = cpu->idelay, iold = cpu->iold;
    uint8_t op, b, lo, hi, v;
    uint16_t ea;
    unsigned t;

    if (cpu->jammed) {
        cpu->cycles = until;
        return until;
    }
    while ((int32_t)(cyc - until) < 0) {
        uint8_t masked = idelay ? iold : fi;
        idelay = 0;
        if (cpu->nmi || (cpu->irq && !masked && (int32_t)(cyc - cpu->irq_since) >= 2)) {
            uint16_t vec = 0xfffe;
            if (cpu->nmi) {
                cpu->nmi = 0;
                vec = 0xfffa;
            }
            DUM(pc);
            DUM(pc);
            PUSH(pc >> 8);
            PUSH(pc);
            PUSH(GETP(0));
            fi = 1;
            RD(lo, vec);
            RD(hi, vec + 1);
            pc = lo | (hi << 8);
            continue;
        }

        RD(op, pc);
        pc++;
        switch (op) {
            // --- ALU, documented
            READ_GROUP(0x00, a |= v; SETNZ(a))                      // ORA
            READ_GROUP(0x20, a &= v; SETNZ(a))                      // AND
            READ_GROUP(0x40, a ^= v; SETNZ(a))                      // EOR
            READ_GROUP(0x60, OP_ADC())                              // ADC
            READ_GROUP(0xc0, OP_CMP(a))                             // CMP
            READ_GROUP(0xe0, OP_SBC())                              // SBC
            READ_GROUP(0xa0, a = v; SETNZ(a))                       // LDA ($A9 immediate included)

            // STA (no immediate form; $89 is a NOP #imm)
            case 0x81: AM_IZX(); WR(ea, a); break;
            case 0x85: AM_ZP(); WR(ea, a); break;
            case 0x8d: AM_ABS(); WR(ea, a); break;
            case 0x91: AM_IZY_W(); WR(ea, a); break;
            case 0x95: AM_ZPX(); WR(ea, a); break;
            case 0x99: AM_ABI_W(y); WR(ea, a); break;
            case 0x9d: AM_ABI_W(x); WR(ea, a); break;

            // --- shifts, INC, DEC
            RMW_GROUP(0x00, OP_ASL())
            RMW_GROUP(0x20, OP_ROL())
            RMW_GROUP(0x40, OP_LSR())
            RMW_GROUP(0x60, OP_ROR())
            RMW_GROUP(0xc0, v--; SETNZ(v))                          // DEC
            RMW_GROUP(0xe0, v++; SETNZ(v))                          // INC
            case 0x0a: IMPLIED(v = a; OP_ASL(); a = v); break;
            case 0x2a: IMPLIED(v = a; OP_ROL(); a = v); break;
            case 0x4a: IMPLIED(v = a; OP_LSR(); a = v); break;
            case 0x6a: IMPLIED(v = a; OP_ROR(); a = v); break;

            // --- X and Y loads, stores, compares
            case 0xa2: RD(v, pc); pc++; x = v; SETNZ(x); break;
            case 0xa6: READ_OP(AM_ZP(), x = v; SETNZ(x)); break;
            case 0xae: READ_OP(AM_ABS(), x = v; SETNZ(x)); break;
            case 0xb6: READ_OP(AM_ZPY(), x = v; SETNZ(x)); break;
            case 0xbe: READ_OP(AM_ABI_R(y), x = v; SETNZ(x)); break;
            case 0xa0: RD(v, pc); pc++; y = v; SETNZ(y); break;
            case 0xa4: READ_OP(AM_ZP(), y = v; SETNZ(y)); break;
            case 0xac: READ_OP(AM_ABS(), y = v; SETNZ(y)); break;
            case 0xb4: READ_OP(AM_ZPX(), y = v; SETNZ(y)); break;
            case 0xbc: READ_OP(AM_ABI_R(x), y = v; SETNZ(y)); break;
            case 0x86: AM_ZP(); WR(ea, x); break;
            case 0x8e: AM_ABS(); WR(ea, x); break;
            case 0x96: AM_ZPY(); WR(ea, x); break;
            case 0x84: AM_ZP(); WR(ea, y); break;
            case 0x8c: AM_ABS(); WR(ea, y); break;
            case 0x94: AM_ZPX(); WR(ea, y); break;
            case 0xe0: RD(v, pc); pc++; OP_CMP(x); break;
            case 0xe4: READ_OP(AM_ZP(), OP_CMP(x)); break;
            case 0xec: READ_OP(AM_ABS(), OP_CMP(x)); break;
            case 0xc0: RD(v, pc); pc++; OP_CMP(y); break;
            case 0xc4: READ_OP(AM_ZP(), OP_CMP(y)); break;
            case 0xcc: READ_OP(AM_ABS(), OP_CMP(y)); break;

            // --- BIT
            case 0x24: READ_OP(AM_ZP(), fz = a & v; fn = v; fv = (v >> 6) & 1); break;
            case 0x2c: READ_OP(AM_ABS(), fz = a & v; fn = v; fv = (v >> 6) & 1); break;

            // --- register transfers, increments, flags
            case 0xaa: IMPLIED(x = a; SETNZ(x)); break;
            case 0xa8: IMPLIED(y = a; SETNZ(y)); break;
            case 0x8a: IMPLIED(a = x; SETNZ(a)); break;
            case 0x98: IMPLIED(a = y; SETNZ(a)); break;
            case 0xba: IMPLIED(x = s; SETNZ(x)); break;
            case 0x9a: IMPLIED(s = x); break;
            case 0xe8: IMPLIED(x++; SETNZ(x)); break;
            case 0xc8: IMPLIED(y++; SETNZ(y)); break;
            case 0xca: IMPLIED(x--; SETNZ(x)); break;
            case 0x88: IMPLIED(y--; SETNZ(y)); break;
            case 0x18: IMPLIED(fc = 0); break;
            case 0x38: IMPLIED(fc = 1); break;
            case 0x58: IMPLIED(idelay = 1; iold = fi; fi = 0); break;   // CLI
            case 0x78: IMPLIED(idelay = 1; iold = fi; fi = 1); break;   // SEI
            case 0xb8: IMPLIED(fv = 0); break;
            case 0xd8: IMPLIED(fd = 0); break;
            case 0xf8: IMPLIED(fd = 1); break;
            case 0xea: IMPLIED((void)0); break;

            // --- branches
            case 0x10: BRANCH(!(fn & 0x80)); break;
            case 0x30: BRANCH(fn & 0x80); break;
            case 0x50: BRANCH(!fv); break;
            case 0x70: BRANCH(fv); break;
            case 0x90: BRANCH(!fc); break;
            case 0xb0: BRANCH(fc); break;
            case 0xd0: BRANCH(fz); break;
            case 0xf0: BRANCH(!fz); break;

            // --- jumps, subroutines, stack
            case 0x4c: AM_ABS(); pc = ea; break;
            case 0x6c:                                              // JMP (ind), page wrap bug
                AM_ABS();
                RD(lo, ea);
                RD(hi, (ea & 0xff00) | ((ea + 1) & 0xff));
                pc = lo | (hi << 8);
                break;
            case 0x20:                                              // JSR
                RD(lo, pc);
                pc++;
                DUM(0x100 | s);
                PUSH(pc >> 8);
                PUSH(pc);
                RD(hi, pc);
                pc = lo | (hi << 8);
                break;
            case 0x60:                                              // RTS
                DUM(pc);
                DUM(0x100 | s);
                s++;
                RD(lo, 0x100 | s);
                s++;
                RD(hi, 0x100 | s);
                pc = lo | (hi << 8);
                DUM(pc);
                pc++;
                break;
            case 0x40:                                              // RTI
                DUM(pc);
                DUM(0x100 | s);
                s++;
                RD(v, 0x100 | s);
                SETP(v);
                s++;
                RD(lo, 0x100 | s);
                s++;
                RD(hi, 0x100 | s);
                pc = lo | (hi << 8);
                break;
            case 0x00:                                              // BRK
                RD(v, pc);
                pc++;
                PUSH(pc >> 8);
                PUSH(pc);
                PUSH(GETP(1));
                fi = 1;
                RD(lo, 0xfffe);
                RD(hi, 0xffff);
                pc = lo | (hi << 8);
                break;
            case 0x48: DUM(pc); PUSH(a); break;
            case 0x08: DUM(pc); PUSH(GETP(1)); break;
            case 0x68: DUM(pc); DUM(0x100 | s); s++; RD(a, 0x100 | s); SETNZ(a); break;
            case 0x28:                                              // PLP
                DUM(pc);
                DUM(0x100 | s);
                s++;
                RD(v, 0x100 | s);
                idelay = 1;
                iold = fi;
                SETP(v);
                break;

            // --- undocumented: RMW + ALU
            URMW_GROUP(0x00, OP_ASL(); a |= v; SETNZ(a))            // SLO
            URMW_GROUP(0x20, OP_ROL(); a &= v; SETNZ(a))            // RLA
            URMW_GROUP(0x40, OP_LSR(); a ^= v; SETNZ(a))            // SRE
            URMW_GROUP(0x60, OP_ROR(); OP_ADC())                    // RRA
            URMW_GROUP(0xc0, v--; OP_CMP(a))                        // DCP
            URMW_GROUP(0xe0, v++; OP_SBC())                         // ISC

            // SAX
            case 0x83: AM_IZX(); WR(ea, a & x); break;
            case 0x87: AM_ZP(); WR(ea, a & x); break;
            case 0x8f: AM_ABS(); WR(ea, a & x); break;
            case 0x97: AM_ZPY(); WR(ea, a & x); break;
            // LAX
            case 0xa3: READ_OP(AM_IZX(), a = x = v; SETNZ(a)); break;
            case 0xa7: READ_OP(AM_ZP(), a = x = v; SETNZ(a)); break;
            case 0xaf: READ_OP(AM_ABS(), a = x = v; SETNZ(a)); break;
            case 0xb3: READ_OP(AM_IZY_R(), a = x = v; SETNZ(a)); break;
            case 0xb7: READ_OP(AM_ZPY(), a = x = v; SETNZ(a)); break;
            case 0xbf: READ_OP(AM_ABI_R(y), a = x = v; SETNZ(a)); break;

            // Immediate
            case 0x0b: case 0x2b:                                   // ANC
                RD(v, pc); pc++; a &= v; SETNZ(a); fc = a >> 7; break;
            case 0x4b:                                              // ALR
                RD(v, pc); pc++; v &= a; OP_LSR(); a = v; break;
            case 0x6b:                                              // ARR
                RD(v, pc);
                pc++;
                if (fd) {
                    uint8_t t_ = a & v;
                    uint8_t r_ = (uint8_t)((t_ >> 1) | (fc << 7));
                    fn = r_;    // N = old carry
                    fz = r_;
                    fv = ((t_ ^ r_) >> 6) & 1;
                    if (((t_ & 0x0f) + (t_ & 0x01)) > 5) r_ = (r_ & 0xf0) | ((r_ + 6) & 0x0f);
                    if (((t_ & 0xf0) + (t_ & 0x10)) > 0x50) {
                        r_ += 0x60;
                        fc = 1;
                    } else {
                        fc = 0;
                    }
                    a = r_;
                } else {
                    a = (uint8_t)(((a & v) >> 1) | (fc << 7));
                    SETNZ(a);
                    fc = (a >> 6) & 1;
                    fv = ((a >> 6) ^ (a >> 5)) & 1;
                }
                break;
            case 0x8b:                                              // ANE (unstable)
                RD(v, pc); pc++; a = (a | C6510_ANE_MAGIC) & x & v; SETNZ(a); break;
            case 0xab:                                              // LXA (unstable)
                RD(v, pc); pc++; a = x = (a | C6510_LXA_MAGIC) & v; SETNZ(a); break;
            case 0xcb:                                              // SBX
                RD(v, pc); pc++; t = (a & x) - v; fc = t < 0x100; x = (uint8_t)t; SETNZ(x); break;
            case 0xeb:                                              // SBC #imm
                RD(v, pc); pc++; OP_SBC(); break;

            // Unstable stores and LAS
            case 0x93: AM_IZY_W(); SH_STORE(a & x); break;          // SHA (zp),y
            case 0x9f: AM_ABI_W(y); SH_STORE(a & x); break;         // SHA abs,y
            case 0x9e: AM_ABI_W(y); SH_STORE(x); break;             // SHX abs,y
            case 0x9c: AM_ABI_W(x); SH_STORE(y); break;             // SHY abs,x
            case 0x9b: AM_ABI_W(y); s = a & x; SH_STORE(s); break;  // TAS abs,y
            case 0xbb: READ_OP(AM_ABI_R(y), a = x = s = v & s; SETNZ(a)); break; // LAS

            // NOPs
            case 0x1a: case 0x3a: case 0x5a: case 0x7a: case 0xda: case 0xfa:
                IMPLIED((void)0); break;
            case 0x80: case 0x82: case 0x89: case 0xc2: case 0xe2:
                RD(v, pc); pc++; break;
            case 0x04: case 0x44: case 0x64:
                READ_OP(AM_ZP(), (void)0); break;
            case 0x14: case 0x34: case 0x54: case 0x74: case 0xd4: case 0xf4:
                READ_OP(AM_ZPX(), (void)0); break;
            case 0x0c:
                READ_OP(AM_ABS(), (void)0); break;
            case 0x1c: case 0x3c: case 0x5c: case 0x7c: case 0xdc: case 0xfc:
                READ_OP(AM_ABI_R(x), (void)0); break;

            // JAM: the CPU stops (only RESET restarts it).
            default:
                pc--;
                cpu->jammed = 1;
                cyc = until;
                break;
        }
    }

    cpu->cycles = cyc;
    cpu->pc = pc;
    cpu->a = a;
    cpu->x = x;
    cpu->y = y;
    cpu->s = s;
    cpu->fn = fn;
    cpu->fz = fz;
    cpu->fc = fc;
    cpu->fv = fv;
    cpu->fd = fd;
    cpu->fi = fi;
    cpu->idelay = idelay;
    cpu->iold = iold;
    return cyc;
}

#undef RD
#undef WR
#undef DUM
#undef PUSH
#undef SETNZ
#undef GETP
#undef SETP
#undef AM_ZP
#undef AM_ZPX
#undef AM_ZPY
#undef AM_ABS
#undef AM_ABI_R
#undef AM_ABI_W
#undef AM_IZX
#undef AM_IZY_R
#undef AM_IZY_W
#undef OP_ADC
#undef OP_SBC
#undef OP_CMP
#undef OP_ASL
#undef OP_LSR
#undef OP_ROL
#undef OP_ROR
#undef READ_OP
#undef RMW_OP
#undef IMPLIED
#undef BRANCH
#undef SH_STORE
#undef READ_GROUP
#undef RMW_GROUP
#undef URMW_GROUP
