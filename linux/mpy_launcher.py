# SPDX-License-Identifier: MIT
import mpyrepl
import os
import sys

safe_mode = mpyrepl.user_button_pressed()
if safe_mode:
    print("Safe mode: main.py skipped")
try:
    os.stat("main.py")
    has_main = True
except OSError:
    has_main = False
if safe_mode or not has_main:
    # The REPL comes first: a display that fails, or a Ctrl-C from a client connecting now, only
    # costs the ready screen.
    try:
        import mpy_ready

        mpy_ready.show(safe_mode)
    except (Exception, KeyboardInterrupt) as error:
        print("ready screen:", repr(error))
sys.exit(mpyrepl.run(safe_mode))
