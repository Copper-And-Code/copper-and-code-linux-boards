# SPDX-License-Identifier: MIT
# machine.I2C, ADC (also at a fixed rate), PWM, SPI, UART and Timer served by the Cortex-M4 of an STM32MP15 board, as on a
# microcontroller: the same MicroPython code runs on both.
#
#   from machine import I2C, ADC
#   i2c = I2C(5, freq=400000)        # I2C5: Arduino D14 SDA / D15 SCL, 40-pin header pins 3 and 5
#   i2c.scan()
#   ADC("A0").read_uv()              # Arduino A0..A5, pin names ("PF14") or ADC.CORE_VREF
#   ADC("A0").read_timed(array.array("H", bytes(2000)), 10000)   # 1000 samples at 10 kHz, by the M4
#   PWM("D6", freq=1000, duty_u16=16384)   # TIM1 channel 1 on PE9 (Arduino D6)
#   SPI(4, baudrate=1000000)         # SCK D13, MISO D12, MOSI D11; chip select: Pin("PE11") (D10)
#   UART(7, 9600)                    # RX D0, TX D1
#   Timer(0, period=500, callback=lambda t: print("tick"))    # 8 timers of the M4, 1 ms and up
#
# The firmware (mcu/stm32mp157c-dk2, mpy_m4.elf) runs on the M4, loaded by Linux through
# remoteproc; the two talk over RPMsg (a raw channel, or the virtual serial port) with the protocol of
# mcu/stm32mp157c-dk2/firmware/Inc/mpy_m4_protocol.h. An error of the M4 is an OSError with the
# errno a microcontroller gives: ENODEV when no device answers an address, ETIMEDOUT, EIO.

import os
import select
import struct
import sys
import termios
import time

import _thread

REMOTEPROC = "/sys/class/remoteproc/remoteproc0"
FIRMWARE = "mpy_m4.elf"
PROTOCOL = 2

_INFO = 0x01
_LED = 0x02
_I2C_INIT = 0x10
_I2C_SCAN = 0x11
_I2C_WRITE = 0x12
_I2C_READ = 0x13
_I2C_MEM_READ = 0x14
_I2C_MEM_WRITE = 0x15
_ADC_READ = 0x20
_ADC_CAPTURE = 0x21
_ADC_CAPTURE_STATUS = 0x22
_ADC_CAPTURE_FETCH = 0x23
_CAPTURE_MAX = 32768
_PWM_INIT = 0x30
_PWM_COMPARE = 0x31
_PWM_DEINIT = 0x32
_PIN_MEASURE = 0x40
_PORT_CAPTURE = 0x41
_SPI_INIT = 0x50
_SPI_TRANSFER = 0x51
_SPI_DEINIT = 0x52
_UART_INIT = 0x60
_UART_WRITE = 0x61
_UART_READ = 0x62
_UART_STATUS = 0x63
_UART_BREAK = 0x64
_UART_DEINIT = 0x65
_TIMER_INIT = 0x70
_TIMER_DEINIT = 0x71
_EVENT_ACK = 0x72
_EVENT_TIMER = 1

_MAX_DATA = 400
_VTIME = 5  # indices in termios' cc on Linux
_VMIN = 6
_EIO = 5
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


def _ports():
    # The firmware's two channels: the raw one (rpmsg-raw: /dev/rpmsgN through rpmsg_char) and
    # the virtual serial port (rpmsg-tty: /dev/ttyRPMSGN). Their numbers change when the M4
    # restarts.
    names = os.listdir("/dev")
    raw = sorted(n for n in names if n.startswith("rpmsg") and n[5:].isdigit())
    tty = sorted(n for n in names if n.startswith("ttyRPMSG"))
    return ("/dev/" + raw[0]) if raw else None, ("/dev/" + tty[0]) if tty else None


_libc = None


def _c(name, result, arguments):
    global _libc
    if _libc is None:
        import ffi

        _libc = ffi.open("libc.so.6")
    return _libc.func(result, name, arguments)


