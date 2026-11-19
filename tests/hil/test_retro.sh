#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# mpy-retro on the board: one game for 15 seconds, full screen, with the board's own environment
# (rotation, sound device). Passes when the core keeps its speed and nearly every frame is shown.
#
#   tests/hil/test_retro.sh GAME_ON_THE_BOARD [CORE] [BOARD]
#   MPY_SSH_ALIAS=stm32mp157c-dk2 tests/hil/test_retro.sh "/usr/local/mpy/fs/games/snes/My game.sfc"
#
# The game is yours, already on the board: none comes with this repository. MicroPython stops
# for the test (the screen is the frontend's) and starts again after it.
set -euo pipefail
GAME=${1:?usage: test_retro.sh GAME_ON_THE_BOARD [CORE] [BOARD]}
CORE=${2:-/usr/local/mpy/retro/cores/snes9x2005_libretro.so}
BOARD=${3:-192.168.7.1}
SSH_OPTIONS=(-o BatchMode=yes -o ConnectTimeout=5)
[[ -n ${MPY_SSH_ALIAS:-} ]] && SSH_OPTIONS+=(-o "HostKeyAlias=$MPY_SSH_ALIAS")

# The service's environment (MPY_DISPLAY_ROTATION, MPY_AUDIO_SINK...), as the app would give it.
OUTPUT=$(ssh "${SSH_OPTIONS[@]}" "root@$BOARD" "
    env_line=\$(systemctl show -p Environment mpy-repl | cut -d= -f2-)
    systemctl stop mpy-repl
    eval \"env \$env_line /usr/local/mpy/bin/mpy-retro '$CORE' '$GAME' --frames 900\" 2>&1
    status=\$?
    systemctl start mpy-repl
    exit \$status")
echo "$OUTPUT"
python3 - "$OUTPUT" <<'EOF'
import re, sys
text = sys.argv[1]
match = re.search(r"([\d.]+) fps, ([\d.]+) shown a second", text)
if not match:
    sys.exit("FAIL: no frame counts")
fps, shown = float(match.group(1)), float(match.group(2))
print(("PASS" if fps >= 59 else "FAIL") + ": %.2f emulated frames a second" % fps)
print(("PASS" if shown >= 57 else "FAIL") + ": %.2f shown a second" % shown)
sys.exit(0 if fps >= 59 and shown >= 57 else 1)
EOF
