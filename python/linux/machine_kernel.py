# SPDX-License-Identifier: MIT
# machine.I2C, SPI, UART, PWM, ADC and Timer on Linux's own drivers, for a board without a
# microcontroller beside its processor: the same MicroPython code as on a microcontroller.
#
#   from machine import I2C, SPI, UART, PWM, ADC, Timer
#   I2C(5).scan()                    # /dev/i2c-N (i2c-dev)
#   SPI(5, baudrate=1000000)         # /dev/spidevB.C (spidev)
#   UART(8, 115200)                  # /dev/ttyX (termios)
#   PWM("PB1", freq=1000, duty_u16=32768)    # /sys/class/pwm
#   ADC("PA5").read_uv()             # IIO (/sys/bus/iio); read_timed() at a rate kept by a timer
#   Timer(-1, period=100, callback=f)        # a thread of this process, on Linux's clock
#
# Which bus, channel or line each name is comes from the board's description (mpy_board: a JSON
# file per board, linux/boards/ in the repository): a new board needs that file and the kernel's
# drivers, no code. Nothing here touches the registers.
#
# What Linux cannot promise is said, not hidden: a Timer callback runs when Linux runs this
# process (usually within a millisecond, later under load); a peripheral the board doesn't offer
# is an error naming those it has. The I2C bus speed is the device tree's (clock-frequency).

import os
import select
import struct
import time

import _thread

from mpy_board import describe as board

_EIO = 5
_ENODEV = 19
_ETIMEDOUT = 110


# --- the C library, for what MicroPython's os has not: ioctl, read and write without waiting ---

_libc = None
_funcs = {}


def _c(name, result, arguments):
    global _libc
    key = name
    if key not in _funcs:
        if _libc is None:
            import ffi

            _libc = ffi.open("libc.so.6")
        _funcs[key] = _libc.func(result, name, arguments)
    return _funcs[key]


def _errno():
    import uctypes

    address = _c("__errno_location", "p", "")()
    return struct.unpack("<i", uctypes.bytes_at(address, 4))[0]


def _check(result, what):
    if result < 0:
        raise OSError(_errno(), what)
    return result


def _open(path, flags):
    return _check(_c("open", "i", "si")(path, flags | 0o2000000), "cannot open " + path)  # O_CLOEXEC


def _close(fd):
    _c("close", "i", "i")(fd)


def _ioctl(fd, request, arg, what):
    return _check(_c("ioctl", "i", "iLp")(fd, request, arg), what)


def _address(buf):
    import uctypes

    return uctypes.addressof(buf)


# --- sysfs ---


def _read(path):
    with open(path) as f:
        return f.read().strip()


def _write(path, value):
    with open(path, "w") as f:
        f.write(str(value))


def _exists(path):
    try:
        os.stat(path)
        return True
    except OSError:
        return False


def _target(path):
    # Where a sysfs link leads, as the kernel wrote it ("" when it is no link).
    buf = bytearray(512)
    count = _c("readlink", "i", "spi")(path, buf, len(buf))
    return buf[:count].decode() if count > 0 else ""


def _find(folder, device, prefix=""):
    # The entry of a sysfs folder (a class, a bus) whose device is the platform device named
    # ("4c006000.i2c") or one of its children: its own link, or its "device" link, say so.
    try:
        names = sorted(os.listdir(folder))
    except OSError:
        return None
    for name in names:
        if not name.startswith(prefix):
            continue
        where = "/" + _target(folder + "/" + name) + "/" + _target(folder + "/" + name + "/device") + "/"
        if "/" + device + "/" in where or "/" + device + ":" in where:
            return name
    return None


def _offered(kind):
    found = board().get(kind, {})
    return ", ".join(sorted(str(k) for k in found)) or "none on this board"


def _pin_name(pin):
    # "PA5", an alias of the board's ("A0"), or the name inside a machine.Pin's repr.
    if isinstance(pin, str):
        name = pin.upper()
    else:
        text = repr(pin)
        start = text.find("'")
        name = text[start + 1 : text.find("'", start + 1)] if start >= 0 else text
    return board().get("aliases", {}).get(name, name)


# --- I2C: /dev/i2c-N, combined transfers through I2C_RDWR ---

_I2C_RDWR = 0x0707
_I2C_M_RD = 0x0001


