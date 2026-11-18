#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# A tap on the board's touchscreen, at screen coordinates as the user sees them.
#
#   scripts/tap.sh X Y [MILLISECONDS] [BOARD]      (default 100 ms, 192.168.7.1)
#
# MPY_SSH_ALIAS as for scripts/deploy.sh.
set -euo pipefail
X=$1 Y=$2 HOLD=${3:-100} BOARD=${4:-192.168.7.1}
SSH=(ssh -o BatchMode=yes ${MPY_SSH_ALIAS:+-o HostKeyAlias=$MPY_SSH_ALIAS} "root@$BOARD")
"${SSH[@]}" "r=\$(systemctl show mpy-repl -p Environment | tr ' ' '\n' | sed -n 's/^MPY_DISPLAY_ROTATION=//p');
             cd /usr/local/mpy/fs && MPY_DISPLAY_ROTATION=\${r:-0} MICROPYPATH=.frozen \
             /usr/local/mpy/bin/micropython /usr/local/mpy/lib/mpy_tap.py $X $Y $HOLD"
