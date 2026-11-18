// SPDX-License-Identifier: MIT
// The protocol between MicroPython on Linux (python/linux/mpy_m4.py) and the Cortex-M4 firmware,
// over RPMsg: the raw channel rpmsg-raw (/dev/rpmsgN through rpmsg_char), one frame a message, or
// the virtual serial port rpmsg-tty (/dev/ttyRPMSGN), a stream of frames. Little endian.
//
//   request:  length u16, sequence u8, operation u8, arguments
//   answer:   length u16, sequence u8, status u8, result
//   event:    length u16, 0, kind u8, data          (sent by the M4 unasked)
//
// length counts the bytes after itself. The answer repeats the request's sequence number, never 0:
// sequence 0 marks an event, which can come between a request and its answer. status
// is 0 or a Linux errno value, which Python raises as OSError: ENODEV when no device answers an
// I2C address, EIO when one stops answering, ETIMEDOUT, EINVAL for bad arguments, EOPNOTSUPP for
// an unknown operation.
#ifndef MPY_M4_PROTOCOL_H
#define MPY_M4_PROTOCOL_H

#define MPY_M4_PROTOCOL_VERSION 2  // 2: events (timers), acknowledged
#define MPY_M4_MAX_FRAME 480  // what one RPMsg buffer carries (496 bytes, less the header)
#define MPY_M4_MAX_DATA 400   // bytes of one I2C transfer
#define MPY_M4_CAPTURE 32768  // samples of one ADC capture (64 KB of the M4's 128 KB of data RAM)
#define MPY_M4_CAPTURE_MAX_HZ 100000

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
    // Sampling at a fixed rate: TIM2 triggers the conversions in hardware, an interrupt stores
    // them in the M4's buffer (MPY_M4_CAPTURE samples); Linux collects them afterwards.
    OP_ADC_CAPTURE = 0x21,     // ADC u8, channel u8, port u8, pin u8, samples u32, rate Hz u32
                               //   -> TIM2 clock Hz u32, period u32 (rate = clock / period),
                               //      sampling time in ADC cycles x10 u32
    OP_ADC_CAPTURE_STATUS = 0x22, // -> state u8 (0 idle, 1 running, 2 done), samples taken u32,
                               //    cycles from the first sample to the last u32, overruns u32
    OP_ADC_CAPTURE_FETCH = 0x23,  // first u32, count u16 (<= 200) -> samples u16 each

    // TIM1 channel 1 on PE9 (Arduino D6). PWM_INIT sets the frequency and the duty (65535:
    // always high) from the first period; PWM_COMPARE sets the high time in timer ticks (period
    // ticks: always high), from the next period.
    OP_PWM_INIT = 0x30,        // frequency u32, duty u16 -> frequency u32, period ticks u32, tick Hz u32
    OP_PWM_COMPARE = 0x31,     // ticks u32
    OP_PWM_DEINIT = 0x32,      // the pin back to analog, the timer off
    // A melody on the same PWM: up to 100 notes, each a frequency (0: a rest) and a duration, played
    // by the M4 on time while it answers other requests. PWM_INIT and PWM_DEINIT stop it.
    OP_TONE_SEQUENCE = 0x33,   // notes: Hz u16, ms u16 each
    OP_TONE_STATUS = 0x34,     // -> note playing i16 (-1: none), notes u16

    // SPI4: SCK PE12 (Arduino D13), MISO PE13 (D12), MOSI PE14 (D11); 8-bit frames. Chip select
    // is the caller's (PE11, D10, is free for a Linux GPIO line).
    OP_SPI_INIT = 0x50,        // baud rate u32, mode u8 (bit 0 CPOL, bit 1 CPHA, bit 2 LSB first),
                               //   MISO pull u8 (0 none, 1 up, 2 down) -> baud rate u32
    OP_SPI_TRANSFER = 0x51,    // flags u8 (bit 0 answer the bytes read, bit 1 watch the pins), data
                               //   -> bytes read (bit 0), then with bit 1: the bytes seen on MOSI
                               //      at the sampling edges of SCK, and the SCK edges u32
    OP_SPI_DEINIT = 0x52,      // the pins back to analog, SPI4 off

    // UART7: RX PE7 (Arduino D0), TX PE8 (D1). Received bytes wait in a 4 KB buffer of the M4,
    // bytes to send in a 2 KB one; both move under interrupt.
    OP_UART_INIT = 0x60,       // baud rate u32, data bits u8 (7, 8, 9), parity u8 (0 none, 1 odd,
                               //   2 even), stop bits u8 (1, 2), flags u8 (bit 0: single wire on
                               //   TX, which also receives what it sends) -> baud rate u32
    OP_UART_WRITE = 0x61,      // data -> bytes taken u16 (fewer when the buffer is full)
    OP_UART_READ = 0x62,       // count u16 -> up to count bytes
    OP_UART_STATUS = 0x63,     // -> bytes waiting u16, bytes to send u16, all sent u8,
                               //    overruns u16, framing errors u16, parity errors u16
    OP_UART_BREAK = 0x64,
    OP_UART_DEINIT = 0x65,     // the pins back to analog, UART7 off

    // Timers of the M4 (8, numbered 0 to 7), on the cycle counter: at each expiry an event
    // EVENT_TIMER. Expiries the M4 could not report in time are counted in the next event.
    OP_TIMER_INIT = 0x70,      // timer u8, mode u8 (0 one shot, 1 periodic), period us u32 (>= 1000)
    OP_TIMER_DEINIT = 0x71,    // timer u8 (0xFF: all)
    OP_EVENT_ACK = 0x72,       // Linux has handled the events so far: each timer may send its next
                               //   one. Until then a timer counts its expiries into that next event,
                               //   so a busy Linux never piles events up in the serial port.

    // What a pin does, sampled by the M4 for a while: for tests without instruments.
    // A whole GPIO port sampled at a fixed rate on the cycle counter, into the capture buffer
    // (collected with ADC_CAPTURE_FETCH): a logic analyser. A stimulus can start with it, so the
    // board can watch its own buses: bytes for SPI4 or UART7 (set up first), fed between
    // samples.
    OP_PORT_CAPTURE = 0x41,    // port u8, rate Hz u32, samples u32, stimulus u8 (0 none, 1 SPI4,
                               //   2 UART7), bytes -> core Hz u32, cycles a sample u32, late u32
    OP_PIN_MEASURE = 0x40,     // port u8, pin u8, milliseconds u16
                               //   -> samples u32, cycles high u32, rising edges u32,
                               //      cycles u32, core Hz u32, first and last rising edge (cycles
                               //      from the start) u32 u32
};

// Events (the status byte of a frame with sequence 0).
#define EVENT_TIMER 1  // timer u8, expiries u32, late us u32 (from the first expiry to the sending)
#define MPY_M4_TIMERS 8
#define MPY_M4_TIMER_MIN_US 1000

// Channels of ADC_READ beyond the inputs 0 to 19; ADC2 only.
#define ADC_CHANNEL_CODE_VREFINT 0x80
#define ADC_CHANNEL_CODE_TEMPSENSOR 0x81
#define ADC_NO_PIN 0xFF  // port and pin, for internal channels and analog pads (ANA0, ANA1)

// Status values: Linux's errno numbers (the firmware's C library numbers some differently).
#define E_IO 5
#define E_NODEV 19
#define E_BUSY 16
#define E_INVAL 22
#define E_OPNOTSUPP 95
#define E_TIMEDOUT 110

#endif
