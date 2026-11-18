# machine examples

Apps that show what MicroPython's `machine` API does on a Linux board, made with LVGL MPY Studio
and its MCP tools. They use the standard API (`Pin`, `ADC`, `PWM`, `I2C`, `SPI`, `UART`,
`Timer`), so their hardware code is the same as on a microcontroller board; what serves each class
is the board's business:

| Board | `Pin` | `ADC`, `PWM`, `I2C`, `SPI`, `UART`, `Timer` |
|---|---|---|
| STM32MP157C-DK2 | Linux GPIO lines | the Cortex-M4 (`mpy_m4`) |
| STM32MP135F-DK | Linux GPIO lines | not yet |

Each app says what it needs, and in the Studio simulator, where there is no hardware, a stand-in
gives changing values so the screen can be tried without a board.

| App | Wires | What it shows |
|---|---|---|
| [board-explorer](board-explorer) | none | The Arduino connector: each pin as an input, an output, an ADC or the PWM, live; the Cortex-M4 reads back what Linux drives and measures its own PWM; LEDs from Linux's LED class, `machine.Pin` and the M4 |
| [oscilloscope](oscilloscope) | one, D6 to A0 | A0 to A5 sampled up to 100 kHz by the Cortex-M4's hardware, trigger, measurements; a test signal on D6 |
| [timing-lab](timing-lab) | none | Every 10 ms three ways: a Python sleep, `machine.Timer` callbacks, the M4's count; load Linux and see who keeps the time |
| [logic-analyzer](logic-analyzer) | none | Port E sampled up to 4 MHz by the M4 while the board's own UART, SPI and PWM send; decoded and drawn on Linux |
| [web-panel](web-panel) | none | A page for the phone: inputs live, LEDs, PWM, a message to the screen; QR code and PIN on the board |
| [data-logger](data-logger) | none | Samples at the pace of an M4 timer, a dated CSV file (NTP), a chart, the file for a browser |
