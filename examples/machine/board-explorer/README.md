# Board Explorer

![Board Explorer](../../../docs/images/board-explorer.png)

The STM32MP157C-DK2's Arduino connector on the screen, made with Copper and Code Studio and its MCP tools.
Tap a pin and choose what it does; its value is live. Nothing needs to be wired.

| Pins | What you can do | Served by |
|---|---|---|
| D2, D3, D4, D5, D7, D8, D9, D10 | input, input with pull-up, output | Linux GPIO lines (`machine.Pin`); for an output, the Cortex-M4 reads back the level the pin really has |
| D6 | PWM at 50 Hz, 1 kHz or 20 kHz, duty 0 to 100% | the M4's TIM1 (`machine.PWM`), measured on the pin by the M4 (`mpy_m4.measure`) |
| A0 to A5 | voltage, 0 to 2.5 V | the M4's ADC (`machine.ADC`); unwired inputs float |
| D14, D15 | scan of the I2C bus | `machine.I2C(5)` on the M4 |
| D11 to D13, D0, D1 | (SPI4, UART7) | shown; used through `machine.SPI(4)` and `machine.UART(7)` |

The three LEDs come from three places: the blue one (PD11) through Linux's LED class
(`/sys/class/leds/heartbeat`: off gives its blinking back to the kernel), the red one (PA13)
through `machine.Pin`, the orange one (PH7) through the Cortex-M4. The green LED (PA14) is also the
USER1 button, whose interrupt the device tree gives the M4: Linux may read that pin but not drive
it.

## On the board

Open `project-800x480.json` in Copper and Code Studio, connect to the STM32MP157C-DK2 and run it. The first use of
the M4 starts its firmware (about half a second).

## On the STM32MP135F-DK

![Board Explorer on the 40-pin connector, in the Studio simulator](../../../docs/images/board-explorer-mp135.png)

`project.json` (480×272): the same app on the 40-pin connector CN8, numbered as on a Raspberry Pi,
in a grid of its 28 signal pins. The board's description says what each pin is: GPIO, the ADC on
pins 8, 10, 13 and 15 (0 to 3.3 V), PWM on 7, 31 and 32, I2C5 on 3 and 5, SPI5, UART8 and I2C1, all on
Linux's drivers ([machine on Linux's drivers](../../../docs/machine-linux.md)); the green and orange
LEDs (LD7, LD6) on the I/O expander. With no microcontroller there, nobody reads back an output or
measures the PWM on its pin: the screen says so. In the Studio simulator `project.json` shows this
board with made-up values, `project-800x480.json` the STM32MP157C-DK2.

## On a microcontroller board: the STM32F769I-DISCO

The same app runs on the STM32F769I-DISCO with its 800×480 MB1166 display, a microcontroller board
from [mpy-lvgl-boards](https://github.com/Copper-And-Code/mpy-lvgl-boards): there `machine` is
MicroPython's own, and the board's description (`mpy_board`, frozen in its firmware) gives the
Arduino connector. Import the board into Copper and Code Studio from `studio-board.json` in that board's
folder, open `project-800x480.json`, pick the board and run.

| Pins | What you can do | Served by |
|---|---|---|
| D2, D4, D7, D8 | input, input with pull-up, output | `machine.Pin`; for an output the MCU reads the pin's level back |
| D3, D5, D6, D9, D10 | GPIO modes, or PWM | TIM10, TIM8, TIM11, TIM12, TIM1 (`machine.PWM`); up to 5 kHz the MCU times the signal on its pin (`machine.time_pulse_us`, 1 to 4% high at 1 kHz), above that it says it cannot |
| A0 to A5 | voltage, 0 to 3.3 V | `machine.ADC` (A3 to A5 on ADC3); `read_u16()` scaled, as STM32's ADC has no `read_uv()` |
| D14, D15 | scan of the I2C bus | `machine.I2C(1)` |
| D11 to D13, D0, D1 | (SPI2, UART6) | shown; used through `machine.SPI(2)` and `machine.UART(6)` |

The red and green LEDs (LD1, LD2) are `machine.Pin`. On the STM32 a PWM gets a new frequency by
being made again: a lower prescaler takes effect only at the counter's next overflow, which at the
old one can come 0.65 s later.

## Files

| Module | What it does |
|---|---|
| `board` | the connector's pins and what serves them; the hardware, or the simulator's stand-in |
| `explorer_app` | the screen: pin buttons, modes, values, LEDs; updates from the page's timer |
