#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# machine.I2C, ADC (also at a fixed rate), PWM, SPI, UART and Timer served by the Cortex-M4 of the STM32MP157C-DK2, with
# nothing wired to the connectors: the bus is empty, the internal reference is known, the M4 measures the
# PWM and the UART and watches the SPI on its own pins, the UART loops back on a single wire.
#
#   P=/dev/ttyACM1 tests/hil/test_m4.sh
set -euo pipefail
P=${P:-/dev/ttyACM1}
echo "Serial target: $P"

mpremote connect "$P" exec "
import errno, time, machine, mpy_m4

def fails(errno_wanted, call, *args, **kwargs):
    try:
        call(*args, **kwargs)
    except OSError as e:
        assert e.errno == errno_wanted, (call, e)
        return
    raise AssertionError('no OSError %d from %r' % (errno_wanted, call))

assert machine.Pin and machine.I2C and machine.ADC
print('firmware:', mpy_m4.info(), 'over', mpy_m4.link.transport)
assert mpy_m4.link.transport.startswith('rpmsg_char'), mpy_m4.link.transport

i2c = machine.I2C(5, freq=100000)
assert i2c.freq == 100000, i2c.freq
i2c.init(freq=400000)
assert i2c.freq == 400000, i2c.freq
start = time.ticks_ms()
assert i2c.scan() == [], 'something answers on I2C5: unplug it for this test'
print('PASS: empty I2C5 scanned in %d ms' % time.ticks_diff(time.ticks_ms(), start))
fails(errno.ENODEV, i2c.writeto, 0x50, b'\x00')
fails(errno.ENODEV, i2c.readfrom, 0x50, 4)
fails(errno.ENODEV, i2c.readfrom_mem, 0x68, 0x75, 1)
fails(errno.ENODEV, i2c.writeto_mem, 0x50, 0x10, b'ab', addrsize=16)
i2c.writeto(0x50, b'\x00', False)
fails(errno.ENODEV, i2c.readfrom, 0x50, 1)  # register address, repeated start, read
print('PASS: an address nobody answers is ENODEV')
for bad in (lambda: machine.I2C(1), lambda: i2c.readfrom(0x50, 0), lambda: i2c.readfrom_mem(0x50, 0, 1, addrsize=12)):
    try:
        bad()
        raise AssertionError('no ValueError')
    except ValueError:
        pass
print('PASS: bad arguments are ValueError')

vref = machine.ADC(machine.ADC.CORE_VREF)
values = [vref.read_uv() for _ in range(20)]
assert 1150000 < min(values) and max(values) < 1270000, values
print('PASS: VREFINT %d..%d uV (datasheet 1.18..1.24 V)' % (min(values), max(values)))
assert 400000 < machine.ADC(machine.ADC.CORE_TEMP).read_uv() < 900000
for name in ('A0', 'A1', 'A2', 'A3', 'A4', 'A5', 'PF14', 'ANA0'):
    assert 0 <= machine.ADC(name).read_u16() <= 65535
print('PASS: A0..A5 convert')
try:
    machine.ADC('PA0')
    raise AssertionError('no ValueError')
except ValueError:
    pass
start = time.ticks_us()
for _ in range(500):
    vref.read_u16()
