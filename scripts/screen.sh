#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# What the board's panel shows now, as a PNG on the PC, turned as the user sees it.
#
#   scripts/screen.sh [out.png] [BOARD]      (default screen.png, 192.168.7.1)
#
# MPY_SSH_ALIAS as for scripts/deploy.sh.
set -euo pipefail
OUT=${1:-screen.png}
BOARD=${2:-192.168.7.1}
SSH=(ssh -o BatchMode=yes ${MPY_SSH_ALIAS:+-o HostKeyAlias=$MPY_SSH_ALIAS} "root@$BOARD")
# The panel's rotation, while the panel is the screen in use (not on a monitor).
"${SSH[@]}" 'r=$(systemctl show mpy-repl -p Environment | tr " " "\n" | sed -n "s/^MPY_DISPLAY_ROTATION=//p");
             grep -qx enabled /sys/class/drm/card*-DSI-*/enabled /sys/class/drm/card*-LVDS-*/enabled 2>/dev/null || r=0;
             /usr/local/mpy/bin/mpy-screenshot -r "${r:-0}"' > "$OUT"
echo "$OUT"
