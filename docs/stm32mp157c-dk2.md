# STM32MP157C-DK2

Second board of this project, and the first with a microcontroller beside Linux: its Cortex-M4
serves `machine.I2C`, `ADC`, `PWM`, `SPI`, `UART` and `Timer` to MicroPython on the Cortex-A7.

**Status:** OpenSTLinux flashed; our build installed with `scripts/deploy.sh` (the same MicroPython
binary as the STM32MP135F-DK); shell, app menu and REPL working (serial and TCP); the 480×800 panel
turned to 800×480 in software (about 50 full frames per second); Wi-Fi, weather and the web radio
with sound through the board's codec; `machine.Pin` on Linux GPIO lines, `machine.I2C`,
`ADC`, `PWM`, `SPI`, `UART` and `Timer` on the Cortex-M4. Bring-up log: `docs/log/stm32mp157c-dk2-bringup.txt`.
Product page: <https://www.st.com/en/evaluation-tools/stm32mp157c-dk2.html>. Flashing and boot:
[Populate the target and boot the image](https://wiki.st.com/stm32mpu/wiki/Getting_started/STM32MP1_boards/STM32MP157x-DK2/Let%27s_start/Populate_the_target_and_boot_the_image)
on the ST wiki.

## The board

| Topic | What the build does |
|---|---|
| Display | DSI panel 480×800 portrait, turned to 800×480 landscape by LVGL (`MPY_DISPLAY_ROTATION=90`, set by `install.sh`) |
| Touch | FT6236 at I2C `0x38`, its range matches the panel; LVGL turns the points with the screen |
| USB | `dwc2` OTG port (USB-C CN7): the composite serial and network gadget, with TX FIFOs set by a device tree overlay |
| Audio | CS42L51 codec on the headphone jack, through `alsasink device=plughw:0,0` (`MPY_AUDIO_SINK`): ALSA's default device goes to PulseAudio, which runs only with the graphical session |
| Device tree | ST's `stm32mp157c-dk2-m4-examples.dtb` with our USB overlay (`scripts/build_mp157_usb.sh`), installed as `/boot/stm32mp157c-dk2.dtb`; the original is kept as `.st-original` |

## `machine` on the Cortex-M4

```python
import array
from machine import I2C, ADC, PWM, SPI, UART, Timer, Pin

i2c = I2C(5, freq=400000)        # Arduino D14 SDA / D15 SCL, 40-pin header pins 3 and 5
print(i2c.scan())
i2c.readfrom_mem(0x76, 0xD0, 1)  # a register, as on any MicroPython board

print(ADC("A0").read_uv())       # Arduino A0..A5, from 0 to 2.5 V
print(ADC(ADC.CORE_VREF).read_uv())
samples = array.array("H", bytes(2 * 1000))
ADC("A0").read_timed(samples, 10000)           # 1000 samples at exactly 10 kHz, by the M4

servo = PWM("D6", freq=50, duty_ns=1500000)   # TIM1 channel 1 on PE9, Arduino D6

spi = SPI(4, baudrate=8000000)                 # SCK D13, MISO D12, MOSI D11
cs = Pin("PE11", Pin.OUT, value=1)             # D10: a Linux GPIO line as chip select
cs(0); spi.read(3, 0x9F); cs(1)

gps = UART(7, 9600, timeout=1000)             # RX D0, TX D1
print(gps.readline())

Timer(0, period=500, callback=lambda t: print("tick"))

Pin("PA14", Pin.IN, Pin.PULL_UP).value()   # Pin stays on Linux GPIO lines, as on the STM32MP135F-DK
                                           # (PA14 is the USER1 button: as an input only, see below)
```

How it fits together:

- **Device tree.** The `m4-examples` device tree gives the Cortex-M4 the Arduino connector's ADC,
  DAC, I2C5, SPI4, UART7, TIM1 and TIM2, and the orange LED; Linux keeps the display, touch, Wi-Fi,
  audio and USB. Linux only reserves those pins: the firmware sets them itself. It also gives the
  M4 the USER1 button's interrupt (PA14, shared with the green LED): Linux may read PA14 but not
  drive it (`EIO`).
- **Firmware** (`mcu/stm32mp157c-dk2/`, built by `scripts/build_m4.sh` with `arm-none-eabi-gcc`,
  without STM32CubeIDE): STM32Cube HAL and OpenAMP from STM32CubeMP1 1.7.0, our `main.c` and the
  protocol. `install.sh` puts it in `/lib/firmware/mpy_m4.elf`.
- **Start.** The first use of `machine.I2C`, `ADC`, `PWM`, `SPI`, `UART` or `Timer` loads the firmware through remoteproc
  if the M4 is off (about 0.6 s). A new firmware installed by `deploy.sh` takes over at once.
- **Events.** The M4 can also speak unasked: a frame with sequence number 0 is an event (a timer's
  expiry). A thread of `mpy_m4` reads the port while nobody makes requests; events read during a
  request are kept for later. Handlers run outside the port's lock, so a callback may use the M4
  itself. Then Python acknowledges, and only then does the M4 send that timer's next event.
- **Link.** Two RPMsg channels, the same frames on both: a raw one (`rpmsg-raw`, which Linux's
  `rpmsg_char` turns into `/dev/rpmsgN` by itself) and a virtual serial port (`rpmsg-tty`,
  `/dev/ttyRPMSGN`). `mpy_m4` uses the raw one, through the C library (`ffi`), and falls back on
  the serial port; their numbers change when the M4 restarts, so it looks them up. Requests and
  answers carry a length, a sequence number and an errno: the protocol is in
  `mcu/stm32mp157c-dk2/firmware/Inc/mpy_m4_protocol.h`. A request takes about 0.3 ms there and
  back (see Latency below).
- **Python.** `mpy_m4` (frozen) has the classes; the frozen `machine` module takes the place of
  MicroPython's built-in one (an "extensible" module in v1.29), imports all of it from `umachine`
  and adds `I2C`, `ADC`, `PWM`, `SPI`, `UART` and `Timer` on a board with a Cortex-M4. On the STM32MP135F-DK nothing changes.

### Latency

What a request to the M4 costs, there and back (2000 to 3000 requests each, as installed):

| | idle: median / 99% / 99.9% | two busy Linux processes: median / 99% / 99.9% |
|---|---|---|
| `ADC.read_u16()`, raw channel (`rpmsg_char`, the default) | 290 / 371 / 494 to 772 us | 292 / 386 to 418 us / 5.8 to 6.9 ms |
| `ADC.read_u16()`, serial port | 316 / 411 to 467 us / 2.3 ms | 316 / 503 to 1103 us / 9.9 ms |
| a bare request on the raw channel, no `mpy_m4` | 162 / 215 to 240 / 270 to 600 us | 162 / 220 to 260 us / 7 to 9 ms |
| a Linux GPIO line, `Pin.value()`, for comparison | 8 us | 8 us |

About 3000 requests a second: plenty to command and read, 40 times a GPIO line, so the fast loops
stay on the M4 (`read_timed`, `mpy_m4.measure`, a whole I2C scan in one request). The M4's own work
is small (the slowest ADC conversion, 810.5 cycles, about 70 us); of the rest, about 160 us are
Linux (system calls, the M4's interrupt, waking Python up) and about 70 us `mpy_m4`'s Python.

What brought these numbers down, measured one step at a time:

- `mpy_m4` reads a whole answer in one `poll()` and one `read()`: about 10%.
- `mpy-repl.service` runs MicroPython at real-time priority (`SCHED_FIFO` 10; the programs it starts
  through `linuxproc` at normal priority): under load the slowest 1% went from 8 ms to under 0.6 ms.
- The raw channel goes around Linux's tty layer and its worker thread: 25 us less a request through
  `mpy_m4`, and a slowest 0.1% three to four times shorter when idle.

Left: under load, about one request in a thousand takes up to 10 ms. That is one tick of this
kernel (`CONFIG_HZ=100`): it stays even with the load at the lowest priority, so it is not the load
taking the CPU but a stage of the kernel waiting for its tick. Only another kernel (HZ 1000,
PREEMPT_RT) would remove it; what must happen at an exact time runs on the M4 anyway.

### I2C

Bus 5 only (`I2C(5)`, the default): SDA PA12, SCL PA11, on the Arduino connector (D14, D15) and on
the 40-pin header (pins 3 and 5, GPIO2/GPIO3). 10 kHz to 1 MHz; the default is 400 kHz. The weak
internal pull-ups keep an empty bus high; devices bring their own.

| Method | Behaviour |
|---|---|
| `scan()` | the addresses 0x08 to 0x77 that answer (3 ms for an empty bus at 400 kHz) |
| `writeto`, `writevto`, `readfrom`, `readfrom_into` | as on MicroPython; `OSError(ENODEV)` when no device answers the address |
| `readfrom_mem`, `readfrom_mem_into`, `writeto_mem` | `addrsize` 8 or 16 |
| `writeto(addr, reg, False)` then `readfrom(addr, n)` | the register read with a repeated start, for a register address of 1 or 2 bytes |

Up to 400 bytes a transfer. Not there: the primitive operations (`start`, `stop`, `readinto`,
`write`), 10-bit addresses.

### ADC

16 bits against the 2.5 V reference buffer (VREFBUF), which Linux turns on for the M4: inputs from
0 to 2.5 V, never more than 3.3 V. `read_u16()` and `read_uv()`.

| Name | Pin | Converter |
|---|---|---|
| `A0` | ANA0 | ADC1 input 0 |
| `A1` | ANA1 | ADC1 input 1 |
| `A2` | PF14 | ADC2 input 6 |
| `A3` | PF13 | ADC2 input 2 |
| `A4` | PC3 | ADC1 input 13 |
| `A5` | PF12 | ADC1 input 6 |
| `ADC.CORE_VREF` | internal reference, about 1.21 V | ADC2 |
| `ADC.CORE_TEMP` | temperature sensor (a voltage; Linux gives the chip's temperature in `/sys/class/thermal`) | ADC2 |

The names follow the board's schematic (the net names of user manual UM2534); the manual's table
of Arduino pins has some rows shifted. Pin names (`ADC("PF14")`) work too.

**Sampling at a fixed rate.** `adc.read_timed(buf, freq)`, as on the Pyboard but with the rate
instead of a timer: up to 32768 samples at 1 Hz to 100 kHz. The M4's TIM2 (32 bits at 208.9 MHz)
triggers each conversion in hardware and an interrupt stores it in the M4's memory, so the samples
are evenly spaced whatever Linux does; Python sleeps meanwhile (the GUI and the timers go on) and
then collects them, 200 at a time. `array('H')` gets 16-bit values, a `bytearray` their top 8
bits; afterwards `adc.freq` is the exact rate (TIM2's clock over its period) and `adc.overruns`
the conversions lost (none so far). The sampling time is the longest that fits the rate (810.5
ADC cycles up to 10 kHz, 64.5 at 100 kHz).
For a GUI that must not wait, the same in steps (not in MicroPython's API): `adc.capture(count,
freq)` starts and returns the exact rate, `adc.capture_done()`, `adc.capture_read(buf)`; the
oscilloscope example draws from them.

Measured: VREFINT at 1 kHz, 10 kHz and 100 kHz averages 1.206 V; the times between the first and
the last conversion give 1000.00 Hz, 9999.90 Hz and 99,989.4 Hz (TIM2's 208.9 MHz over 2089);
32768 samples at 100 kHz take 0.41 s with their collection (0.33 s of sampling); no overruns.
A real signal without wires: Linux charges A2 (PF14) as a GPIO output, the capture turns the pin
analog and samples it: 1.87 V, then a falling curve, about 12% less at each conversion, the same
from 10 to 100 kHz: the ADC's own sampling capacitor takes the charge. (A pin the ADC reads must be
analog: driven by Linux as an output, the M4 sees it toggle digitally, at 50.00 Hz, but the ADC
does not.)

Checked on the board (`tests/hil/test_m4.sh`, nothing wired): VREFINT reads 1.203 to 1.216 V
(datasheet 1.18 to 1.24 V; its factory calibration in OTP says 1.209 V), the six inputs convert,
an empty I2C bus answers `ENODEV`. Not checked yet: a real I2C device and a known voltage on an
input.

### PWM

TIM1 channel 1 on PE9, Arduino D6 (`PWM("D6")` or `PWM("PE9")`), the pin the device tree gives the
M4 for it. The MicroPython API: `freq()`, `duty_u16()`, `duty_ns()`, `init()`, `deinit()`; a new
frequency keeps the pulse width when it was set in nanoseconds, the share otherwise. A new duty
takes effect at the next period, without a glitch. From 1 Hz up; the timer counts at 208.9 MHz, so
the duty has 16 bits of resolution up to 3.2 kHz (about 11 bits at 100 kHz). `invert` is not
supported.

`mpy_m4.measure(pin, ms)` has the M4 sample a pin for a while (about 8 million samples a second)
and returns its frequency and the share of time it was high: that is how the tests check the PWM
without instruments. Measured on PE9: 1 kHz 25% gives 1000.00 Hz 25.00%; 50 Hz 1.5 ms gives
50.00 Hz 7.50%; 100 kHz 30% gives 99,990 Hz (208.9 MHz / 2089) 29.95%; 0% and 100% are steady.

### SPI

SPI4: SCK PE12 (D13), MISO PE13 (D12), MOSI PE14 (D11), on the Arduino connector. `SPI(4)` with the
MicroPython API: `init()`, `deinit()`, `read()`, `readinto()`, `write()`, `write_readinto()`,
`polarity`, `phase`, `firstbit` (`SPI.MSB`, `SPI.LSB`); 8-bit frames only. The rate is the fastest
not above the one asked: the 64 MHz kernel clock over 2, 4, ... 256, so 32 MHz down to 250 kHz.
Transfers longer than 400 bytes go in pieces of 400.

The chip select is the program's, as on most MicroPython boards: PE11 (D10) is not used by Linux,
so `Pin("PE11", Pin.OUT)` drives it through the kernel's GPIO lines, beside the SPI on the M4.

Checked without a device: during a transfer the M4 samples its own SCK and MOSI pins and rebuilds
the bytes it sees at the sampling edges; in the four modes, MSB and LSB first, at 500 kHz, they are
the bytes sent, with 2 edges a bit (the sampling keeps up to about 1 MHz). MISO reads 0xFF with
its internal pull-up and 0x00 with the pull-down. 1024 bytes at 32 MHz take 1.4 ms (732 kB/s); a
short transfer about 0.36 ms there and back. Not checked yet: a real SPI device.

### UART

UART7: RX PE7 (Arduino D0), TX PE8 (D1). `UART(7, baudrate)` with the MicroPython API: `init()`,
`deinit()`, `any()`, `read()`, `readinto()`, `readline()`, `write()`, `flush()`, `txdone()`,
`sendbreak()`, `timeout` and `timeout_char` (at least a character's time, as on MicroPython).
300 baud to 4 Mbaud; 7, 8 or 9 data bits (with parity 7 or 8), parity `None`, 0 (even) or 1
(odd), 1 or 2 stop bits. `errors()` (not in MicroPython) counts overruns, framing and parity errors.

The M4 receives and sends under interrupt, with a 4 KB buffer for what arrives and a 2 KB one for
what goes out: bytes are not lost between two calls from Python, and `write()` returns while the M4
sends.

Checked without wires: in single-wire mode the TX pin also receives what it sends. 8N1, 7E2, 8O1
from 300 baud to 4 Mbaud come back intact; 3 KB (more than the 2 KB to send) too; `readline()`,
the timeout (200 ms measured 200 ms), a break (read as 0x00 with a framing error). The M4 measures
the rate on the pin while sending 0x55 (a square wave at half the rate): 0.14% under nominal at
every rate, the same offset everywhere, well inside what a serial line tolerates. Not checked yet:
a real device on the line.

### Timer

`Timer(id, mode=Timer.PERIODIC, period=ms, callback=f)` or `freq=Hz`, `init()`, `deinit()`,
`Timer.ONE_SHOT`; 8 timers (0 to 7, or -1 for a free one), periods from 1 ms (up to 1000 Hz).

The M4 counts, Linux calls: the expiries are timed by the M4's clock and none is lost, while the
callback runs on Linux, in `mpy_m4`'s reader thread, when Linux gets to it (typically within a
millisecond; a 10 ms timer's gaps have a median of 10.000 ms, single ones from 5 to 15 ms). Until
Python has handled an event, the M4 sends no further one for that timer: it counts the expiries
into the next, and the callback finds them in `timer.missed` (not in MicroPython's API, like
`timer.late_us`, how late the M4 sent the event). So a busy Linux never piles events up.

As with MicroPython's soft callbacks, the callback waits while the main thread runs Python
without a pause: the GIL of MicroPython's unix port is not fair, and a loop that never sleeps,
waits or does I/O keeps it (the LVGL thread waits the same way). Measured at 1 kHz for 0.5 s of
such a loop: a few calls, the rest in `missed`, the count right. Anything that must happen at an
exact time belongs on the M4 itself, as the ADC's sampling at a fixed rate does.

### What the M4 has that is not used

| Peripheral | Why not |
|---|---|
| DAC | Its outputs PA4 and PA5 are the CC lines of the USB-C power connector (CN6), which tell the board what powers it; they do not reach the connectors. Left alone. |
| DMA2 | Not needed so far: an interrupt stores the samples up to 100 kHz. (TIM2 paces `read_timed`; `machine.Timer` uses the M4's cycle counter.) |
| HASH, CRYP, CRC, RNG | MicroPython on Linux has `hashlib` and `os.urandom`, the board `/dev/hwrng`. |

Linux keeps every timer off, so TIM4 (D3, D5) and TIM12 (D9) could give more PWM outputs, once
declared to the M4 in a device tree overlay. `machine.WDT` (`/dev/watchdog`) and `Pin.irq` (GPIO
edge events) belong on the Linux side.

## Next

- Captures that do not block Python (start, then collect), several channels in turn, DMA above
  100 kHz: for an oscilloscope app.
- More PWM outputs on TIM4 and TIM12; `machine.WDT` and `Pin.irq` on Linux.
- An I2C sensor and a known voltage on A0 for the HIL tests.
- A firmware that answers while Linux sleeps, and the M4 started at boot by a unit instead of at
  first use, if the 0.6 s matter.

## Layout

| Path | Content |
|---|---|
| `mcu/stm32mp157c-dk2/` | Cortex-M4 firmware: `Makefile`, `firmware/` (our sources) |
| `python/linux/mpy_m4.py`, `python/linux/machine.py` | The Python side, frozen in the firmware |
| `scripts/build_m4.sh`, `scripts/build_mp157_usb.sh` | Firmware and device tree builds |
| `tests/hil/test_m4.sh` | Tests of `machine.I2C`, `ADC`, `PWM`, `SPI`, `UART` and `Timer` on the board |