print('PASS: %d us a reading' % (time.ticks_diff(time.ticks_us(), start) // 500))

# PWM on PE9 (Arduino D6), measured on the pin by the M4 itself.
def near(value, wanted, tolerance):
    assert abs(value - wanted) <= tolerance, (value, wanted)
pwm = machine.PWM('D6', freq=1000, duty_u16=16384)
f, d = mpy_m4.measure('D6', 200)
near(f, 1000, 0.1); near(d, 0.25, 0.001)
pwm.duty_ns(100000)
pwm.freq(2000)
f, d = mpy_m4.measure('D6', 200)
near(f, 2000, 0.1); near(d, 0.2, 0.001)
assert pwm.duty_ns() == 100000, pwm.duty_ns()
pwm.init(freq=50, duty_ns=1500000)
f, d = mpy_m4.measure('PE9', 200)
near(f, 50, 0.01); near(d, 0.075, 0.001)
for duty in (0, 65535):
    pwm.init(freq=1000, duty_u16=duty)
    time.sleep_ms(2)
    f, d = mpy_m4.measure('D6', 50)
    assert f == 0 and d == duty // 65535, (duty, f, d)
pwm.freq(100000)
pwm.duty_u16(19661)
f, d = mpy_m4.measure('D6', 100)
near(f, 100000, 20); near(d, 0.3, 0.005)  # 80 samples a period: an interrupt shows
pwm.deinit()
assert mpy_m4.measure('D6', 20) == (0, 0)
for bad in (lambda: machine.PWM('PA14'), lambda: machine.PWM('D6', invert=True), lambda: machine.PWM('D6', freq=0)):
    try:
        bad()
        raise AssertionError('no ValueError')
    except ValueError:
        pass
print('PASS: PWM 1 kHz 25%, 2 kHz 100 us, 50 Hz 1.5 ms, 0% and 100%, 100 kHz 30%, deinit')

# SPI4: the M4 watches its own MOSI and SCK pins during a transfer (up to 1 MHz); MISO reads its
# internal pull-up or pull-down. The chip select is a Linux GPIO line (PE11, D10).
cs = machine.Pin('PE11', machine.Pin.OUT, value=1)
spi = machine.SPI(4, baudrate=1000000, _miso_pull=1)
assert spi.baudrate == 1000000, spi.baudrate
for polarity in (0, 1):
    for phase in (0, 1):
        for firstbit in (machine.SPI.MSB, machine.SPI.LSB):
            spi.init(baudrate=500000, polarity=polarity, phase=phase, firstbit=firstbit)
            cs(0)
            seen, edges = spi._watch(b'\xa5\x3c\x01\x80MicroPython')
            cs(1)
            assert seen == b'\xa5\x3c\x01\x80MicroPython', (polarity, phase, firstbit, seen)
            assert edges in (2 * 8 * 15 - 1, 2 * 8 * 15), edges  # the last edge may end with the transfer
assert spi.read(4, 0x9F) == b'\xff' * 4
buf = bytearray(4)
spi.write_readinto(b'abcd', buf)
assert buf == b'\xff' * 4
spi = machine.SPI(4, baudrate=1000000, _miso_pull=2)
assert spi.read(3) == b'\x00' * 3
spi.init(baudrate=100000000)
assert spi.baudrate == 32000000, spi.baudrate
spi.write(bytes(1000))
spi.deinit()
cs.deinit()
for bad in (lambda: machine.SPI(1), lambda: machine.SPI(4, bits=16), lambda: machine.SPI(4, sck='PA1')):
    try:
        bad()
        raise AssertionError('no ValueError')
    except ValueError:
        pass
print('PASS: SPI4 in the 8 modes seen on the pins, MISO pull-up and pull-down, 32 MHz, CS on Linux')

# UART7 in single-wire mode: TX (PE8, D1) also receives what it sends; the M4 measures the baud
# rate on the pin (0x55 at 8N1 is a square wave at half the baud rate).
for baud, bits, parity, stop in ((9600, 8, None, 1), (9600, 7, 0, 2), (19200, 8, 1, 1), (1000000, 8, None, 1)):
    u = machine.UART(7, baud, bits=bits, parity=parity, stop=stop, timeout=200, _single_wire=True)
    u.write(b'machine.UART')
    assert u.read(12) == b'machine.UART', (baud, bits, parity, stop)
    assert u.errors() == (0, 0, 0), u.errors()
u = machine.UART(7, 115200, timeout=200, _single_wire=True)
u.write(b'one\ntwo\n')
assert u.readline() == b'one\n' and u.readline() == b'two\n'
start = time.ticks_ms()
assert u.read(1) is None
near(time.ticks_diff(time.ticks_ms(), start), 200, 30)
data = bytes(range(256)) * 12  # more than the M4's 2 KB to send
assert u.write(data) == len(data)
assert u.read(len(data)) == data
u.flush()
assert u.txdone()
u = machine.UART(7, 9600)
u.write(b'U' * 400)
f, d = mpy_m4.measure('D1', 300)
near(2 * f, 9600, 9600 * 0.005)
u.deinit()
for bad in (lambda: machine.UART(1), lambda: machine.UART(7, bits=9, parity=0), lambda: machine.UART(7, tx='PA1')):
    try:
        bad()
        raise AssertionError('no ValueError')
    except ValueError:
        pass
print('PASS: UART7 8N1, 7E2, 8O1 and 1 Mbaud looped back, readline, timeout, 3 KB, 9600 baud on the pin')

# Timers: the M4 counts, Linux runs the callbacks; events wait for Linux's acknowledgement.
calls = []
t = machine.Timer(0, period=10, callback=lambda t: calls.append(time.ticks_us()))
time.sleep_ms(1000)
t.deinit()
near(len(calls) + t.missed, 100, 2)
gaps = sorted(time.ticks_diff(b, a) for a, b in zip(calls, calls[1:]))
near(gaps[len(gaps) // 2], 10000, 300)
shot = []
start = time.ticks_us()
machine.Timer(-1, mode=machine.Timer.ONE_SHOT, period=50, callback=lambda t: shot.append(time.ticks_diff(time.ticks_us(), start)))
time.sleep_ms(150)
assert len(shot) == 1 and 49000 < shot[0] < 60000, shot
reads = []
t = machine.Timer(1, freq=100, callback=lambda t: reads.append(machine.ADC(machine.ADC.CORE_VREF).read_uv()))
start = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), start) < 300:
    machine.ADC('A0').read_u16()  # requests of the main thread beside the callbacks' own
t.deinit()
assert 25 <= len(reads) <= 32 and all(1150000 < v < 1270000 for v in reads), reads
calls = []
t = machine.Timer(2, freq=1000, callback=lambda t: calls.append(1))
start = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), start) < 500:
    pass  # a busy main thread: callbacks wait, the M4 counts, nothing piles up
