# SPDX-License-Identifier: MIT
# The settings every app shares, in one JSON file: settings.json in the board's folder
# (/usr/local/mpy/fs on the STM32MP135F-DK, the folder mpremote sees as ":"), or the path in
# MPY_SETTINGS. Sections such as "wifi" hold the values of one subject:
#
#   {"wifi": {"ssid": "Home", "password": "..."}}
#
# Values are kept in clear, Wi-Fi password included: whoever can read the board's files can read them.

import json
import os

PATH = os.getenv("MPY_SETTINGS") or "settings.json"


def load():
    try:
        with open(PATH) as source:
            data = json.load(source)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def get(section, default=None):
    value = load().get(section)
    return value if isinstance(value, dict) else default


def save(data):
    # Through a temporary file, so a power cut leaves either the old file or the new one.
    temporary = PATH + ".tmp"
    with open(temporary, "w") as target:
        json.dump(data, target)
    os.rename(temporary, PATH)


def update(section, values):
    data = load()
    data[section] = values
    save(data)
