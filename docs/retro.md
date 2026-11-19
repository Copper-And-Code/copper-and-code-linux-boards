# Games: mpy-retro

`mpy-retro` is a libretro frontend in C for these boards: it loads one core (a `.so`), runs one game
full screen and gives the screen back to MicroPython when the game ends. An LVGL app chooses the
game ([the Games example](examples.md#games)); the emulation is C, on its own CPU.

## Measured on the STM32MP157C-DK2

Snes9x2005, the core of the earlier STM32H743 port, built for the Cortex-A7 at 650 MHz. One core
emulates, every frame drawn, 600 frames after 1800 of warm-up (its benchmark build, no screen):

| Game | Emulation and drawing | Budget | STM32H743 at 480 MHz |
|---|---:|---:|---:|
| Super Mario World | 9.2 ms | 16.7 ms | 24.0 ms |
| F-Zero | 13.1 ms (slowest 16.1) | 16.7 ms | 24.9 ms |
| Zelda: A Link to the Past | 11.3 ms | 16.7 ms | 26.1 ms |

The final image of Super Mario World matches the PC build's bit for bit. Through the frontend, with
the sound pacing it and the scaling on the other CPU, Super Mario World runs at 60.0 emulated frames
a second on a 1280×720 monitor and on the turned 800×480 panel. On the panel, 59.8 of them a second
reach the screen: the panel is switched to its 60 Hz mode (it prefers 50), the scaled and turned
frame takes 3.6 ms to draw, and three buffers keep a frame that comes while another waits for the
blank (the few missing come from 60.04 Hz of the SNES against 60.00 of the panel).

The C64 core of the same projects runs 3.3 times faster than real time on one Cortex-A7: it is the
next core to wrap.

## The frontend

| | |
|---|---|
| Screen | DRM/KMS: the screen `display_driver` would choose (a monitor before the panel), at the refresh nearest the game's, three buffers flipped on the vertical blank. A second thread scales the frame to the largest size with the core's aspect, and turns it with the panel (`MPY_DISPLAY_ROTATION`) |
| Sound | ALSA, the device of `MPY_AUDIO_SINK` (the headphone jack, or HDMI on a monitor). Writing the samples paces the game; without a sound card a clock does |
| Input | evdev keyboards and gamepads as player 1's RetroPad, also those plugged in during the game. The REPL hub lends the keyboards while `/run/mpy-keyboard-lent` names the frontend's live process |
| Leaving | Esc, the Windows key, Select and Start together, the gamepad's home button, SIGTERM |
| Saves | The core's save RAM next to the game, as `GAME.srm` |

```sh
mpy-retro /usr/local/mpy/retro/cores/snes9x2005_libretro.so "/usr/local/mpy/fs/games/snes/My game.sfc"
```

Only with the screen free: from MicroPython, `mpy_retro.play(path)` does it all.

## Building

```sh
scripts/build_retro.sh      # the cores at a fixed commit, and the frontend, with the board's SDK
scripts/deploy.sh           # installs dist/retro/ when present
```

The cores are fetched and built in `work/`; their sources and licenses stay theirs and are not part of
this repository (Snes9x has non-commercial terms: see its `copyright` file). The frontend and
`libretro.h` are MIT. No game comes with the board or this repository.

## Next

- The C64 core of the STM32H743 port as a libretro core, with its keyboard and joystick.
- More cores, each measured the same way before it is listed: NES, Game Boy, Mega Drive, PC Engine,
  Game Boy Advance, arcade, PlayStation.
- A real C64 joystick or SNES pad on the Arduino connector, read by the Cortex-M4.