class I2C:
    # machine.I2C on an i2c-dev bus. A device that doesn't answer its address raises
    # OSError(ENODEV), as on a microcontroller. stop=False keeps the transfer open: the next one
    # follows with a repeated start, in one I2C_RDWR.

    def __init__(self, id=None, *, scl=None, sda=None, freq=400000, timeout=None):
        buses = board().get("i2c", {})
        if id is None and buses:
            id = sorted(buses)[0]
        entry = buses.get(str(id))
        if entry is None:
            raise ValueError("I2C(%r) doesn't exist; buses: %s" % (id, _offered("i2c")))
        for pin, name in ((scl, entry.get("scl")), (sda, entry.get("sda"))):
            if pin is not None and _pin_name(pin) != name:
                raise ValueError("I2C(%s) uses SCL %s and SDA %s" % (id, entry.get("scl"), entry.get("sda")))
        bus = _find("/sys/bus/i2c/devices", entry["device"], prefix="i2c-")
        if bus is None:
            raise OSError(_ENODEV, "the kernel has no bus %s (I2C%s)" % (entry["device"], id))
        self._id = id
        self._path = "/dev/" + bus
        self._fd = _open(self._path, 2)  # O_RDWR
        self._pending = []
        self.freq = self._dt_freq(bus) or freq

    @staticmethod
    def _dt_freq(bus):
        try:
            with open("/sys/bus/i2c/devices/%s/of_node/clock-frequency" % bus, "rb") as f:
                return struct.unpack(">I", f.read(4))[0]
        except OSError:
            try:
                with open("/sys/bus/i2c/devices/%s/device/of_node/clock-frequency" % bus, "rb") as f:
                    return struct.unpack(">I", f.read(4))[0]
            except OSError:
                return None

    def init(self, *, freq=None, **ignored):
        pass  # the bus speed is the device tree's

    def __repr__(self):
        return "I2C(%s, freq=%d, %s)" % (self._id, self.freq, self._path)

    def deinit(self):
        if self._fd >= 0:
            _close(self._fd)
            self._fd = -1

    def _transfer(self, messages):
        # messages: [(address, read, buffer)], run as one transfer with repeated starts.
        messages = self._pending + messages
        self._pending = []
        table = bytearray(12 * len(messages))
        for i, (address, read, buf) in enumerate(messages):
            struct.pack_into("<HHHxxI", table, 12 * i, address, _I2C_M_RD if read else 0, len(buf),
                             _address(buf) if len(buf) else 0)
        request = struct.pack("<II", _address(table), len(messages))
        if _c("ioctl", "i", "iLp")(self._fd, _I2C_RDWR, request) < 0:
            error = _errno()
            # The bus says "no acknowledge" in its own words: ENXIO, EREMOTEIO, EIO.
            if error in (6, 121, _EIO):
                raise OSError(_ENODEV, "no device answers at 0x%02x" % messages[0][0])
            raise OSError(error, "I2C transfer")

    def scan(self):
        found = []
        probe = bytearray(1)
        for address in range(0x08, 0x78):
            try:
                self._transfer([(address, True, probe)])
                found.append(address)
            except OSError:
                pass
        return found

    def writeto(self, addr, buf, stop=True):
        data = bytearray(buf)
        if not stop:
            self._pending.append((addr, False, data))
            return len(data)
        self._transfer([(addr, False, data)])
        return len(data)

    def writevto(self, addr, vector, stop=True):
        data = bytearray()
        for part in vector:
            data += part
        return self.writeto(addr, data, stop)

    def readfrom_into(self, addr, buf, stop=True):
        target = buf if isinstance(buf, bytearray) else bytearray(len(buf))
        self._transfer([(addr, True, target)])
        if target is not buf:
            buf[:] = target

    def readfrom(self, addr, nbytes, stop=True):
        buf = bytearray(nbytes)
        self.readfrom_into(addr, buf, stop)
        return bytes(buf)

    @staticmethod
    def _memory(memaddr, addrsize):
        return bytearray(memaddr.to_bytes(addrsize // 8, "big"))

    def readfrom_mem_into(self, addr, memaddr, buf, *, addrsize=8):
        target = buf if isinstance(buf, bytearray) else bytearray(len(buf))
        self._transfer([(addr, False, self._memory(memaddr, addrsize)), (addr, True, target)])
        if target is not buf:
            buf[:] = target

    def readfrom_mem(self, addr, memaddr, nbytes, *, addrsize=8):
        buf = bytearray(nbytes)
        self.readfrom_mem_into(addr, memaddr, buf, addrsize=addrsize)
        return bytes(buf)

    def writeto_mem(self, addr, memaddr, buf, *, addrsize=8):
        self._transfer([(addr, False, self._memory(memaddr, addrsize) + bytearray(buf))])


# --- SPI: /dev/spidevB.C ---

_SPI_IOC_MESSAGE_1 = 0x40206B00
_SPI_IOC_WR_MODE = 0x40016B01
_SPI_IOC_WR_LSB_FIRST = 0x40016B02
_SPI_IOC_WR_BITS_PER_WORD = 0x40016B03
_SPI_IOC_WR_MAX_SPEED_HZ = 0x40046B04
_SPI_IOC_RD_MAX_SPEED_HZ = 0x80046B04


class SPI:
    # machine.SPI on a spidev device. The kernel drives the bus's chip select line around each
    # transfer (the board's "cs"); a device on another line has its chip select from Pin, as on a
    # microcontroller. The rate is the controller's nearest at or below the one asked.

    MSB = 0
    LSB = 1

    def __init__(self, id=None, baudrate=1000000, *, polarity=0, phase=0, bits=8, firstbit=0, sck=None,
                 mosi=None, miso=None, **ignored):
        buses = board().get("spi", {})
        if id is None and buses:
            id = sorted(buses)[0]
        entry = buses.get(str(id))
        if entry is None:
            raise ValueError("SPI(%r) doesn't exist; buses: %s" % (id, _offered("spi")))
        for pin, key in ((sck, "sck"), (mosi, "mosi"), (miso, "miso")):
            if pin is not None and _pin_name(pin) != entry.get(key):
                raise ValueError("SPI(%s) uses SCK %s, MOSI %s, MISO %s" % (id, entry["sck"], entry["mosi"], entry["miso"]))
        name = _find("/sys/class/spidev", entry["device"])
        if name is None:
            raise OSError(_ENODEV, "the kernel has no spidev on %s (SPI%s)" % (entry["device"], id))
        self._id = id
        self._path = "/dev/" + name
        self._fd = _open(self._path, 2)
        self._settings = {"baudrate": baudrate, "polarity": polarity, "phase": phase, "bits": bits,
                          "firstbit": firstbit}
        self.init()

    def init(self, baudrate=None, *, polarity=None, phase=None, bits=None, firstbit=None, **ignored):
        for key, value in (("baudrate", baudrate), ("polarity", polarity), ("phase", phase), ("bits", bits),
                           ("firstbit", firstbit)):
            if value is not None:
                self._settings[key] = value
        s = self._settings
        if s["bits"] != 8:
            raise ValueError("bits: 8")
        _ioctl(self._fd, _SPI_IOC_WR_MODE, bytearray([(s["polarity"] << 1) | s["phase"]]), "SPI mode")
        _ioctl(self._fd, _SPI_IOC_WR_LSB_FIRST, bytearray([1 if s["firstbit"] == SPI.LSB else 0]), "SPI bit order")
        _ioctl(self._fd, _SPI_IOC_WR_BITS_PER_WORD, bytearray([8]), "SPI bits")
        _ioctl(self._fd, _SPI_IOC_WR_MAX_SPEED_HZ, bytearray(struct.pack("<I", int(s["baudrate"]))), "SPI rate")

    def __repr__(self):
        s = self._settings
        return "SPI(%s, baudrate=%d, polarity=%d, phase=%d, bits=8, %s)" % (
            self._id, s["baudrate"], s["polarity"], s["phase"], self._path)

    def deinit(self):
        if self._fd >= 0:
            _close(self._fd)
            self._fd = -1

    def _transfer(self, out, into):
        count = len(out) if out is not None else len(into)
        if not count:
            return
        message = struct.pack("<QQIIHBBBBBB", _address(out) if out is not None else 0,
                              _address(into) if into is not None else 0, count, int(self._settings["baudrate"]),
                              0, 8, 0, 0, 0, 0, 0)
        _ioctl(self._fd, _SPI_IOC_MESSAGE_1, message, "SPI transfer")

    def write(self, buf):
        self._transfer(bytearray(buf), None)

    def readinto(self, buf, write=0x00):
        out = bytearray([write]) * len(buf)
        target = buf if isinstance(buf, bytearray) else bytearray(len(buf))
        self._transfer(out, target)
        if target is not buf:
            buf[:] = target

    def read(self, nbytes, write=0x00):
        buf = bytearray(nbytes)
        self.readinto(buf, write)
        return bytes(buf)

    def write_readinto(self, write_buf, read_buf):
        if len(write_buf) != len(read_buf):
            raise ValueError("buffers of the same length")
        target = read_buf if isinstance(read_buf, bytearray) else bytearray(len(read_buf))
        self._transfer(bytearray(write_buf), target)
        if target is not read_buf:
            read_buf[:] = target


# --- UART: a tty of the kernel ---

_BAUD = {50: 0o1, 75: 0o2, 110: 0o3, 134: 0o4, 150: 0o5, 200: 0o6, 300: 0o7, 600: 0o10, 1200: 0o11,
         1800: 0o12, 2400: 0o13, 4800: 0o14, 9600: 0o15, 19200: 0o16, 38400: 0o17, 57600: 0o10001,
         115200: 0o10002, 230400: 0o10003, 460800: 0o10004, 500000: 0o10005, 576000: 0o10006,
         921600: 0o10007, 1000000: 0o10010, 1152000: 0o10011, 1500000: 0o10012, 2000000: 0o10013,
         2500000: 0o10014, 3000000: 0o10015, 3500000: 0o10016, 4000000: 0o10017}
_CBAUD = 0o10017
_CSIZE = 0o60
_CS7 = 0o40
_CS8 = 0o60
_CSTOPB = 0o100
_CREAD = 0o200
_PARENB = 0o400
_PARODD = 0o1000
_CLOCAL = 0o4000
_CRTSCTS = 0o20000000000


class UART:
    # machine.UART on a serial port of the kernel, raw. The kernel keeps what arrives (4 KB) while
    # Python is busy. Baud rates are Linux's standard ones (300 to 4000000); 7 or 8 data bits;
    # parity None, 0 (even) or 1 (odd); 1 or 2 stop bits. timeout and timeout_char in ms.

    def __init__(self, id=None, baudrate=9600, bits=8, parity=None, stop=1, *, tx=None, rx=None, timeout=0,
                 timeout_char=0, **ignored):
        ports = board().get("uart", {})
        if id is None and ports:
            id = sorted(ports)[0]
        entry = ports.get(str(id))
        if entry is None:
            raise ValueError("UART(%r) doesn't exist; ports: %s" % (id, _offered("uart")))
        for pin, key in ((tx, "tx"), (rx, "rx")):
            if pin is not None and _pin_name(pin) != entry.get(key):
                raise ValueError("UART(%s) uses TX %s and RX %s" % (id, entry["tx"], entry["rx"]))
        name = _find("/sys/class/tty", entry["device"])
        if name is None:
            raise OSError(_ENODEV, "the kernel has no serial port on %s (UART%s)" % (entry["device"], id))
        self._id = id
        self._entry = entry
        self._path = "/dev/" + name
        self._fd = _open(self._path, 0o4402)  # O_RDWR | O_NOCTTY | O_NONBLOCK
        self._poll = select.poll()
        self._poll.register(self._fd, select.POLLIN)
        self._settings = {"baudrate": baudrate, "bits": bits, "parity": parity, "stop": stop, "timeout": timeout,
                          "timeout_char": timeout_char}
        self.init()

    def init(self, baudrate=None, bits=None, parity=-1, stop=None, *, timeout=None, timeout_char=None, **ignored):
        import termios

        for key, value in (("baudrate", baudrate), ("bits", bits), ("stop", stop), ("timeout", timeout),
                           ("timeout_char", timeout_char)):
            if value is not None:
                self._settings[key] = value
        if parity != -1:
            self._settings["parity"] = parity
        s = self._settings
        if s["baudrate"] not in _BAUD:
            raise ValueError("baudrate: one of Linux's, %s" % ", ".join(str(b) for b in sorted(_BAUD) if b >= 300))
        if s["bits"] not in (7, 8) or s["parity"] not in (None, 0, 1) or s["stop"] not in (1, 2):
            raise ValueError("bits 7 or 8, parity None, 0 or 1, stop 1 or 2")
        attributes = termios.tcgetattr(self._fd)
        cflag = attributes[2] & ~(_CBAUD | _CSIZE | _CSTOPB | _PARENB | _PARODD | _CRTSCTS)
        cflag |= _BAUD[s["baudrate"]] | (_CS8 if s["bits"] == 8 else _CS7) | _CREAD | _CLOCAL
        if s["stop"] == 2:
            cflag |= _CSTOPB
        if s["parity"] is not None:
            cflag |= _PARENB | (_PARODD if s["parity"] == 1 else 0)
        speed = _BAUD[s["baudrate"]]
        cc = attributes[6]
        termios.tcsetattr(self._fd, termios.TCSANOW, [0, 0, cflag, 0, speed, speed, cc])
        self.baudrate = s["baudrate"]
        frame = 1 + s["bits"] + (0 if s["parity"] is None else 1) + s["stop"]
        self._char_ms = max(s["timeout_char"], (frame * 1000 + self.baudrate - 1) // self.baudrate + 1)
        self._buffer = b""

    def __repr__(self):
        s = self._settings
        return "UART(%s, baudrate=%d, bits=%d, parity=%s, stop=%d, tx=%s, rx=%s, timeout=%d, timeout_char=%d)" % (
            self._id, self.baudrate, s["bits"], s["parity"], s["stop"], self._entry["tx"], self._entry["rx"],
            s["timeout"], s["timeout_char"])

    def deinit(self):
        if self._fd >= 0:
            self._poll.unregister(self._fd)
            _close(self._fd)
            self._fd = -1

    def _fetch(self, wait_ms):
        if not self._poll.poll(wait_ms):
            return 0
        chunk = bytearray(1024)
        count = _c("read", "i", "ipi")(self._fd, chunk, len(chunk))
        if count > 0:
            self._buffer += bytes(chunk[:count])
            return count
        return 0

    def any(self):
        while self._fetch(0):
            pass
        return len(self._buffer)

    def _wait_for(self, count, line=False):
        wait = self._settings["timeout"]
        while len(self._buffer) < count and not (line and b"\n" in self._buffer):
            if not self._fetch(wait):
                break
            wait = self._char_ms

    def read(self, nbytes=None):
        if nbytes is None:
            self._wait_for(1)
            self.any()
            nbytes = len(self._buffer)
        else:
            self._wait_for(nbytes)
        data, self._buffer = self._buffer[:nbytes], self._buffer[nbytes:]
        return data or None

    def readinto(self, buf, nbytes=None):
        data = self.read(len(buf) if nbytes is None else nbytes)
        if data is None:
            return None
        buf[: len(data)] = data
        return len(data)

    def readline(self):
        self._wait_for(1 << 30, line=True)
        end = self._buffer.find(b"\n") + 1 or len(self._buffer)
        data, self._buffer = self._buffer[:end], self._buffer[end:]
        return data or None

    def write(self, buf):
        data = bytes(buf)
        sent = 0
        out = select.poll()
        out.register(self._fd, select.POLLOUT)
        while sent < len(data):
            count = _c("write", "i", "ipi")(self._fd, data[sent:], len(data) - sent)
            if count > 0:
                sent += count
            else:
                out.poll(100)
        return sent

    def txdone(self):
        pending = bytearray(4)
        _ioctl(self._fd, 0x5411, pending, "TIOCOUTQ")
        return struct.unpack("<i", pending)[0] == 0

    def flush(self):
        while not self.txdone():
            time.sleep_ms(1)

    def sendbreak(self):
        _c("tcsendbreak", "i", "ii")(self._fd, 0)


# --- PWM: /sys/class/pwm ---


class PWM:
    # machine.PWM on a channel of the kernel's PWM framework. Periods in nanoseconds: from about
    # 1 Hz to the timer's clock; the duty is set in ns, the nearest the timer can make.

    def __init__(self, dest, *, freq=None, duty_u16=None, duty_ns=None, invert=False):
        name = _pin_name(dest)
        outputs = board().get("pwm", {})
        entry = outputs.get(name)
        if entry is None:
            raise ValueError("no PWM on %s; PWM pins: %s" % (name, ", ".join(sorted(outputs)) or "none on this board"))
        chip = _find("/sys/class/pwm", entry["device"])
        if chip is None:
            raise OSError(_ENODEV, "the kernel has no PWM on %s (%s)" % (entry["device"], name))
        self._name = name
        self._path = "/sys/class/pwm/%s/pwm%d" % (chip, entry["channel"])
        if not _exists(self._path):
            _write("/sys/class/pwm/%s/export" % chip, entry["channel"])
        self._chip = chip
        self._channel = entry["channel"]
        self._period = 0
        self._duty = 0
        self._on = False
        self.init(freq=freq, duty_u16=duty_u16, duty_ns=duty_ns, invert=invert)

    def init(self, *, freq=None, duty_u16=None, duty_ns=None, invert=None):
        if invert is not None:
            if self._on:
                _write(self._path + "/enable", 0)
                self._on = False
            _write(self._path + "/polarity", "inversed" if invert else "normal")
        if freq is not None:
            self.freq(freq)
        if duty_ns is not None:
            self.duty_ns(duty_ns)
        elif duty_u16 is not None:
            self.duty_u16(duty_u16)

    def __repr__(self):
        if not self._period:
            return "PWM(%r)" % self._name
        return "PWM(%r, freq=%d, duty_u16=%d)" % (self._name, self.freq(), self.duty_u16())

    def _enable(self):
        if self._period and not self._on:
            _write(self._path + "/enable", 1)
            self._on = True

    def deinit(self):
        if _exists(self._path):
            _write(self._path + "/enable", 0)
            _write("/sys/class/pwm/%s/unexport" % self._chip, self._channel)
        self._on = False

    def freq(self, value=None):
        if value is None:
            return 1000000000 // self._period if self._period else 0
        if not 0 < value <= 50000000:
            raise ValueError("freq 1 Hz to 50 MHz")
        ratio = self._duty / self._period if self._period else 0
        period = 1000000000 // value
        _write(self._path + "/duty_cycle", 0)  # never above the period
        _write(self._path + "/period", period)
        self._period = int(_read(self._path + "/period"))
        self._set_duty(int(self._period * ratio))
        self._enable()

    def _set_duty(self, ns):
        _write(self._path + "/duty_cycle", max(0, min(ns, self._period)))
        self._duty = int(_read(self._path + "/duty_cycle"))

    def duty_u16(self, value=None):
        if value is None:
            return self._duty * 65535 // self._period if self._period else 0
        if not 0 <= value <= 65535:
            raise ValueError("duty_u16 0 to 65535")
        if not self._period:
            raise ValueError("set freq first")
        self._set_duty(self._period * value // 65535)
        self._enable()

    def duty_ns(self, value=None):
        if value is None:
            return self._duty
        if not self._period:
            raise ValueError("set freq first")
        self._set_duty(int(value))
        self._enable()


# --- ADC: IIO ---


def _iio(device):
    return _find("/sys/bus/iio/devices", device, prefix="iio:device")


class ADC:
    # machine.ADC on an IIO channel: read_u16() scaled to 16 bits from the converter's own (12 on
    # an STM32MP13), read_uv() in microvolts from the kernel's scale. read_timed() and capture()
    # sample at a rate a hardware timer keeps (IIO buffer and trigger), as a microcontroller would:
    # evenly spaced whatever Linux does meanwhile.

    def __init__(self, source, *, atten=None):
        adc = board().get("adc")
        if not adc:
            raise ValueError("no ADC on this board")
        name = _pin_name(source)
        pins = adc.get("pins", {})
        if name not in pins:
            raise ValueError("no ADC input on %s; inputs: %s" % (name, ", ".join(sorted(pins))))
        device = _iio(adc["device"])
        if device is None:
            raise OSError(_ENODEV, "the kernel has no ADC %s" % adc["device"])
        self._name = name
        self._channel = pins[name]
        self._bits = adc.get("bits", 12)
        self._dir = "/sys/bus/iio/devices/" + device
        self._dev = "/dev/" + device
        self._trigger = adc.get("trigger")
        self._max_freq = adc.get("max_freq", 100000)
        self._scale = float(_read(self._dir + "/in_voltage_scale"))  # mV per step
        try:
            self._offset = int(_read(self._dir + "/in_voltage_offset"))
        except OSError:
            self._offset = 0
        self._capture = None
        self.freq = 0
        self.overruns = 0

    def __repr__(self):
        return "ADC(%r)" % self._name

    def _raw(self):
        return int(_read("%s/in_voltage%d_raw" % (self._dir, self._channel)))

    def _wide(self, raw):
        # From the converter's bits to 16, the full scale to 65535.
        return raw * 65535 // ((1 << self._bits) - 1)

    def read_u16(self):
        return self._wide(self._raw())

    def read_uv(self):
        return int((self._raw() + self._offset) * self._scale * 1000)

    # Sampling at a fixed rate: the trigger (a timer of the SoC) starts each conversion and the
    # kernel moves the results into a buffer by DMA; Python collects them.

    def _trigger_dir(self):
        for entry in os.listdir("/sys/bus/iio/devices"):
            if entry.startswith("trigger") and _read("/sys/bus/iio/devices/%s/name" % entry) == self._trigger:
                return "/sys/bus/iio/devices/" + entry
        raise OSError(_ENODEV, "the kernel has no trigger %s" % self._trigger)

    def _stop(self):
        try:
            _write(self._dir + "/buffer/enable", 0)
        except OSError:
            pass
        try:
            _write(self._dir + "/trigger/current_trigger", "")
        except OSError:
            pass
        if self._capture is not None:
            _close(self._capture[0])
            self._capture = None

    def capture(self, count, freq):
        """Starts count samples at freq Hz and returns the exact rate; capture_done() tells when
        they are all in, capture_read(buf) collects them. Not in MicroPython's API: for a GUI that
        must not wait."""
        if not self._trigger:
            raise OSError(95, "this board's ADC has no sampling clock")  # EOPNOTSUPP
        if not 0 < count <= 32768:
            raise ValueError("1 to 32768 samples at a time")
        if not 1 <= freq <= self._max_freq:
            raise ValueError("freq 1 to %d Hz" % self._max_freq)
        self._stop()
        trigger = self._trigger_dir()
        try:
            _write(trigger + "/master_mode", "update")
        except OSError:
            pass
        _write(trigger + "/sampling_frequency", int(freq))
        self.freq = float(_read(trigger + "/sampling_frequency"))
        scan = self._dir + "/scan_elements/"
        for entry in os.listdir(scan):
            if entry.endswith("_en"):
                _write(scan + entry, 1 if entry == "in_voltage%d_en" % self._channel else 0)
        _write(self._dir + "/buffer/length", max(count, 64))
        try:
            _write(self._dir + "/buffer/watermark", 1)
        except OSError:
            pass
        _write(self._dir + "/trigger/current_trigger", self._trigger)
        fd = _open(self._dev, 0o4000)  # O_RDONLY | O_NONBLOCK
        self._capture = [fd, bytearray(2 * count), 0]
        _write(self._dir + "/buffer/enable", 1)
        self.overruns = 0
        return self.freq

    def capture_done(self):
        if self._capture is None:
            return True
        fd, data, taken = self._capture
        while taken < len(data):
            part = bytearray(len(data) - taken)
            got = _c("read", "i", "ipi")(fd, part, len(part))
            if got <= 0:
                break
            data[taken : taken + got] = part[:got]
            taken += got
        self._capture[2] = taken
        if taken >= len(data):
            _write(self._dir + "/buffer/enable", 0)
            return True
        return False

    def capture_read(self, buf):
        if self._capture is None:
            return 0
        data = self._capture[1]
        count = min(len(buf), self._capture[2] // 2)
        wide = not isinstance(buf, (bytearray, bytes))
        shift = self._bits - 8
        for i in range(count):
            raw = data[2 * i] | (data[2 * i + 1] << 8)
            buf[i] = self._wide(raw) if wide else raw >> shift
        self._stop()
        return count

    def read_timed(self, buf, freq):
        # As on the Pyboard, with the rate instead of a timer: fills buf (array('H'): 16-bit
        # values; bytearray: their top 8 bits) with samples taken at freq Hz.
        self.capture(len(buf), freq)
        deadline = time.ticks_add(time.ticks_ms(), len(buf) * 1000 // max(1, int(self.freq)) + 1000)
        while not self.capture_done():
            if time.ticks_diff(deadline, time.ticks_ms()) < 0:
                self._stop()
                raise OSError(_ETIMEDOUT, "the capture did not finish")
            time.sleep_ms(2)
        self.capture_read(buf)


# --- Timer: a thread on Linux's monotonic clock ---


class Timer:
    # machine.Timer without a microcontroller: one thread of this process keeps every timer and
    # runs the callbacks, at absolute times so that the period does not drift. When Linux runs
    # the thread late, expiries it could not take in time come as one call, counted in missed;
    # late_us says how late the last call was. Periods from 1 ms. Any id; -1 takes a free one.

    ONE_SHOT = 0
    PERIODIC = 1
    _all = {}
    _lock = None
    _running = False

    def __init__(self, id=-1, *, mode=1, period=-1, freq=-1, callback=None, hard=False, **ignored):
        if id == -1:
            id = 0
            while id in Timer._all:
                id += 1
        self._id = id
        self._callback = None
        self._due = None
        self.missed = 0
        self.late_us = 0
        if period != -1 or freq != -1:
            self.init(mode=mode, period=period, freq=freq, callback=callback)

    def init(self, *, mode=1, period=-1, freq=-1, callback=None, tick_hz=1000, hard=False, **ignored):
        if mode not in (Timer.ONE_SHOT, Timer.PERIODIC):
            raise ValueError("mode is Timer.ONE_SHOT or Timer.PERIODIC")
        if freq != -1:
            if not 0 < freq <= 1000:
                raise ValueError("freq up to 1000 Hz")
            us = round(1000000 / freq)
        elif period != -1:
            us = round(period * 1000000 / tick_hz)
        else:
            raise ValueError("period or freq is needed")
        if us < 1000:
            raise ValueError("the period is at least 1 ms")
        if Timer._lock is None:
            Timer._lock = _thread.allocate_lock()
        with Timer._lock:
            self._callback = callback
            self._mode = mode
            self._us = us
            self._due = time.ticks_add(time.ticks_us(), us)
            Timer._all[self._id] = self
        if not Timer._running:
            Timer._running = True
            _thread.start_new_thread(Timer._run, ())

    def deinit(self):
        if Timer._lock is None:
            return
        with Timer._lock:
            if Timer._all.get(self._id) is self:
                del Timer._all[self._id]
            self._due = None

    def __repr__(self):
        if Timer._all.get(self._id) is not self:
            return "Timer(%d)" % self._id
        return "Timer(%d, mode=%s, period=%d us)" % (self._id, ("ONE_SHOT", "PERIODIC")[self._mode], self._us)

    @staticmethod
    def _run():
        while True:
            with Timer._lock:
                timers = list(Timer._all.values())
            if not timers:
                time.sleep_ms(5)
                continue
            now = time.ticks_us()
            first = min(timers, key=lambda t: time.ticks_diff(t._due, now))
            wait = time.ticks_diff(first._due, now)
            if wait > 0:
                # Sleep in steps of at most 20 ms: a timer set meanwhile is seen soon. sleep_ms
                # lets the other threads run (the GIL is released); the Unix port's sleep_us keeps
                # the GIL, so only the last fraction of a millisecond goes through it.
                step = min(wait, 20000)
                if step > 1500:
                    time.sleep_ms((step - 500) // 1000)
                else:
                    time.sleep_us(step)
                continue
            late = -wait
            with Timer._lock:
                if Timer._all.get(first._id) is not first or first._due is None:
                    continue
                periods = late // first._us
                first.missed += periods
                first.late_us = late % first._us if first._mode == Timer.PERIODIC else late
                if first._mode == Timer.ONE_SHOT:
                    del Timer._all[first._id]
                    first._due = None
                else:
                    first._due = time.ticks_add(first._due, (periods + 1) * first._us)
            if first._callback:
                try:
                    first._callback(first)
                except Exception as error:  # a callback's error must not stop every timer
                    import sys

                    sys.print_exception(error)


def offered():
    """The classes this board offers here: those with something in its description, and Timer."""
    found = ["Timer"]
    described = board()
    for name, key in (("I2C", "i2c"), ("SPI", "spi"), ("UART", "uart"), ("PWM", "pwm"), ("ADC", "adc")):
        if described.get(key):
            found.append(name)
    return found
