# SPDX-License-Identifier: MIT
# machine.I2C, machine.ADC and machine.PWM served by the Cortex-M4 of an STM32MP15 board, as on a
# microcontroller: the same MicroPython code runs on both.
#
#   from machine import I2C, ADC
#   i2c = I2C(5, freq=400000)        # I2C5: Arduino D14 SDA / D15 SCL, 40-pin header pins 3 and 5
#   i2c.scan()
#   ADC("A0").read_uv()              # Arduino A0..A5, pin names ("PF14") or ADC.CORE_VREF
#   PWM("D6", freq=1000, duty_u16=16384)   # TIM1 channel 1 on PE9 (Arduino D6)
#
# The firmware (mcu/stm32mp157c-dk2, mpy_m4.elf) runs on the M4, loaded by Linux through
# remoteproc; the two talk over an RPMsg virtual serial port with the protocol of
# mcu/stm32mp157c-dk2/firmware/Inc/mpy_m4_protocol.h. An error of the M4 is an OSError with the
# errno a microcontroller gives: ENODEV when no device answers an address, ETIMEDOUT, EIO.

import os
import select
import struct
import termios
import time

REMOTEPROC = "/sys/class/remoteproc/remoteproc0"
FIRMWARE = "mpy_m4.elf"
PROTOCOL = 1

_INFO = 0x01
_LED = 0x02
_I2C_INIT = 0x10
_I2C_SCAN = 0x11
_I2C_WRITE = 0x12
_I2C_READ = 0x13
_I2C_MEM_READ = 0x14
_I2C_MEM_WRITE = 0x15
_ADC_READ = 0x20
_PWM_INIT = 0x30
_PWM_COMPARE = 0x31
_PWM_DEINIT = 0x32
_PIN_MEASURE = 0x40

_MAX_DATA = 400
_EOPNOTSUPP = 95
_ETIMEDOUT = 110


