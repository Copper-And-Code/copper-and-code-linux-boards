# SPDX-License-Identifier: MIT
# Games through libretro cores. mpy-retro, a frontend in C, runs one full screen with its own sound
# and input while MicroPython waits; the app's screen comes back when the game ends (Esc, the
# Windows key, or Select and Start together on a gamepad).
#
#   import mpy_retro
#   mpy_retro.games()                    # [(name, path), ...] in /usr/local/mpy/fs/games
#   mpy_retro.play(path)                 # (exit status, the frontend's last messages)
#
# The core comes from the game's extension (SYSTEMS); MPY_GAMES names another games folder. The
# games are the user's own files: none comes with the board.

import os
import time

FRONTEND = "/usr/local/mpy/bin/mpy-retro"
CORES = "/usr/local/mpy/retro/cores"
GAMES = os.getenv("MPY_GAMES") or "/usr/local/mpy/fs/games"
SYSTEMS = {
    "smc": "snes9x2005",
    "sfc": "snes9x2005",
    "fig": "snes9x2005",
    "swc": "snes9x2005",
}


def _extension(path):
    name = path.rsplit("/", 1)[-1]
    return name.rsplit(".", 1)[-1].lower() if "." in name else ""


def core_for(path):
    name = SYSTEMS.get(_extension(path))
    if name is None:
        raise ValueError("no core for " + path)
    core = "%s/%s_libretro.so" % (CORES, name)
    os.stat(core)  # OSError when the core is not installed
    return core


def _is_dir(path):
    try:
        return os.stat(path)[0] & 0x4000 != 0
    except OSError:
        return False


def games(folder=GAMES):
    """The games in folder and in its subfolders (one per system, for example), by name."""
    found = []
    try:
        names = os.listdir(folder)
    except OSError:
        return found
    for name in names:
        path = folder + "/" + name
        if _is_dir(path):
            found.extend(games(path))
        elif _extension(name) in SYSTEMS:
            found.append((name.rsplit(".", 1)[0], path))
    found.sort()
    return found


def play(path, core=None):
    """Runs the game full screen and waits for it. Returns (exit status, the last messages)."""
    import display_driver
    import linuxproc

    core = core or core_for(path)
    display_driver.release()
    output = b""
    status = None
    try:
        process = linuxproc.Process([FRONTEND, core, path])
        while status is None:
            data = process.read()
            if data:
                output = (output + data)[-2000:]
            status = process.poll()
            if status is None:
                time.sleep_ms(50)
        data = process.read()
        if data:
            output = (output + data)[-2000:]
    finally:
        display_driver.resume()
    return status, output.decode("utf-8", "replace")