time.sleep_ms(50)
t.deinit()
assert len(calls) < 100 and 400 < len(calls) + t.missed <= 560, (len(calls), t.missed)
for bad in (lambda: machine.Timer(8), lambda: machine.Timer(0, period=0), lambda: machine.Timer(0, freq=2000)):
    try:
        bad()
        raise AssertionError('no ValueError')
    except ValueError:
        pass
print('PASS: Timer 10 ms (median gap %d us), one shot 50 ms, requests in callbacks, 1 kHz under a busy main thread' % gaps[len(gaps) // 2])

# ADC sampling at a fixed rate: TIM2 triggers the conversions, the M4 keeps the samples.
import array
vref = machine.ADC(machine.ADC.CORE_VREF)
for count, rate in ((1000, 1000), (32768, 100000)):
    samples = array.array('H', bytes(2 * count))
    start = time.ticks_ms()
    vref.read_timed(samples, rate)
    near(vref.freq, rate, rate * 0.0002)
    assert vref.overruns == 0
    assert all(1150000 < v * 2500000 // 65535 < 1270000 for v in samples), (min(samples), max(samples))
    elapsed = time.ticks_diff(time.ticks_ms(), start)
    assert elapsed < count * 1000 // rate + 1000, (count, rate, elapsed)  # a hang, not speed
ticks = []
t = machine.Timer(0, period=10, callback=lambda t: ticks.append(1))
vref.read_timed(array.array('H', bytes(2 * 5000)), 10000)
t.deinit()
assert len(ticks) >= 40, len(ticks)  # the rest of Python goes on during a capture
# A real signal: Linux charges A2 (PF14) as an output, the capture lets it go; each conversion
# takes a share of the charge: a falling curve, sample by sample.
pin = machine.Pin('PF14', machine.Pin.OUT, value=1)
time.sleep_ms(5)
curve = array.array('H', bytes(16))
machine.ADC('A2').read_timed(curve, 50000)
pin.deinit()
assert curve[0] > 40000 and all(a > b for a, b in zip(curve, curve[1:])), list(curve)
eight = bytearray(10)
vref.read_timed(eight, 1000)
assert all(115 < v < 131 for v in eight), eight
assert 1150000 < vref.read_uv() < 1270000  # single reads after a capture
for bad in (lambda: vref.read_timed(bytearray(0), 1000), lambda: vref.read_timed(bytearray(10), 200000)):
    try:
        bad()
        raise AssertionError('no ValueError')
    except ValueError:
        pass
print('PASS: read_timed 1 kHz and 32768 at 100 kHz, a discharge on A2, timers meanwhile')

# Logic analyser: the M4 samples port E while its own UART7 and SPI4 send (started by the capture).
def uart_bytes(samples, bit, rate, baud):
    mask = 1 << bit; per_bit = rate / baud; out = []; i = 1
    while i < len(samples):
        if not samples[i] & mask and samples[i - 1] & mask:
            stop = int(i + per_bit * 9.5)
            if stop >= len(samples):
                break
            if samples[stop] & mask:
                out.append(sum(1 << k for k in range(8) if samples[int(i + per_bit * (1.5 + k))] & mask))
                i = stop
                continue
        i += 1
    return bytes(out)
machine.UART(7, 115200).deinit()
machine.UART(7, 115200)
samples, rate, late = mpy_m4.logic_capture(2000, 1000000, uart=b'Hello, M4!')
assert uart_bytes(samples, 8, rate, 115200) == b'Hello, M4!', uart_bytes(samples, 8, rate, 115200)
machine.SPI(4, baudrate=250000)
samples, rate, late = mpy_m4.logic_capture(1000, 2000000, spi=b'\xa5\x3c')
rising = [i for i in range(1, len(samples)) if samples[i] & 0x1000 and not samples[i - 1] & 0x1000]
assert len(rising) == 16, len(rising)
value = 0
for i in rising:
    value = value << 1 | (samples[i] >> 14) & 1
assert value == 0xA53C, hex(value)
pwm = machine.PWM('D6', freq=10000, duty_u16=19661)
samples, rate, late = mpy_m4.logic_capture(2000, 1000000)
pwm.deinit()
rising = [i for i in range(1, len(samples)) if samples[i] & 0x200 and not samples[i - 1] & 0x200]
near(rate * (len(rising) - 1) / (rising[-1] - rising[0]), 10000, 10)
print('PASS: logic analyser: its own UART Hello, SPI A5 3C, PWM 10 kHz on port E')

mpy_m4.led(True)
time.sleep_ms(200)
mpy_m4.led(False)
p = machine.Pin('PA14', machine.Pin.IN, machine.Pin.PULL_UP)
assert p.value() in (0, 1)
p.deinit()
print('PASS: LED and machine.Pin beside the M4')
"
echo "PASS: Cortex-M4 HIL tests completed"
