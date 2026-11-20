# machine examples

Apps that show what MicroPython's `machine` API does on a Linux board, made with LVGL MPY Studio
and its MCP tools. They use the standard API (`Pin`, `ADC`, `PWM`, `I2C`, `SPI`, `UART`,
`Timer`), so their hardware code is the same as on a microcontroller board; what serves each class
is the board's business:

| Board | `Pin` | `ADC`, `PWM`, `I2C`, `SPI`, `UART`, `Timer` |
|---|---|---|
| STM32MP157C-DK2 | Linux GPIO lines | the Cortex-M4 (`mpy_m4`) |
| STM32MP135F-DK | Linux GPIO lines | Linux's drivers (`machine_kernel`: i2c-dev, spidev, ttys, PWM, IIO, a thread for `Timer`) |
| STM32F769I-DISCO (a microcontroller, [mpy-lvgl-boards](https://github.com/Copper-And-Code/mpy-lvgl-boards)) | MicroPython's own `machine` | MicroPython's own `machine` |

Any other Linux board without a microcontroller gets the second kind with a description file and
no code: see [machine on Linux's drivers](../../docs/machine-linux.md). The apps take the board's
connector, pins and LEDs from that description too (`mpy_board`), so the same app runs on both
boards: `project-800x480.json` for the STM32MP157C-DK2, `project.json` (480×272) for the
STM32MP135F-DK.

Each app says what it needs, and in the Studio simulator, where there is no hardware, a stand-in
gives changing values so the screen can be tried without a board.

| App | Boards | Wires | What it shows |
|---|---|---|---|
| [board-explorer](board-explorer) | both, and the STM32F769I-DISCO | none | The board's connector: each pin as an input, an output, an ADC or a PWM, live; on the DK2 the Cortex-M4 reads back what Linux drives and measures the PWM; the board's LEDs |
| [oscilloscope](oscilloscope) | DK2 | one, D6 to A0 | A0 to A5 sampled up to 100 kHz by the Cortex-M4's hardware, trigger, measurements; a test signal on D6 |
| [timing-lab](timing-lab) | both | none | Every 10 ms three ways: a Python sleep, `machine.Timer` callbacks, the count of expiries; load Linux and see who keeps the time |
| [logic-analyzer](logic-analyzer) | DK2 | none | Port E sampled up to 4 MHz by the M4 while the board's own UART, SPI and PWM send; decoded and drawn on Linux |
| [web-panel](web-panel) | both | none | A page for the phone: inputs live, LEDs, PWM, a message to the screen; QR code and PIN on the board |
| [data-logger](data-logger) | both | none | Samples at the pace of `machine.Timer`, a dated CSV file (NTP), a chart, the file for a browser |
| [tone-player](tone-player) | DK2 | a passive buzzer on D6 (or none: the M4 measures) | A keyboard on the screen, tunes the M4 plays on time by itself, each key lit as it sounds |
| [system-monitor](system-monitor) | both | none | Linux's cores, memory, temperature and network, and what serves `machine`: the M4's state and request time, or the kernel's devices and an ADC reading's time |

The oscilloscope, the logic analyser and the tone player stay on the STM32MP157C-DK2: they need
work done on time by a microcontroller (100 kHz sampling, 4 MHz on a whole port, notes played by
themselves), which a Linux board alone cannot promise.