def _read_text(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def available():
    # True on a board with a Cortex-M4 that Linux drives (STM32MP15x).
    return _read_text(REMOTEPROC + "/name") == "m4"


def _start():
    # Load the firmware if the M4 is off; root is needed (mpy-repl.service runs as root).
    if _read_text(REMOTEPROC + "/state") == "running":
        return
    with open(REMOTEPROC + "/firmware", "w") as f:
        f.write(FIRMWARE)
    with open(REMOTEPROC + "/state", "w") as f:
        f.write("start")


def _port():
    names = sorted(n for n in os.listdir("/dev") if n.startswith("ttyRPMSG"))
    return "/dev/" + names[0] if names else None


class _Link:
    # One request at a time, answered by the same sequence number.

    def __init__(self):
        self._file = None
        self._sequence = 0

    def _open(self):
        if not available():
            raise OSError(19, "no Cortex-M4 on this board")
        _start()
        port = None
        for _ in range(40):  # the virtual serial port appears once the firmware is up
            port = _port()
            if port:
                break
            time.sleep_ms(50)
        if not port:
            raise OSError(19, "the Cortex-M4 firmware does not answer (no /dev/ttyRPMSG*)")
        self._file = open(port, "r+b", buffering=0)
        termios.setraw(self._file.fileno())
        self._poll = select.poll()
        self._poll.register(self._file.fileno(), select.POLLIN)
        while self._poll.poll(0):  # leftovers of an earlier session
            self._file.read(256)
        info = self.request(_INFO)
        if info[0] != PROTOCOL:
            raise OSError(_EOPNOTSUPP, "Cortex-M4 firmware speaks protocol %d, not %d" % (info[0], PROTOCOL))
        self.firmware = info[1:].decode()

    def close(self):
        if self._file:
            self._file.close()
            self._file = None

    def _read(self, count, timeout_ms):
        data = b""
        while len(data) < count:
            if not self._poll.poll(timeout_ms):
                raise OSError(_ETIMEDOUT, "the Cortex-M4 did not answer")
            data += self._file.read(count - len(data))
        return data

    def request(self, operation, arguments=b"", timeout_ms=1000):
        if self._file is None:
            self._open()
        self._sequence = (self._sequence + 1) & 0xFF
        try:
            self._file.write(struct.pack("<HBB", 2 + len(arguments), self._sequence, operation) + arguments)
            while True:
                length = struct.unpack("<H", self._read(2, timeout_ms))[0]
                answer = self._read(length, timeout_ms)
                if answer[0] == self._sequence:
                    break  # anything else answers a request given up on
        except OSError:
            self.close()  # out of step: open again at the next request
            raise
        if answer[1]:
            raise OSError(answer[1])
        return answer[2:]


link = _Link()


def info():
    # The firmware's name, starting the M4 if needed.
    link.request(_INFO)
    return link.firmware


def led(on):
    # The orange LED, which the device tree gives the M4.
    link.request(_LED, bytes([1 if on else 0]))


class I2C:
    # machine.I2C on the M4's I2C5. Bus 5 only: SDA PA12, SCL PA11.

    def __init__(self, id=5, *, scl=None, sda=None, freq=400000, timeout=None):
        if id != 5:
            raise ValueError("I2C(%r) doesn't exist: the Cortex-M4 has I2C5" % (id,))
        for pin, name in ((scl, "PA11"), (sda, "PA12")):
            if pin is not None and _pin_name(pin) != name:
                raise ValueError("I2C5 uses SCL PA11 and SDA PA12")
        self._pending = None  # bytes written with stop=False, waiting for the read that follows
        self.init(freq=freq)

    def init(self, *, freq=400000, **ignored):
        self.freq = struct.unpack("<I", link.request(_I2C_INIT, struct.pack("<I", freq)))[0]

    def __repr__(self):
        return "I2C(5, freq=%d)" % self.freq

    def deinit(self):
        pass

    def scan(self):
        self._flush()
        return list(link.request(_I2C_SCAN))

    def _flush(self):
        if self._pending:
            address, data = self._pending
            self._pending = None
            self._write(address, data)

    def _write(self, address, data):
        return struct.unpack("<H", link.request(_I2C_WRITE, bytes([address]) + data))[0]

    def writeto(self, addr, buf, stop=True):
        self._flush()
        data = bytes(buf)
        if len(data) > _MAX_DATA:
            raise ValueError("at most %d bytes at a time" % _MAX_DATA)
        if not stop:
            # Kept for the read that follows (a register address, then a repeated start).
            self._pending = (addr, data)
            return len(data)
        return self._write(addr, data)

    def writevto(self, addr, vector, stop=True):
        return self.writeto(addr, b"".join(bytes(b) for b in vector), stop)

    def readfrom(self, addr, nbytes, stop=True):
        if not 0 < nbytes <= _MAX_DATA:
            raise ValueError("1 to %d bytes at a time" % _MAX_DATA)
        if self._pending:
            address, data = self._pending
            self._pending = None
            if address == addr and 0 < len(data) <= 2:
                return self.readfrom_mem(addr, int.from_bytes(data, "big"), nbytes, addrsize=8 * len(data))
            if address == addr:
                raise OSError(_EOPNOTSUPP, "a repeated start after more than 2 bytes")
            self._write(address, data)
        return link.request(_I2C_READ, struct.pack("<BH", addr, nbytes), 1000 + nbytes)

    def readfrom_into(self, addr, buf, stop=True):
        data = self.readfrom(addr, len(buf), stop)
        buf[:] = data

    def _memory(self, addr, memaddr, addrsize):
        if addrsize not in (8, 16):
            raise ValueError("addrsize is 8 or 16")
        return struct.pack("<BBH", addr, addrsize // 8, memaddr)

    def readfrom_mem(self, addr, memaddr, nbytes, *, addrsize=8):
        self._flush()
        if not 0 < nbytes <= _MAX_DATA:
            raise ValueError("1 to %d bytes at a time" % _MAX_DATA)
        return link.request(_I2C_MEM_READ, self._memory(addr, memaddr, addrsize) + struct.pack("<H", nbytes))

    def readfrom_mem_into(self, addr, memaddr, buf, *, addrsize=8):
        buf[:] = self.readfrom_mem(addr, memaddr, len(buf), addrsize=addrsize)

    def writeto_mem(self, addr, memaddr, buf, *, addrsize=8):
        self._flush()
        data = bytes(buf)
        if len(data) > _MAX_DATA:
            raise ValueError("at most %d bytes at a time" % _MAX_DATA)
        link.request(_I2C_MEM_WRITE, self._memory(addr, memaddr, addrsize) + data)


# Arduino digital pins the M4 uses (connector CN14/CN13, from the board's schematic).
_DIGITAL = {"D6": "PE9", "D14": "PA12", "D15": "PA11"}


def _pin_name(pin):
    # "PF14", an Arduino name, or the name inside a machine.Pin's repr: Pin('PF14', ...).
    if isinstance(pin, str):
        name = pin.upper()
    else:
        text = repr(pin)
        start = text.find("'")
        name = text[start + 1 : text.find("'", start + 1)] if start >= 0 else text
    return _DIGITAL.get(name, name)


def _port_pin(name):
    if len(name) < 3 or name[0] != "P" or not "A" <= name[1] <= "K" or not name[2:].isdigit() or int(name[2:]) > 15:
        raise ValueError("not a pin name: %r" % name)
    return ord(name[1]) - ord("A"), int(name[2:])


def measure(pin, ms=100):
    # What a pin does, sampled by the M4 for ms milliseconds (1 to 1000): (frequency in Hz, share
    # of the time it was high from 0 to 1). About 8 million samples a second: good up to a few
    # hundred kHz. For tests without instruments; the M4 does nothing else meanwhile.
    port, number = _port_pin(_pin_name(pin))
    answer = link.request(_PIN_MEASURE, struct.pack("<BBH", port, number, ms), ms + 1000)
    samples, high, rising, cycles, core = struct.unpack("<5I", answer)
    seconds = cycles / core
    return rising / seconds, high / cycles


class PWM:
    # machine.PWM on the M4's TIM1 channel 1: pin PE9, Arduino D6. 1 Hz to about 100 MHz; the
    # duty has 16 bits of resolution up to 3.2 kHz and fewer above (the timer's period in ticks).

    def __init__(self, dest, *, freq=None, duty_u16=None, duty_ns=None, invert=False):
        if _pin_name(dest) != "PE9":
            raise ValueError("PWM is on PE9 (Arduino D6): TIM1 channel 1 of the Cortex-M4")
        self._freq = 1000
        self._duty = ("u16", 0)  # what was asked last: kept when the frequency changes
        self._period = 0
        self.init(freq=freq, duty_u16=duty_u16, duty_ns=duty_ns, invert=invert)

    def init(self, *, freq=None, duty_u16=None, duty_ns=None, invert=False):
        if invert:
            raise ValueError("invert is not supported")
        if duty_u16 is not None and duty_ns is not None:
            raise ValueError("duty_u16 or duty_ns, not both")
        if duty_u16 is not None:
            self._duty = ("u16", self._check_u16(duty_u16))
        elif duty_ns is not None:
            self._duty = ("ns", max(0, int(duty_ns)))
        self._start(self._freq if freq is None else freq)

    def _check_u16(self, value):
        if not 0 <= value <= 65535:
            raise ValueError("duty_u16 is 0 to 65535")
        return int(value)

    def _start(self, freq):
        freq = int(freq)
        if freq < 1:
            raise ValueError("freq must be at least 1 Hz")
        kind, value = self._duty
        approx = value if kind == "u16" else min(65535, value * freq * 65535 // 1000000000)
        answer = link.request(_PWM_INIT, struct.pack("<IH", freq, approx))
        self._actual, self._period, self._tick = struct.unpack("<3I", answer)
        self._freq = freq
        if kind == "ns":
            self._compare(self._ns_ticks(value))  # exact, from the next period

    def _ns_ticks(self, ns):
        return min(self._period, (ns * self._tick + 500000000) // 1000000000)

    def _compare(self, ticks):
        link.request(_PWM_COMPARE, struct.pack("<I", ticks))
        self._ticks = ticks

    def __repr__(self):
        if not self._period:
            return "PWM(PE9, deinit)"
        return "PWM(PE9, freq=%d, duty_u16=%d)" % (self._actual, self.duty_u16())

    def deinit(self):
        link.request(_PWM_DEINIT)
        self._period = 0

    def freq(self, value=None):
        if value is None:
            return self._actual
        self._start(value)

    def duty_u16(self, value=None):
        if value is None:
            kind, duty = self._duty
            if kind == "u16":
                return duty
            return min(65535, self._ns_ticks(duty) * 65535 // self._period)
        self._duty = ("u16", self._check_u16(value))
        if not self._period:
            self._start(self._freq)
        else:
            self._compare(self._period * self._duty[1] // 65535)

    def duty_ns(self, value=None):
        if value is None:
            kind, duty = self._duty
            ticks = self._period * duty // 65535 if kind == "u16" else self._ns_ticks(duty)
            return ticks * 1000000000 // self._tick
        self._duty = ("ns", max(0, int(value)))
        if not self._period:
            self._start(self._freq)
        else:
            self._compare(self._ns_ticks(self._duty[1]))


_NO_PIN = 0xFF

# Analog inputs of the STM32MP157 that the DK2 brings out: pin -> (ADC, channel). The Arduino
# names follow the board's schematic (connector CN17).
_PINS = {
    "ANA0": (1, 0),
    "ANA1": (1, 1),
    "PF14": (2, 6),
    "PF13": (2, 2),
    "PC3": (1, 13),
    "PF12": (1, 6),
}
_ARDUINO = {"A0": "ANA0", "A1": "ANA1", "A2": "PF14", "A3": "PF13", "A4": "PC3", "A5": "PF12"}


class ADC:
    # machine.ADC on the M4: 16 bits, against the 2.5 V reference buffer (VREFBUF) that Linux
    # turns on for the M4. Inputs from 0 to 2.5 V.

    CORE_VREF = 0x80  # the internal reference, about 1.21 V
    CORE_TEMP = 0x81  # the temperature sensor's voltage
    VREF_UV = 2500000

    def __init__(self, source, *, atten=None):
        if source in (ADC.CORE_VREF, ADC.CORE_TEMP):
            self._args = bytes([2, source, _NO_PIN, _NO_PIN])
            self._name = "CORE_VREF" if source == ADC.CORE_VREF else "CORE_TEMP"
            return
        name = _pin_name(source)
        name = _ARDUINO.get(name, name)
        if name not in _PINS:
            raise ValueError("no ADC input on %s; inputs: %s" % (name, ", ".join(sorted(_ARDUINO))))
        number, channel = _PINS[name]
        if name.startswith("ANA"):
            port = pin = _NO_PIN  # dedicated analog pads, no GPIO to set
        else:
            port, pin = _port_pin(name)
        self._args = bytes([number, channel, port, pin])
        self._name = name

    def __repr__(self):
        return "ADC(%r)" % self._name

    def read_u16(self):
        return struct.unpack("<H", link.request(_ADC_READ, self._args))[0]

    def read_uv(self):
        return self.read_u16() * ADC.VREF_UV // 65535
