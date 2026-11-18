# STM32MP157C-DK2

Second board of this project, and the first with a microcontroller beside Linux: its Cortex-M4
serves `machine.I2C`, `ADC`, `PWM` and `SPI` to MicroPython on the Cortex-A7.

**Status:** OpenSTLinux flashed; our build installed with `scripts/deploy.sh` (the same MicroPython
binary as the STM32MP135F-DK); shell, app menu and REPL working (serial and TCP); the 480×800 panel
turned to 800×480 in software (about 50 full frames per second); Wi-Fi, weather and the web radio
with sound through the board's codec; `machine.Pin` on Linux GPIO lines, `machine.I2C`,
`ADC`, `PWM` and `SPI` on the Cortex-M4. Bring-up log: `docs/log/stm32mp157c-dk2-bringup.txt`.
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
from machine import I2C, ADC, PWM, SPI, Pin

i2c = I2C(5, freq=400000)        # Arduino D14 SDA / D15 SCL, 40-pin header pins 3 and 5
print(i2c.scan())
i2c.readfrom_mem(0x76, 0xD0, 1)  # a register, as on any MicroPython board

print(ADC("A0").read_uv())       # Arduino A0..A5, from 0 to 2.5 V
print(ADC(ADC.CORE_VREF).read_uv())

servo = PWM("D6", freq=50, duty_ns=1500000)   # TIM1 channel 1 on PE9, Arduino D6

spi = SPI(4, baudrate=8000000)                 # SCK D13, MISO D12, MOSI D11
cs = Pin("PE11", Pin.OUT, value=1)             # D10: a Linux GPIO line as chip select
cs(0); spi.read(3, 0x9F); cs(1)

Pin("PA14", Pin.IN, Pin.PULL_UP).value()   # Pin stays on Linux GPIO lines, as on the STM32MP135F-DK
```

How it fits together:

- **Device tree.** The `m4-examples` device tree gives the Cortex-M4 the Arduino connector's ADC,
  DAC, I2C5, SPI4, UART7, TIM1 and TIM2, and the orange LED; Linux keeps the display, touch, Wi-Fi,
  audio and USB. Linux only reserves those pins: the firmware sets them itself.
- **Firmware** (`mcu/stm32mp157c-dk2/`, built by `scripts/build_m4.sh` with `arm-none-eabi-gcc`,
  without STM32CubeIDE): STM32Cube HAL and OpenAMP from STM32CubeMP1 1.7.0, our `main.c` and the
  protocol. `install.sh` puts it in `/lib/firmware/mpy_m4.elf`.
- **Start.** The first use of `machine.I2C`, `ADC`, `PWM` or `SPI` loads the firmware through remoteproc
  if the M4 is off (about 0.6 s). A new firmware installed by `deploy.sh` takes over at once.
- **Link.** One RPMsg virtual serial port (`/dev/ttyRPMSG*`; the number changes when the M4
  restarts, so `mpy_m4` looks it up). Requests and answers carry a length, a sequence number and an
  errno: the protocol is in `mcu/stm32mp157c-dk2/firmware/Inc/mpy_m4_protocol.h`. A request takes
  about 0.35 ms there and back.
- **Python.** `mpy_m4` (frozen) has the classes; the frozen `machine` module takes the place of
  MicroPython's built-in one (an "extensible" module in v1.29), imports all of it from `umachine`
  and adds `I2C`, `ADC`, `PWM` and `SPI` on a board with a Cortex-M4. On the STM32MP135F-DK nothing changes.

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

## Next

- `Timer`, `UART` (UART7 on D0/D1) and the DAC; more PWM channels (TIM1 channels 2 and 4 share
  pins with SPI4).
- An I2C sensor and a known voltage on A0 for the HIL tests.
- A firmware that answers while Linux sleeps, and the M4 started at boot by a unit instead of at
  first use, if the 0.6 s matter.

## Layout

| Path | Content |
|---|---|
| `mcu/stm32mp157c-dk2/` | Cortex-M4 firmware: `Makefile`, `firmware/` (our sources) |
| `python/linux/mpy_m4.py`, `python/linux/machine.py` | The Python side, frozen in the firmware |
| `scripts/build_m4.sh`, `scripts/build_mp157_usb.sh` | Firmware and device tree builds |
| `tests/hil/test_m4.sh` | Tests of `machine.I2C`, `ADC`, `PWM` and `SPI` on the board |
