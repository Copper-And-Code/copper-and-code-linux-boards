# Examples

Each example is a Copper and Code Studio project (`project.json`, with its Python modules inside), made
with the Studio's MCP tools. Open it in the Studio, try it in the simulator (demo data stands in for
the network and the sound), connect to the board and run it. To have it in the home screen's menu,
add it to `settings.json`, section `home`.

Each example has two projects with the same Python modules: `project.json` for 480×272 (STM32MP135F-DK)
and `project-800x480.json` for 800×480 (STM32MP157C-DK2). Sizes drawn by the code follow the widgets,
so the modules fit both.

Source: [examples on GitHub](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples).

## Weather

![Weather](images/weather.png) ![Weather settings](images/weather-settings.png)

Weather now and for the next five days from [Open-Meteo](https://open-meteo.com) (no account, no
key), with icons drawn by LVGL objects from the WMO weather code: no image files. The settings page
finds the place by name and sets the units, the update interval and the night: by default the
backlight goes down to 20% from 23:00 to 05:30, in the place's own time. `curl` downloads in the
background, so the screen never waits.
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/weather)

## Web radio

![Web radio](images/web-radio.png)

Internet radio stations, built in or from a `stations.txt`, which the Find page fills from the
[Radio Browser](https://www.radio-browser.info) catalogue by country, region, language and genre. GStreamer's `gst-play-1.0` plays the
stream in the background through `linuxproc.Process`; the app connects to the Wi-Fi of the Settings
app by itself. Needs a sound card: a USB audio adapter is enough.
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/web-radio)

## MP3 player

![MP3 player](images/mp3-player.png)

The songs of the `music` folder, with titles and artists from their ID3 tags: play, pause, previous,
next, volume, progress. The same player module as the radio.
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/mp3-player)

## Settings

![Settings](images/settings.png)

Wi-Fi: scan, choose, type the password on the touch keyboard, connect; and the layout of a
keyboard plugged into the board. The choices go to `settings.json`, where every app finds it through `mpy_settings` and `mpy_wifi.ensure()`.
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/settings)

## Shell

A terminal on the display, the home screen at power-on: built-in commands, the `vi` and `edit`
editors, Python programs and Linux programs on a pseudo-terminal, with a USB keyboard plugged into the
board or the keyboard of the serial or TCP terminal. A tap opens the app menu.
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/shell-linux)

## Games

![Games](images/games.png)

Your games, full screen, through libretro cores: the list is LVGL, the game is `mpy-retro`, a
frontend in C that has the screen, the sound and the keyboard while it runs, then gives them back.
SNES (Snes9x2005), C64 (the core of the STM32H743 port) and Neo Geo and arcade (FBNeo) on the
STM32MP157C-DK2, on its panel or a monitor; SNES on the STM32MP135F-DK too. Arrows and Enter,
the mouse or a tap choose; Esc, the Windows key or Select + Start come back. See
[Games: mpy-retro](retro.md).
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/games)

## FTP Server

![FTP Server](images/ftp-server.png)

The board's files for a PC on the network: the whole SD card, the MicroPython folder or the Studio
apps, with FileZilla, WinSCP, Windows Explorer or a file manager. The screen shows the address, the
user and the password (kept in `settings.json`), who is connected and what moves. A MicroPython
server polled from the GUI's timer: files move in pieces, so the screen never waits; about 4 MB/s
to the card over the USB network. Plain FTP: for your own network.
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/ftp-server)

## machine: the hardware

Apps that show MicroPython's `machine` API on a Linux board, in
[`examples/machine`](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/machine).
Their hardware code is the standard API, the same as on a microcontroller; on the STM32MP157C-DK2
`Pin` is served by Linux and the rest by the Cortex-M4, on the STM32MP135F-DK all of it by Linux's
own drivers ([machine on Linux's drivers](machine-linux.md)). Board Explorer, Timing Lab, Web Panel,
Data Logger and System Monitor run on both boards; the oscilloscope, the logic analyser and the tone
player need the Cortex-M4.

### Board Explorer

![Board Explorer](images/board-explorer.png)

The Arduino connector on the screen: tap a pin, make it an input, an output, an analog input or
the PWM, see its value live. The Cortex-M4 reads back the level of an output Linux drives and
measures its own PWM on the pin; the LEDs come from Linux's LED class, `machine.Pin` and the M4.
Nothing to wire.
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/machine/board-explorer)

### Oscilloscope

![Oscilloscope](images/oscilloscope-simulator.png)

One of A0 to A5 on a chart, 0.25 to 20 ms a division, triggered on an edge, with minimum, maximum,
peak to peak, frequency and duty, and a 1 kHz test signal on D6: one wire from D6 to A0 and the
square wave is on the screen. The Cortex-M4's TIM2 paces the conversions in hardware, up to
100,000 samples a second; Python starts a capture, collects it when it is done and draws it, so
the screen never waits. (Above: the Studio simulator.)
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/machine/oscilloscope)

### Timing Lab

![Timing Lab](images/timing-lab.png)

Something every 10 ms, three ways: a Python thread that sleeps, `machine.Timer` callbacks, and the
expiries the Cortex-M4 counts. A histogram shows the intervals; a switch loads Linux with two busy
processes. Measured: even idle, Linux stretches some intervals to tens of milliseconds (the GIL
while the screen is drawn), loaded to over 100 ms; the M4's count stays whole, with no drift.
What must happen at an exact time belongs on the M4. Nothing to wire.
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/machine/timing-lab)

### Logic Analyzer

![Logic Analyzer](images/logic-analyzer.png)

The board watches its own buses, no wires: the Cortex-M4 samples GPIO port E at up to 4 MHz while
it sends a UART message, an SPI transfer or the PWM, started with the capture; Python decodes them
(UART 8N1, SPI mode 0, PWM) and draws one row a pin, the bytes over their frames.
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/machine/logic-analyzer)

### Web Panel

![Web Panel](images/web-panel.png)

The board serves a page to the phones of its network: scan the QR code, type the PIN on the screen,
see A0 to A5 live, switch the LEDs, set the PWM, send a message to the board's screen, which shows
what each browser did. A few lines of MicroPython, polled from the GUI's timer: about 16 ms a
request. (Above: the Studio simulator.)
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/machine/web-panel)

### Data Logger

![Data Logger](images/data-logger.png)

The CPU's temperature (Linux), A0 and the chip's reference (the Cortex-M4), at the pace of an M4
timer, into a CSV file dated by the network's clock, on a chart, and for a browser to download.
Each part where it is best: the M4 keeps the pace, Linux keeps the files and the time.
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/machine/data-logger)

### Tone Player

![Tone Player](images/tone-player.png)

A keyboard on the screen and tunes the Cortex-M4 plays by itself, on time, on D6 (a passive
buzzer): Python hands over the whole score and lights each key as the M4 plays it. Without a
buzzer the M4 measures each note on the pin.
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/machine/tone-player)

### System Monitor

![System Monitor](images/system-monitor.png)

The board's two halves live: Linux's cores, memory, temperature and network from its own files,
and the Cortex-M4's state, firmware, channel and the time of a request there and back.
[Source](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/examples/machine/system-monitor)

