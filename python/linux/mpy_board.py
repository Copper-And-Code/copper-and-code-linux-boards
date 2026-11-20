# SPDX-License-Identifier: MIT
# What this board has, from its description: a JSON file installed with the firmware
# (/usr/local/mpy/board.json; MPY_BOARD names another), one per board in linux/boards/ of the
# repository. machine on Linux's drivers (machine_kernel) takes from it which bus, channel or line
# each name is; apps take the connector, the names of the pins and the LEDs, so that the same app
# runs on every board that has a description.
#
#   import mpy_board
#   mpy_board.describe()["board"]          # "STM32MP135F-DK"
#   mpy_board.pin("A0")                    # "PA5": an alias of the board's, or the name itself
#   mpy_board.connector()                  # ("CN8", "GPIO expansion ...", {"1": "3V3", "3": "PH6", ...})

import os

FILE = os.getenv("MPY_BOARD") or "/usr/local/mpy/board.json"

_described = None


def describe():
    """The board's description, a dict; {} without one (the simulator, an unknown board)."""
    global _described
    if _described is None:
        try:
            import json

            with open(FILE) as f:
                _described = json.load(f)
        except (OSError, ValueError):
            _described = {}
    return _described


def pin(name):
    """The SoC's name of a pin given by an alias of the board ("A0", "D6") or by its own name."""
    return describe().get("aliases", {}).get(name.upper(), name.upper())


def connector():
    """(name, title, {pin label: signal}) of the board's main connector, or None. Labels are the
    connector's own: numbers ("38") on a 40-pin header, names ("D6", "A0") on an Arduino one."""
    found = describe().get("connector")
    if not found:
        return None
    return found.get("name", ""), found.get("title", ""), found.get("pins", {})