class _Raw:
    # A raw RPMsg channel (rpmsg_char), used like the serial port's file: each read() returns one
    # whole message, which the firmware makes one frame. It goes around Linux's tty layer: about
    # 30 us less a request idle, 60 us less with Linux busy. Through the C library (ffi), as
    # MicroPython's read(n) would wait for n bytes.

    def __init__(self, path):
        self._read = _c("read", "i", "ipi")
        self._write = _c("write", "i", "ipi")
        self._close = _c("close", "i", "i")
        self._fd = _c("open", "i", "si")(path, 0o2000002)  # O_RDWR | O_CLOEXEC
        if self._fd < 0:
            raise OSError(_EIO, "cannot open " + path)
        self._buffer = bytearray(512)

    def fileno(self):
        return self._fd

    def read(self, count):
        size = self._read(self._fd, self._buffer, len(self._buffer))
        if size < 0:
            raise OSError(_EIO, "the Cortex-M4's channel failed")
        return bytes(self._buffer[:size])

    def write(self, data):
        if self._write(self._fd, data, len(data)) != len(data):
            raise OSError(_EIO, "the Cortex-M4's channel failed")

    def close(self):
        self._close(self._fd)


class _Link:
    # Requests and answers, one at a time, matched by sequence number (1 to 255). Frames with
    # sequence 0 are events the M4 sends unasked (a timer's expiry); they can arrive between a
    # request and its answer. Whoever reads them (the thread making a request, or the reader
    # thread that watches the port when nobody asks) queues them; handlers run after the port's
    # lock is released, so a handler may make requests itself.

    def __init__(self):
        self._file = None
        self.transport = None  # "rpmsg_char /dev/rpmsgN" or "tty /dev/ttyRPMSGN", once open
        self._raw = False
        self._sequence = 0
        self._received = b""  # bytes read past the last frame
        self._io = _thread.allocate_lock()
        self._events = []
        self.handlers = {}  # event kind -> function(data)
        self._reader = False

    def _open(self):
        if not available():
            raise OSError(19, "no Cortex-M4 on this board")
        _start()
        raw = tty = None
        for _ in range(40):  # the channels appear once the firmware is up
            raw, tty = _ports()
            if raw or tty:
                break
            time.sleep_ms(50)
        if tty and not raw:
            time.sleep_ms(100)  # the raw channel is announced right after the serial port
            raw, tty = _ports()
        self._file = None
        self._raw = False
        if raw:
            try:
                self._file = _Raw(raw)
                self._raw = True
                self.transport = "rpmsg_char " + raw
            except (ImportError, OSError):
                self._file = None  # no ffi, or the channel is busy: the serial port then
        if self._file is None:
            if not tty:
                raise OSError(19, "the Cortex-M4 firmware does not answer (no /dev/rpmsg*, /dev/ttyRPMSG*)")
            self._file = open(tty, "r+b", buffering=0)
            self.transport = "tty " + tty
            termios.setraw(self._file.fileno())
            # VMIN 0: a read returns what has arrived instead of waiting for all it asked for
            # (MicroPython's read(n) waits for n bytes until the driver returns 0). poll() waits.
            attributes = termios.tcgetattr(self._file.fileno())
            attributes[6][_VMIN] = 0
            attributes[6][_VTIME] = 0
            termios.tcsetattr(self._file.fileno(), termios.TCSANOW, attributes)
        self._poll = select.poll()
        self._poll.register(self._file.fileno(), select.POLLIN)
        self._watch = select.poll()  # the reader thread's own
        self._watch.register(self._file.fileno(), select.POLLIN)
        while self._poll.poll(0):  # leftovers of an earlier session
            self._file.read(256)
        self._received = b""
        info = self._exchange(_INFO, b"", 1000)
        if info[0] != PROTOCOL:
            self.close()
            raise OSError(_EOPNOTSUPP, "Cortex-M4 firmware speaks protocol %d, not %d" % (info[0], PROTOCOL))
        self.firmware = info[1:].decode()
        # Timers left running by an earlier program would send events nobody waits for.
        self._exchange(_TIMER_DEINIT, b"\xff", 1000)

    def close(self):
        if self._file:
            self._file.close()
            self._file = None
        self._received = b""

    def _frame(self, timeout_ms):
        # The next frame. In raw mode with VMIN 0 a read returns at once with whatever has
        # arrived, so a whole answer usually takes one poll and one read. Written flat: on this
        # CPU each Python call costs microseconds, next to a round trip of about 250.
        received = self._received
        poll = self._poll.poll
        read = self._file.read
        if self._raw and not received:
            # A raw channel's message is one whole frame: no buffer to go through.
            if not poll(timeout_ms):
                raise OSError(_ETIMEDOUT, "the Cortex-M4 did not answer")
            message = read(0)
            if len(message) >= 2 and len(message) == 2 + (message[0] | message[1] << 8):
                return message[2:]
            received = message
        while True:
            if len(received) >= 2:
                length = received[0] | received[1] << 8
                if not 2 <= length <= 600:
                    raise OSError(_EIO, "out of step with the Cortex-M4")
                if len(received) >= 2 + length:
                    self._received = received[2 + length :]
                    return received[2 : 2 + length]
            if not poll(timeout_ms):
                self._received = received
                raise OSError(_ETIMEDOUT, "the Cortex-M4 did not answer")
            received += read(1024)

    def _exchange(self, operation, arguments, timeout_ms):
        sequence = self._sequence = self._sequence % 255 + 1
        try:
            self._file.write(struct.pack("<HBB", 2 + len(arguments), sequence, operation) + arguments)
            answer = self._frame(timeout_ms)
            while answer[0] != sequence:
                if answer[0] == 0:
                    self._events.append(answer[1:])
                # else: the answer to a request given up on
                answer = self._frame(timeout_ms)
        except OSError:
            self.close()  # out of step: open again at the next request
            raise
        if answer[1]:
            raise OSError(answer[1])
        return answer[2:]

    def request(self, operation, arguments=b"", timeout_ms=1000):
        with self._io:
            if self._file is None:
                self._open()
            answer = self._exchange(operation, arguments, timeout_ms)
        self._dispatch()
        return answer

    def _dispatch(self):
        # Handlers run here, then the M4 hears that they ran: only then does it send each timer's
        # next event (meanwhile it counts the expiries into it).
        handled = False
        while self._events:
            frame = self._events.pop(0)
            handled = True
            handler = self.handlers.get(frame[0])
            if handler:
                try:
                    handler(frame[1:])
                except Exception as error:  # as a scheduled callback: report and go on
                    sys.print_exception(error)
        if handled:
            try:
                with self._io:
                    if self._file:
                        self._exchange(_EVENT_ACK, b"", 1000)
            except OSError:
                pass
            if self._events:  # came with the acknowledgement's answer
                self._dispatch()

    def listen(self):
        # Start the reader thread, once: it reads events while nobody makes requests.
        if not self._reader:
            self._reader = True
            _thread.start_new_thread(self._listen, ())

    def _listen(self):
        while True:
            watch = self._watch if self._file else None
            if watch is None or not watch.poll(20):
                if watch is None:
                    time.sleep_ms(20)
                continue
            with self._io:
                try:
                    while self._file and (self._received or self._poll.poll(0)):
                        frame = self._frame(200)
                        if frame[0] == 0:
                            self._events.append(frame[1:])
                except OSError:
                    self.close()
            self._dispatch()


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
_DIGITAL = {"D0": "PE7", "D1": "PE8", "D6": "PE9", "D10": "PE11", "D11": "PE14", "D12": "PE13", "D13": "PE12", "D14": "PA12", "D15": "PA11"}


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


