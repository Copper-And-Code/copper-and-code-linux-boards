# SPDX-License-Identifier: MIT
# machine on Linux's drivers (machine_kernel) on STM32MP135F-DK, without wires: every class the
# board's description names opens, answers and closes; what the board lacks is an error naming
# what it has. With jumpers on CN8 (MACHINE_LOOPBACK=1 in the environment) it also checks the
# data: SPI MOSI to MISO (pins 19 and 21), UART8 TX to RX (37 and 33), PWM TIM3 to A0 (7 and 8).
#
#   mpremote connect /dev/ttyACM2 run tests/hil/t22_mp135_machine_kernel.py
#
# Expected: MP135_MACHINE_KERNEL_PASS (and MP135_MACHINE_LOOPBACK_PASS with the jumpers;
# MACHINE_LOOPBACK=pwm, spi, uart or a list of them for those wired, 1 for the three).

import array
import os
import time

import machine
import mpy_board

assert mpy_board.describe().get("board") == "STM32MP135F-DK", "no board description"
for name in ("Pin", "I2C", "SPI", "UART", "PWM", "ADC", "Timer"):
    assert hasattr(machine, name), name

# I2C5: the board's own devices answer (the MIPI bridge at 0x14, the touch controller at 0x5d).
i2c = machine.I2C(5)
found = i2c.scan()
print(i2c, [hex(a) for a in found])
assert 0x14 in found and 0x5D in found, found
try:
    i2c.readfrom(0x77, 1)
    raise AssertionError("an empty address answered")
except OSError as error:
    assert error.errno == 19, error  # ENODEV, as on a microcontroller
i2c.deinit()

spi = machine.SPI(5, baudrate=2000000)
print(spi)
spi.write(b"\x55\xaa")
spi.deinit()

uart = machine.UART(8, 115200, timeout=20)
print(uart)
assert uart.write(b"ping") == 4
uart.flush()
uart.deinit()

pwm = machine.PWM("PB1", freq=1000, duty_u16=32768)
print(pwm)
assert pwm.freq() == 1000 and abs(pwm.duty_u16() - 32768) < 70
pwm.freq(20000)
assert pwm.freq() == 20000
pwm.deinit()

adc = machine.ADC("A0")
value = adc.read_u16()
print(adc, value, adc.read_uv())
assert 0 <= value <= 65535
samples = array.array("H", bytes(2000))
start = time.ticks_ms()
adc.read_timed(samples, 10000)
took = time.ticks_diff(time.ticks_ms(), start)
print("read_timed: 1000 samples at %d Hz in %d ms" % (adc.freq, took))
assert adc.freq == 10000 and 90 <= took < 400

calls = []
timer = machine.Timer(-1, period=10, callback=lambda t: calls.append(time.ticks_us()))
time.sleep_ms(500)
timer.deinit()
gaps = [time.ticks_diff(calls[i + 1], calls[i]) for i in range(len(calls) - 1)]
print("Timer: %d calls, %d to %d us apart, %d missed" % (len(calls), min(gaps), max(gaps), timer.missed))
assert 40 <= len(calls) <= 60 and timer.missed == 0
one = []
machine.Timer(-1, mode=machine.Timer.ONE_SHOT, period=20, callback=lambda t: one.append(1))
time.sleep_ms(100)
assert one == [1], one

for make, words in ((lambda: machine.PWM("PA0"), "PWM pins"), (lambda: machine.I2C(3), "buses"),
                    (lambda: machine.ADC("PB1"), "inputs")):
    try:
        make()
        raise AssertionError("no error")
    except ValueError as error:
        assert words in str(error), error
print("MP135_MACHINE_KERNEL_PASS")

# MACHINE_LOOPBACK=1 (or "all") checks the three; "spi", "uart", "pwm", or a list of them, those wired.
wired = os.getenv("MACHINE_LOOPBACK") or ""
wired = ("spi", "uart", "pwm") if wired in ("1", "all") else wired.split(",")
if "spi" in wired:
    spi = machine.SPI(5, baudrate=1000000)
    sent = bytes(range(64))
    got = bytearray(64)
    spi.write_readinto(sent, got)
    assert bytes(got) == sent, got
    spi.deinit()
    print("SPI5: 64 bytes back")
if "uart" in wired:
    uart = machine.UART(8, 115200, timeout=200)
    uart.read()
    uart.write(b"hello, UART8\n")
    assert uart.readline() == b"hello, UART8\n"
    uart.deinit()
    print("UART8: the line back")
if "pwm" in wired:
    # Steady high and low, then the duty seen by samples taken at a fixed rate. 97 kHz is no
    # multiple of the PWM's 20 kHz: the samples slide along its period and their share of highs is
    # the duty (at exactly 100 kHz they would hit the same 5 points of each period).
    pwm = machine.PWM("PB1", freq=1000, duty_u16=65535)
    time.sleep_ms(20)
    top = adc.read_uv()
    pwm.duty_u16(0)
    time.sleep_ms(20)
    bottom = adc.read_uv()
    print("PWM steady on A0: %.3f V high, %.3f V low" % (top / 1e6, bottom / 1e6))
    assert top > 3100000 and bottom < 100000
    pwm.init(freq=20000)
    trace = array.array("H", bytes(8000))
    for duty in (16384, 32768, 49152):
        pwm.duty_u16(duty)
        adc.read_timed(trace, 97000)
        seen = sum(1 for v in trace if v >= 32768) / len(trace)
        print("PWM 20 kHz at %d%%: %.3f of the samples high" % (duty * 100 // 65535, seen))
        assert abs(seen - duty / 65535) < 0.03
    pwm.deinit()
if wired and wired != [""]:
    print("MP135_MACHINE_LOOPBACK_PASS:", ",".join(wired))
