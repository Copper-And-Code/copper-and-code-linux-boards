import mpyrepl
import sys

safe_mode = mpyrepl.user_button_pressed()
if safe_mode:
    print("Safe mode: main.py skipped")
sys.exit(mpyrepl.run(safe_mode))
