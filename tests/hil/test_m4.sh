#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# machine.I2C, ADC, PWM, SPI and UART served by the Cortex-M4 of the STM32MP157C-DK2, with nothing
# wired to the connectors: the bus is empty, the internal reference is known, the M4 measures the
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
print('firmware:', mpy_m4.info())

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
near(f, 100000, 20); near(d, 0.3, 0.002)
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

mpy_m4.led(True)
time.sleep_ms(200)
mpy_m4.led(False)
p = machine.Pin('PA14', machine.Pin.IN, machine.Pin.PULL_UP)
assert p.value() in (0, 1)
p.deinit()
print('PASS: LED and machine.Pin beside the M4')
"
echo "PASS: Cortex-M4 HIL tests completed"