def logic_capture(count, freq, port="E", spi=None, uart=None):
    # A logic analyser (not in MicroPython's API): the M4 samples all 16 pins of a GPIO port
    # count times (up to 32768) at freq Hz (up to 4 MHz), timed by its cycle counter; returns
    # (array of 16-bit port values, exact rate, samples an interrupt delayed past their time). Bit n is pin Pn: on port E, D0 is bit 7 (PE7),
    # D1 bit 8, D6 bit 9, D13 bit 12, D12 bit 13, D11 bit 14, D10 bit 11. With spi or uart
    # (bytes), an SPI4 transfer or a UART7 message (SPI(4) or UART(7) set up first) starts with
    # the capture, so the board can watch its own buses from their first bit.
    # The M4 does nothing else meanwhile (at most a second); a UART's sending and a PWM go on.
    import array

    if not 0 < count <= _CAPTURE_MAX:
        raise ValueError("1 to %d samples" % _CAPTURE_MAX)
    if not 1 <= freq <= 4000000 or count * 1000 // freq > 1000:
        raise ValueError("freq up to 4 MHz, a second at most")
    letter = port.upper()
    if not "A" <= letter <= "K":
        raise ValueError("port A to K")
    stimulus, data = (1, spi) if spi else (2, uart) if uart else (0, b"")
    arguments = struct.pack("<BIIB", ord(letter) - ord("A"), int(freq), count, stimulus) + bytes(data)
    answer = link.request(_PORT_CAPTURE, arguments, 2000 + count * 1000 // freq)
    core, period, late = struct.unpack("<III", answer)
    samples = array.array("H", bytes(2 * count))
    import uctypes

    target = uctypes.bytearray_at(uctypes.addressof(samples), 2 * count)
    for first in range(0, count, 200):
        part = link.request(_ADC_CAPTURE_FETCH, struct.pack("<IH", first, min(200, count - first)))
        target[2 * first : 2 * first + len(part)] = part
    return samples, core / period, late


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

    def read_timed(self, buf, freq):
        # As on the Pyboard, with the rate instead of a timer: fills buf with samples taken at freq
        # Hz (1 to 100000), up to 32768 at a time. The M4's TIM2 triggers each conversion in
        # hardware, so the samples are evenly spaced whatever Linux does; Python waits meanwhile
        # (sleeping: the GUI goes on) and collects them. array('H'): 16-bit values; bytearray:
        # their top 8 bits. Afterwards: self.freq, the exact rate; self.overruns (0 normally).
        self.capture(len(buf), freq)
        time.sleep_ms(self._count * 1000 * self._period // self._clock)
        deadline = time.ticks_add(time.ticks_ms(), 1000)
        while not self.capture_done():
            if time.ticks_diff(deadline, time.ticks_ms()) < 0:
                raise OSError(_ETIMEDOUT, "the capture did not finish")
            time.sleep_ms(2)
        self.capture_read(buf)

    # The same in steps, for a GUI that must not wait (not in MicroPython's API): capture()
    # starts and returns, capture_done() tells when the M4 has all the samples, capture_read()
    # collects them. One capture at a time on the board.

    def capture(self, count, freq):
        if not 0 < count <= _CAPTURE_MAX:
            raise ValueError("1 to %d samples at a time" % _CAPTURE_MAX)
        if not 1 <= freq <= 100000:
            raise ValueError("freq 1 to 100000 Hz")
        answer = link.request(_ADC_CAPTURE, self._args + struct.pack("<II", count, int(freq)))
        self._clock, self._period, _ = struct.unpack("<III", answer)
        self._count = count
        self.freq = self._clock / self._period
        return self.freq

    def capture_done(self):
        state, taken, cycles, overruns = struct.unpack("<BIII", link.request(_ADC_CAPTURE_STATUS))
        self.overruns = overruns
        return state == 2 and taken == self._count

    def capture_read(self, buf):
        count = min(len(buf), self._count)
        wide = not isinstance(buf, (bytearray, bytes))
        if wide:
            import uctypes

            target = uctypes.bytearray_at(uctypes.addressof(buf), 2 * count)
        for first in range(0, count, 200):
            part = link.request(_ADC_CAPTURE_FETCH, struct.pack("<IH", first, min(200, count - first)))
            if wide:
                target[2 * first : 2 * first + len(part)] = part
            else:
                for i in range(len(part) // 2):
                    buf[first + i] = part[2 * i + 1]
        return count


class SPI:
    # machine.SPI on the M4's SPI4: SCK PE12 (D13), MISO PE13 (D12), MOSI PE14 (D11), 8-bit frames.
    # The rate is the fastest not above the one asked: 64 MHz over 2, 4, ... 256 (32 MHz to
    # 250 kHz). The chip select is the program's: PE11 (D10) is free as a Linux GPIO line,
    # Pin("PE11", Pin.OUT, value=1).

    MSB = 0
    LSB = 1
    _CHUNK = _MAX_DATA

    def __init__(self, id=4, baudrate=1000000, *, polarity=0, phase=0, bits=8, firstbit=0, sck=None,
                 mosi=None, miso=None, _miso_pull=0):
        if id != 4:
            raise ValueError("SPI(%r) doesn't exist: the Cortex-M4 has SPI4" % (id,))
        for pin, name in ((sck, "PE12"), (mosi, "PE14"), (miso, "PE13")):
            if pin is not None and _pin_name(pin) != name:
                raise ValueError("SPI4 uses SCK PE12, MISO PE13, MOSI PE14")
        self._settings = {"baudrate": baudrate, "polarity": polarity, "phase": phase, "bits": bits,
                          "firstbit": firstbit}
        self._miso_pull = _miso_pull  # for tests without a device: 1 up, 2 down
        self.init()

    def init(self, baudrate=None, *, polarity=None, phase=None, bits=None, firstbit=None, **ignored):
        for key, value in (("baudrate", baudrate), ("polarity", polarity), ("phase", phase), ("bits", bits),
                           ("firstbit", firstbit)):
            if value is not None:
                self._settings[key] = value
        settings = self._settings
        if settings["bits"] != 8:
            raise ValueError("bits must be 8")
        if settings["polarity"] not in (0, 1) or settings["phase"] not in (0, 1) or settings["firstbit"] not in (0, 1):
            raise ValueError("polarity, phase and firstbit are 0 or 1")
        if settings["baudrate"] < 1:
            raise ValueError("baudrate must be positive")
        mode = settings["polarity"] | settings["phase"] << 1 | settings["firstbit"] << 2
        answer = link.request(_SPI_INIT, struct.pack("<IBB", settings["baudrate"], mode, self._miso_pull))
        self.baudrate = struct.unpack("<I", answer)[0]
        self._on = True

    def __repr__(self):
        s = self._settings
        return "SPI(4, baudrate=%d, polarity=%d, phase=%d, bits=8, firstbit=%s)" % (
            self.baudrate, s["polarity"], s["phase"], "SPI.LSB" if s["firstbit"] else "SPI.MSB")

    def deinit(self):
        link.request(_SPI_DEINIT)
        self._on = False

    def _transfer(self, data, flags):
        if not self._on:
            self.init()
        answer = b""
        for start in range(0, len(data), self._CHUNK):
            part = data[start : start + self._CHUNK]
            answer += link.request(_SPI_TRANSFER, bytes([flags]) + part, 1000 + len(part))
        return answer

    def write(self, buf):
        self._transfer(bytes(buf), 0)

    def read(self, nbytes, write=0x00):
        return self._transfer(bytes([write]) * nbytes, 1)

    def readinto(self, buf, write=0x00):
        buf[:] = self.read(len(buf), write)

    def write_readinto(self, write_buf, read_buf):
        if len(write_buf) != len(read_buf):
            raise ValueError("buffers must be the same length")
        read_buf[:] = self._transfer(bytes(write_buf), 1)

    def _watch(self, buf):
        # For tests: the bytes the M4 saw on the MOSI pin and the SCK edges, during one transfer of
        # at most 400 bytes; reliable up to 1 MHz, the sampling's limit.
        data = bytes(buf)
        answer = link.request(_SPI_TRANSFER, bytes([2]) + data, 1000 + len(data))
        return answer[: len(data)], struct.unpack("<I", answer[len(data) :])[0]


class UART:
    # machine.UART on the M4's UART7: RX PE7 (Arduino D0), TX PE8 (D1). The M4 keeps received
    # bytes in a 4 KB buffer and sends from a 2 KB one, both under interrupt, so nothing is lost
    # between two calls. 300 baud to 4 Mbaud; 7, 8 or 9 data bits (with parity: 7 or 8);
    # parity None, 0 (even) or 1 (odd); 1 or 2 stop bits. timeout and timeout_char in ms, as on
    # MicroPython: how long read() waits for the first byte and for each next one.

    def __init__(self, id=7, baudrate=9600, bits=8, parity=None, stop=1, *, tx=None, rx=None, timeout=0,
                 timeout_char=0, _single_wire=False, **ignored):
        if id != 7:
            raise ValueError("UART(%r) doesn't exist: the Cortex-M4 has UART7" % (id,))
        for pin, name in ((tx, "PE8"), (rx, "PE7")):
            if pin is not None and _pin_name(pin) != name:
                raise ValueError("UART7 uses RX PE7 (D0) and TX PE8 (D1)")
        self._settings = {"baudrate": baudrate, "bits": bits, "parity": parity, "stop": stop,
                          "timeout": timeout, "timeout_char": timeout_char}
        self._single_wire = _single_wire  # for tests without wires: TX also receives what it sends
        self._buffer = b""
        self.init()

    def init(self, baudrate=None, bits=None, parity=-1, stop=None, *, timeout=None, timeout_char=None, **ignored):
        for key, value in (("baudrate", baudrate), ("bits", bits), ("stop", stop), ("timeout", timeout),
                           ("timeout_char", timeout_char)):
            if value is not None:
                self._settings[key] = value
        if parity != -1:
            self._settings["parity"] = parity
        s = self._settings
        if s["parity"] not in (None, 0, 1):
            raise ValueError("parity is None, 0 (even) or 1 (odd)")
        if s["bits"] not in (7, 8, 9) or (s["bits"] == 9 and s["parity"] is not None) or s["stop"] not in (1, 2):
            raise ValueError("bits 7, 8 or 9 (9 without parity), stop 1 or 2")
        if not 300 <= s["baudrate"] <= 4000000:
            raise ValueError("baudrate 300 to 4000000")
        code = 0 if s["parity"] is None else (2 if s["parity"] == 0 else 1)
        answer = link.request(_UART_INIT, struct.pack("<IBBBB", s["baudrate"], s["bits"], code, s["stop"],
                                                       1 if self._single_wire else 0))
        self.baudrate = struct.unpack("<I", answer)[0]
        # As on MicroPython, timeout_char is at least a character's time (start, data, parity, stop),
        # plus the round trip to the M4.
        frame = 1 + s["bits"] + (0 if s["parity"] is None else 1) + s["stop"]
        self._char_ms = max(s["timeout_char"], (frame * 1000 + self.baudrate - 1) // self.baudrate + 1)
        self._buffer = b""
        self._on = True

    def __repr__(self):
        s = self._settings
        return "UART(7, baudrate=%d, bits=%d, parity=%s, stop=%d, tx=PE8, rx=PE7, timeout=%d, timeout_char=%d)" % (
            self.baudrate, s["bits"], s["parity"], s["stop"], s["timeout"], s["timeout_char"])

    def deinit(self):
        link.request(_UART_DEINIT)
        self._on = False

    def _status(self):
        return struct.unpack("<HHBHHH", link.request(_UART_STATUS))

    def errors(self):
        # Not in machine.UART: (overruns, framing errors, parity errors) since init().
        return self._status()[3:]

    def _fetch(self):
        data = link.request(_UART_READ, struct.pack("<H", _MAX_DATA))
        self._buffer += data
        return len(data)

    def any(self):
        return len(self._buffer) + self._status()[0]

    def _wait_for(self, count):
        # Fetch until count bytes are here, or the timeouts run out.
        limit = self._settings["timeout"]
        start = time.ticks_ms()
        while len(self._buffer) < count:
            if self._fetch():
                limit = self._char_ms
                start = time.ticks_ms()
            elif time.ticks_diff(time.ticks_ms(), start) >= limit:
                break
            else:
                time.sleep_ms(1)

    def read(self, nbytes=None):
        if nbytes is None:
            self._wait_for(1)
            while self._fetch():
                pass
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
        limit = self._settings["timeout"]
        start = time.ticks_ms()
        while b"\n" not in self._buffer:
            if self._fetch():
                limit = self._char_ms
                start = time.ticks_ms()
            elif time.ticks_diff(time.ticks_ms(), start) >= limit:
                break
            else:
                time.sleep_ms(1)
        end = self._buffer.find(b"\n") + 1 or len(self._buffer)
        data, self._buffer = self._buffer[:end], self._buffer[end:]
        return data or None

    def write(self, buf):
        data = bytes(buf)
        sent = 0
        while sent < len(data):
            taken = struct.unpack("<H", link.request(_UART_WRITE, data[sent : sent + _MAX_DATA]))[0]
            sent += taken
            if not taken:
                time.sleep_ms(1)  # the M4's 2 KB are full: wait for the line
        return sent

    def txdone(self):
        return bool(self._status()[2])

    def flush(self):
        while not self.txdone():
            time.sleep_ms(1)

    def sendbreak(self):
        link.request(_UART_BREAK)


class Timer:
    # machine.Timer on the M4: 8 timers (0 to 7; -1 takes a free one), counted by the M4's clock.
    # At each expiry the M4 sends an event and the callback runs on Linux, in the thread that
    # reads the M4 (or in one making a request): Linux decides when, typically within a
    # millisecond; the M4 decides how many. Expiries Linux could not take in time come as one
    # call, counted in missed. Periods from 1 ms (freq up to 1000 Hz).

    ONE_SHOT = 0
    PERIODIC = 1
    _all = {}

    def __init__(self, id=-1, *, mode=1, period=-1, freq=-1, callback=None, hard=False, **ignored):
        if id == -1:
            free = [i for i in range(8) if i not in Timer._all]
            id = free[0] if free else None
            if id is None:
                raise OSError(16, "all 8 timers of the Cortex-M4 are in use")  # EBUSY
        elif not 0 <= id < 8:
            raise ValueError("Timer(%r): the Cortex-M4 has timers 0 to 7 (or -1)" % (id,))
        self._id = id
        self._callback = None
        self.missed = 0  # not in machine.Timer: expiries merged into a later call
        self.late_us = 0  # not in machine.Timer: how late the M4 sent the last event
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
        self._callback = callback
        self._mode = mode
        self._us = us
        Timer._all[self._id] = self
        link.handlers[_EVENT_TIMER] = Timer._event
        link.listen()
        link.request(_TIMER_INIT, struct.pack("<BBI", self._id, mode, us))

    def deinit(self):
        link.request(_TIMER_DEINIT, bytes([self._id]))
        if Timer._all.get(self._id) is self:
            del Timer._all[self._id]

    def __repr__(self):
        if Timer._all.get(self._id) is not self:
            return "Timer(%d)" % self._id
        return "Timer(%d, mode=%s, period=%d us)" % (self._id, ("ONE_SHOT", "PERIODIC")[self._mode], self._us)

    @staticmethod
    def _event(data):
        number, count, late = struct.unpack("<BII", data)
        timer = Timer._all.get(number)
        if timer is None:
            return
        timer.missed += count - 1
        timer.late_us = late
        if timer._mode == Timer.ONE_SHOT:
            del Timer._all[number]
        if timer._callback:
            timer._callback(timer)

