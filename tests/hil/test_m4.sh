#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# machine.I2C and machine.ADC served by the Cortex-M4 of the STM32MP157C-DK2, with nothing wired
# to the connectors: the bus is empty, the internal reference is known.
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

mpy_m4.led(True)
time.sleep_ms(200)
mpy_m4.led(False)
p = machine.Pin('PA14', machine.Pin.IN, machine.Pin.PULL_UP)
assert p.value() in (0, 1)
p.deinit()
print('PASS: LED and machine.Pin beside the M4')
"
echo "PASS: Cortex-M4 HIL tests completed"
