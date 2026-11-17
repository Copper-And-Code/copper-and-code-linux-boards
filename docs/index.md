# lvgl-mpy-linux

**Turn a Linux board into a MicroPython board with a touch screen.** Write the GUI in Python with
LVGL, use `mpremote` as on any MicroPython board, and have all of Linux behind it: files, network,
audio, and every program the board can run.

<div class="shot-grid" markdown>
<figure markdown>
![Weather](images/weather.png)
<figcaption>Weather: five days, drawn icons, night dimming</figcaption>
</figure>
<figure markdown>
![Web radio](images/web-radio.png)
<figcaption>Web radio: internet stations</figcaption>
</figure>
<figure markdown>
![MP3 player](images/mp3-player.png)
<figcaption>MP3 player: songs from the SD card</figcaption>
</figure>
<figure markdown>
![Settings](images/settings.png)
<figcaption>Settings: Wi-Fi for every app</figcaption>
</figure>
</div>

All these apps were built with [LVGL MPY Studio](https://copper-and-code.github.io/lvgl-mpy-studio/)
through its MCP tools: an AI assistant designed the screens, wrote the code, tried it in the
Studio's simulator and ran it on the board.

> **Status: [mp135-v0.1](https://github.com/Copper-And-Code/lvgl-mpy-linux/releases/tag/mp135-v0.1).**
> One board tested, the STM32MP135F-DK. A personal, experimental project for makers, not affiliated
> with, sponsored or endorsed by any of the manufacturers of the boards and chips it uses; their
> names only identify the hardware.

<div class="home-grid" markdown>
<div class="home-card" markdown>

## Get started

Install it on an STM32MP135F-DK running OpenSTLinux, then talk to it with `mpremote`.

[Getting started](stm32mp135f-dk-install.md){ .md-button .md-button--primary }

</div>
<div class="home-card" markdown>

## Examples

Weather, web radio, MP3 player, Settings and a shell on the screen, each a Studio project.

[See the examples](examples.md){ .md-button .md-button--primary }

</div>
<div class="home-card" markdown>

## How it is verified

Measurements, test results and known issues, with the logs of every check on the board.

[The STM32MP135F-DK](stm32mp135f-dk.md){ .md-button }

</div>
</div>

## Why

A microcontroller running MicroPython is wonderful for a quick touch GUI: plug in USB, copy a file,
it runs. A Linux board can do much more (play music, stream radio, talk to the internet, run any
program) but usually means Yocto, a desktop compositor and C++ toolkits. lvgl-mpy-linux gives the
Linux board the microcontroller workflow:

- **The same app on MCU and Linux.** The [`display_driver`](display-driver.md) interface is the one
  of the MicroPython MCU boards of [mpy-lvgl-boards](https://github.com/Copper-And-Code/mpy-lvgl-boards):
  an app made for an RP2350 or an ESP32 runs unchanged on a Linux board with a bigger screen.
- **It behaves like a MicroPython board.** A REPL on USB serial and on TCP that works with
  `mpremote` (raw REPL, raw-paste, `cp`, `run`, `mount`), Ctrl-C, Ctrl-D soft reset in 0.2 s, safe
  mode on a button.
- **A home screen.** At power-on a shell on the display (built-in commands, `vi` and `edit`, Python
  and Linux programs); a tap opens a menu of apps, such as the weather and the radio, and a power
  off.
- **Linux in reach of Python.** `linuxproc` runs Linux programs in the background and lets the GUI
  drive them; `machine.Pin` drives the GPIO lines of the kernel; `mpy_wifi` and `mpy_settings` give
  every app the network and the settings.

## LVGL MPY Studio and its MCP tools

[LVGL MPY Studio](https://copper-and-code.github.io/lvgl-mpy-studio/) designs LVGL screens by drag
and drop, runs them in a simulator and uploads them to the board. Import the board definition
(`studio-board/boards/stm32mp135f_dk_480x272.json`) in its board manager and the Linux board is one
more target.

The Studio also offers its tools through MCP, so an AI coding assistant can build a whole
application: add pages and widgets, write handlers and Python modules, render the pages, play
touch scripts in the simulator, upload to the board and read its console. That is how the examples
here were made.

## How it works

```text
 PC: LVGL MPY Studio / mpremote
        │ USB serial (CDC-ACM)        │ TCP over USB network
        └──────────────┬──────────────┘
                 mpy-repl-hub            one REPL for both, Ctrl-C as a signal, systemd watchdog
                       │ pseudo-terminal
     MicroPython unix port + LVGL 9.5    REPL like the MCU ports, soft reset by execve
       ├── display_driver ── linuxfb ──── DRM panel
       │                  └─ touch_evdev ─ touchscreen
       ├── machine.Pin ─────────────────── GPIO character device
       └── linuxproc ───────────────────── Linux programs (gst-play, curl, shell, ...)
```

MicroPython, LVGL and the LVGL binding are used unchanged: everything is an out-of-tree variant,
user C modules and frozen Python.

## Boards

| Board | Status |
|---|---|
| STM32MP135F-DK (480×272, OpenSTLinux) | Tested: build, installation, REPL, display, touch, GPIO, Wi-Fi, examples |
| STM32MP157C-DK2 (with a Cortex-M4) | Planned: `machine` served by the Cortex-M4 over RPMsg ([plan](stm32mp157c-dk2.md)) |
| Other Linux boards | The Linux parts are generic; a port needs a build for its CPU |

What a Linux board needs: a panel the kernel drives through DRM, a touchscreen through evdev, and a
USB device port for the serial REPL (or only the network for the TCP REPL).

## Limits

Honestly, today:

- One board tested. The parts for other Linux boards are written but not tried on one yet.
- Start-up takes about 33 s on the stock OpenSTLinux image.
- `machine` has `Pin` only: no `I2C`, `PWM`, `WDT` or pin interrupts yet.
- No vsync: fast full-screen animations can tear.
- The TCP REPL has no password: it listens only on the USB network.

MicroPython, LVGL and the LVGL binding of PyDevices do the hard work. MIT license.
