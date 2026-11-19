#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# A keyboard plugged into the board types into the REPL: a virtual one (uinput) types on the
# shell of the screen while the TCP REPL watches the echo. Checks letters, shift, Caps Lock, the
# symbols of the layout the board uses (us or it, AltGr included), the cursor keys, Ctrl-C and the
# home key.
#
#   tests/hil/test_keyboard.sh [BOARD]      (default 192.168.7.1; MPY_SSH_ALIAS as for deploy.sh)
#
# Needs the SDK in work/sdk/ to build the virtual keyboard, and the screen's shell at its prompt.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BOARD=${2:-${1:-192.168.7.1}}
SSH_OPTIONS=(-o BatchMode=yes -o ConnectTimeout=5)
[[ -n ${MPY_SSH_ALIAS:-} ]] && SSH_OPTIONS+=(-o "HostKeyAlias=$MPY_SSH_ALIAS")
on_board() { ssh "${SSH_OPTIONS[@]}" "root@$BOARD" "$@"; }

SDK=$(ls -d "$REPO"/work/sdk/openstlinux-*/ | head -1)
TYPER=$(mktemp -d)/keyboard_typer
(unset LD_LIBRARY_PATH; set +u
 # shellcheck disable=SC1090
 source "$SDK"/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
 # shellcheck disable=SC2086
 $CC $CFLAGS -Wall -Wextra -Werror -o "$TYPER" "$REPO/tests/hil/keyboard_typer.c" $LDFLAGS)
scp -q "${SSH_OPTIONS[@]}" "$TYPER" "root@$BOARD:/tmp/keyboard_typer"
on_board 'modprobe uinput 2>/dev/null; true'
# The layout the hub uses now: from settings.json (the Settings app), or the unit's MPY_KEYBOARD.
LAYOUT=$(on_board 'journalctl -u mpy-repl -b -o cat --no-pager' | grep -o 'layout [a-z]*' | tail -1 | cut -d' ' -f2)
LAYOUT=${LAYOUT:-us}
echo "Keyboard layout: $LAYOUT"

# The keys of the symbols, and what they type, for each layout.
if [[ $LAYOUT == it ]]; then
    SYMBOLS="altgr+leftbrace altgr+rightbrace altgr+shift+leftbrace altgr+shift+rightbrace altgr+semicolon
             altgr+apostrophe altgr+equal altgr+minus shift+grave grave 102nd shift+102nd shift+7 shift+3
             leftbrace altgr+e"
    EXPECT='[]{}@#~`|\<>/£è€'
    QUOTE=minus
else
    SYMBOLS="leftbrace rightbrace shift+leftbrace shift+rightbrace shift+2 shift+3 shift+grave grave
             shift+backslash backslash shift+comma shift+dot slash"
    EXPECT='[]{}@#~`|\<>/'
    QUOTE=apostrophe
fi
# shellcheck disable=SC2086
SYMBOLS=$(echo $SYMBOLS)  # one line: it goes into the command run on the board

python3 - "$BOARD" "$EXPECT" <<'EOF' &
import re, socket, sys, time
board, expect = sys.argv[1], sys.argv[2]
s = socket.create_connection((board, 8266))
s.settimeout(0.3)
out = b""
end = time.time() + 12
while time.time() < end:
    try:
        out += s.recv(4096)
    except socket.timeout:
        pass
text = re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", out).replace(b"\r", b"").decode("utf-8", "replace")
checks = [
    ("letters, shift and Caps Lock", "echo Kb1 AB"),
    ("the symbols of the layout", "'%s'" % expect),
    ("cursor keys and history", "echo Kb1 ABx"),
    ("Ctrl-C", "sleep 20\n^C"),
]
failed = False
for what, wanted in checks:
    ok = wanted in text
    failed |= not ok
    print(("PASS: " if ok else "FAIL: ") + what)
if failed:
    print(text[-800:])
    sys.exit(1)
EOF
WATCH=$!
sleep 1
on_board "/tmp/keyboard_typer ctrl+u e c h o space shift+k b 1 space capslock a b capslock enter \
    ctrl+u e c h o space $QUOTE $SYMBOLS $QUOTE enter \
    ctrl+u up up x enter \
    ctrl+u s l e e p space 2 0 enter; sleep 1; /tmp/keyboard_typer ctrl+c"
wait $WATCH

# The home key: the Windows key alone and the Menu key count (the menu opens, then closes), the
# Windows key with another key does not.
COUNTS=$(on_board 'cat /run/mpy-home-key 2>/dev/null || echo 0; /tmp/keyboard_typer meta meta+e menu ctrl+u >/dev/null;
    sleep 0.5; cat /run/mpy-home-key; rm /tmp/keyboard_typer')
read -r BEFORE AFTER <<<"$(echo $COUNTS)"
[[ $((AFTER - BEFORE)) == 2 ]] || { echo "FAIL: home key counted $BEFORE -> $AFTER"; exit 1; }
echo "PASS: home key"
echo "PASS: keyboard"
