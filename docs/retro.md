# Games: mpy-retro

`mpy-retro` is a libretro frontend in C for these boards: it loads one core (a `.so`), runs one game
full screen and gives the screen back to MicroPython when the game ends. An LVGL app chooses the
game ([the Games example](examples.md#games)); the emulation is C, on its own CPU.

## Measured on the STM32MP157C-DK2

Snes9x2005, the core of the earlier STM32H743 port, built for the Cortex-A7 at 650 MHz. One core
emulates, every frame drawn, 600 frames after 1800 of warm-up (its benchmark build, no screen):

| Game | Emulation and drawing | Budget | STM32H743 at 480 MHz |
|---|---:|---:|---:|
| A platform game, 1990 | 9.2 ms | 16.7 ms | 24.0 ms |
| A racing game in Mode 7, 1990 | 13.1 ms (slowest 16.1) | 16.7 ms | 24.9 ms |
| An action adventure, 1991 | 11.3 ms | 16.7 ms | 26.1 ms |

The final image of the platform game matches the PC build's bit for bit. Through the frontend, with
the sound pacing it and the scaling on the other CPU, the platform game runs at 60.0 emulated frames
a second on a 1280×720 monitor and on the turned 800×480 panel. On the panel, 59.8 of them a second
reach the screen: the panel is switched to its 60 Hz mode (it prefers 50), the scaled and turned
frame takes 3.6 ms to draw, and three buffers keep a frame that comes while another waits for the
blank (the few missing come from 60.04 Hz of the SNES against 60.00 of the panel).

## The C64

The C64 core of the same STM32H743 and RP2350 projects, in this repository as a libretro core
([cores/c64](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/cores/c64)): 6510, CIAs,
VIC-II, SID, the emulated 1541 and cartridges. `.prg`, `.d64` (loaded and run by itself) and `.crt`
files start from the Games list. On the STM32MP157C-DK2 it runs at 50.1 emulated and 49.9 shown
frames a second (the panel switched to 50 Hz), 5.4 ms to draw a frame; while a disk loads, three
frames run for each one shown, about 2.5 times real speed.

The whole keyboard is the C64's, by position (as VICE's positional map): Esc RUN/STOP, Tab CTRL,
left Ctrl C=, Home CLR/HOME, Insert £, Page Up ↑, the backquote ←, F2 F4 F6 F8 as the shifted
function keys. The joystick is in port 2: a gamepad, or the keypad's 8 4 6 2 and 0; the gamepad's
Select moves it to port 1. The Windows key, or Select and Start together, come back to the list.

The Commodore ROMs are the user's, in `/usr/local/mpy/retro/system/c64/` (see
[the core's notes](https://github.com/Copper-And-Code/lvgl-mpy-linux/tree/main/cores/c64)). The
demo used to check it, [Nine](https://www.linusakesson.net/scene/nine/) by Linus Åkesson, comes
from its author's page.

## Neo Geo and the arcade: FBNeo

FBNeo, the libretro core of reference for the Neo Geo, built for our CPU (`platform=rpi2`: Cortex-A7
with NEON) with Cyclone, its 68000 in ARM assembly. The full build, which also brings CPS1, CPS2,
Sega System 16 and the other arcade systems FBNeo knows; romsets are FBNeo's (`.zip`), the BIOS
`neogeo.zip` goes next to the Neo Geo games and is not listed. The Neo Geo runs at 59.19 frames a
second; measured on the STM32MP157C-DK2:

| Game | As fast as it goes (no sound, no pacing) | With the sound pacing it: emulated, shown |
|---|---:|---:|
| A puzzle game, 1994 | 74.5 | 58.8, 58.1 |
| An action platformer, 1990 | 71.5 | |
| A shoot-'em-up, 1998 | 66.8 | |
| A shoot-'em-up, 1995 | 66.6 | 58.7, 57.7 |
| A fighting game, 1997 | 59.4 | 54.4; with its 68000 at 85 %: 59.1, 45.8 |
| A fighting game, 1999 | 56.7 | 55.0; with its 68000 at 85 %: 59.0, 46.3 |

(As fast as it goes: the first four over their first 15 seconds, mostly the BIOS screen and
the demo; the two fighting games over a minute, with the sound interpolation off. With the sound: a
minute each; the emulated frames count the second a game takes to start.)

Most games keep their speed as they are. The heaviest ones (the fighting games of the late 1990s) have scenes beyond a
Cortex-A7 at 650 MHz: there the frontend skips drawing a frame when the sound runs low (never two in
a row), and a game's own options can lower the emulated 68000's clock, as FBNeo allows:
`GAME.opt` next to `GAME.zip` with `fbneo-cpu-speed-adjust = "85%"`. Then the 1999 fighting game keeps 59.0 frames
a second, about four in five of them drawn. `options.cfg` turns FBNeo's sound interpolation off for
all games (it costs what those games need).

FBNeo is fetched at a fixed commit and built in `work/`; it has non-commercial terms. The BIOS and
the games are the user's.

## The frontend

| | |
|---|---|
| Screen | DRM/KMS: the screen `display_driver` would choose (a monitor before the panel), at the refresh nearest the game's, three buffers flipped on the vertical blank. A second thread scales the frame to the largest size with the core's aspect, and turns it with the panel (`MPY_DISPLAY_ROTATION`) |
| Sound | ALSA, the device of `MPY_AUDIO_SINK` (the headphone jack, or HDMI on a monitor). Writing the samples paces the game; without a sound card a clock does |
| Input | evdev keyboards and gamepads as player 1's RetroPad, also those plugged in during the game (gamepad drivers: `scripts/build_kmods.sh`). The REPL hub lends the keyboards while `/run/mpy-keyboard-lent` names the frontend's live process |
| Keyboard | A core that reads the keyboard (the C64) gets all of it: then it is no RetroPad and Esc is the core's. A key pressed and released between two polls still counts once |
| Leaving | Esc (unless the core has the keyboard), the Windows key, Select and Start together, the gamepad's home button, SIGTERM |
| Saves | The core's save RAM next to the game, as `GAME.srm` |
| ROMs | The cores' system folder: `/usr/local/mpy/retro/system` (`MPY_RETRO_SYSTEM`) |
| Options | `key = "value"` lines as in RetroArch: `GAME.opt` next to a game, then `/usr/local/mpy/retro/options.cfg` (`MPY_RETRO_OPTIONS`) |
| Keeping time | When the sound buffer runs low the next frame is emulated but not drawn (never two in a row); `--bench` runs without sound or pacing, to measure the headroom |

```sh
mpy-retro /usr/local/mpy/retro/cores/snes9x2005_libretro.so "/usr/local/mpy/fs/games/snes/My game.sfc"
```

Only with the screen free: from MicroPython, `mpy_retro.play(path)` does it all.

## Building

```sh
scripts/build_retro.sh      # the cores at a fixed commit, and the frontend, with the board's SDK
scripts/deploy.sh           # installs dist/retro/ when present
```

Snes9x2005 and FBNeo are fetched and built in `work/`; their sources and licenses stay their own
and are not part of this repository (both have non-commercial terms). The C64 core is in
`cores/c64`, MIT. The frontend and `libretro.h` are MIT. No game, demo or ROM comes with the board
or this repository.

## Next

- More cores, each measured the same way before it is listed: NES, Game Boy, Mega Drive, PC Engine,
  Game Boy Advance, PlayStation.
- A real C64 joystick or SNES pad on the Arduino connector, read by the Cortex-M4.
