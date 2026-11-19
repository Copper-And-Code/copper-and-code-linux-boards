# Games

![Games](../../docs/images/games.png)

Your games, full screen, through libretro cores. The list is an LVGL app made with LVGL MPY Studio
and its MCP tools; the game is `mpy-retro`, a frontend in C, which has the screen, the sound and the
keyboard while it runs. When the game ends the list comes back. For now on the STM32MP157C-DK2
(800×480): SNES through Snes9x2005, at full speed with every frame drawn.

## Setting up

1. Build the frontend and the cores, then deploy: `scripts/build_retro.sh` and `scripts/deploy.sh`.
2. Copy your games into `/usr/local/mpy/fs/games` on the board, one folder per system if you like:

   ```sh
   mpremote connect /dev/ttyACM1 mkdir :games/snes
   mpremote connect /dev/ttyACM1 cp "My game.sfc" :games/snes/
   ```

   No game comes with the board or this repository: use dumps of cartridges you own.
3. Upload this project from the Studio and add it to the home menu (`settings.json`, section `home`).

## Playing

| | Keyboard | Gamepad (USB) |
|---|---|---|
| Choose a game | arrows and Enter, or the mouse, or a tap | |
| D-pad | arrows | d-pad, hat or left stick |
| A, B, X, Y | X, Z, S, A | east, south, north, west buttons |
| L, R | Q, W | shoulders |
| Start, Select | Enter, Shift | Start, Select |
| Back to the list | Esc or the Windows key | Select and Start together, or the home button |

The sound goes to the headphone jack, or to HDMI on a monitor. The save RAM is kept next to the game,
as `GAME.srm`. A keyboard or a gamepad plugged in during the game works at once.

## How it works

`games_app.py` lists the games with `mpy_retro.games()` and starts one with `mpy_retro.play(path)`,
which releases the screen (`display_driver.release()`), runs `mpy-retro CORE.so GAME` through
`linuxproc.Process` and takes the screen back (`display_driver.resume()`). The REPL hub lends the
keyboards to the frontend while it runs. The arrows and Enter of the list come from `mpy_keys`.

In the Studio simulator three demo names stand in for the games.
