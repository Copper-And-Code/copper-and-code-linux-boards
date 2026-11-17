# lvgl-mpy-linux

This is a personal, experimental project. It is not affiliated with, sponsored or endorsed by any
of the manufacturers of the boards and chips it uses; their names are used only to identify the
hardware.

MicroPython (unix port, v1.29.0) with LVGL 9.5 on the STM32MP135F-DK under OpenSTLinux, made to
behave like a MicroPython MCU board: the application runs on the board's display with touch, and
the PC sees a REPL that works with `mpremote` (raw REPL, raw-paste, soft reset) over USB CDC-ACM
or TCP. LVGL MPY Studio can then load and run its generated applications through the same
`display_driver` interface as on the MCU boards.

The project is local for now. It started as the `stm32mp135f-dk` branch of
[mpy-lvgl-boards](https://github.com/Copper-And-Code/mpy-lvgl-boards) and was moved here with its
history; nothing of it is published.

## Layout

| Path | Content |
|---|---|
| `boards/unix/STM32MP135F_DK/` | Out-of-tree unix variant: `mpconfigvariant.h/.mk`, frozen manifest |
| `modules/mpyrepl/` | MCU-style REPL (`pyexec`), soft reset by `execve`, safe mode on B1 |
| `modules/linuxfb/` | DRM dumb-buffer RGB565 scan-out |
| `modules/linuxproc/` | Linux programs on a pseudo-terminal, for the shell example |
| `python/linux/` | `display_driver.py` and `touch_evdev.py`, frozen |
| `linux/` | systemd units, USB composite gadget, DTB overlay, launcher |
| `scripts/` | `build_mp135.sh` (MicroPython), `build_mp135_usb.sh` (DTB) |
| `tests/hil/`, `tests/host/` | Tests on the board from the PC, and on the host |
| `examples/` | Linux shell example, LVGL MPY Studio demo project |
| `studio-board/` | The LVGL MPY Studio board definition, kept out of the Studio for now |
| `docs/` | Reconnaissance, build, installation, validation logs |
| `prompts/` | The working plan |
| `work/` | Not versioned: MicroPython and LVGL sources, SDK, Starter Package, host tools |

## Build

With the OpenSTLinux Developer Package SDK installed under `work/sdk/`:

```sh
scripts/build_mp135.sh lvgl
scripts/build_mp135_usb.sh
```

Installation on the board: `docs/stm32mp135f-dk-install.md`. Status and measurements:
`docs/stm32mp135f-dk.md`.

## License

MIT, see `LICENSE`.
