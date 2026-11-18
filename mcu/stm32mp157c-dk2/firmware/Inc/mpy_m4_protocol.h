// SPDX-License-Identifier: MIT
// The protocol between MicroPython on Linux (python/linux/mpy_m4.py) and the Cortex-M4 firmware,
// over the RPMsg virtual serial port /dev/ttyRPMSG0. Little endian.
//
//   request:  length u16, sequence u8, operation u8, arguments
//   answer:   length u16, sequence u8, status u8, result
//
// length counts the bytes after itself. The answer repeats the request's sequence number. status
// is 0 or a Linux errno value, which Python raises as OSError: ENODEV when no device answers an
// I2C address, EIO when one stops answering, ETIMEDOUT, EINVAL for bad arguments, EOPNOTSUPP for
// an unknown operation.
#ifndef MPY_M4_PROTOCOL_H
#define MPY_M4_PROTOCOL_H

#define MPY_M4_PROTOCOL_VERSION 1
#define MPY_M4_MAX_FRAME 480  // what one RPMsg buffer carries (496 bytes, less the header)
#define MPY_M4_MAX_DATA 400   // bytes of one I2C transfer

enum {
    OP_INFO = 0x01,            // -> version u8, text
    OP_LED = 0x02,             // on u8 (the orange LED, which the device tree gives the M4)

    OP_I2C_INIT = 0x10,        // frequency u32 -> frequency u32 (what the timing gives)
    OP_I2C_SCAN = 0x11,        // -> addresses that answer, u8 each
    OP_I2C_WRITE = 0x12,       // address u8, data -> bytes acknowledged u16
    OP_I2C_READ = 0x13,        // address u8, count u16 -> data
    OP_I2C_MEM_READ = 0x14,    // address u8, memory address size u8 (1, 2), memory u16, count u16
    OP_I2C_MEM_WRITE = 0x15,   // address u8, memory address size u8, memory u16, data

    OP_ADC_READ = 0x20,        // ADC u8 (1, 2), channel u8, port u8, pin u8 -> value u16
};

// Channels of ADC_READ beyond the inputs 0 to 19; ADC2 only.
#define ADC_CHANNEL_CODE_VREFINT 0x80
#define ADC_CHANNEL_CODE_TEMPSENSOR 0x81
#define ADC_NO_PIN 0xFF  // port and pin, for internal channels and analog pads (ANA0, ANA1)

// Status values: Linux's errno numbers (the firmware's C library numbers some differently).
#define E_IO 5
#define E_NODEV 19
#define E_INVAL 22
#define E_OPNOTSUPP 95
#define E_TIMEDOUT 110

#endif
