#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copy the build outputs to an STM32MP135F-DK or STM32MP157C-DK2 over SSH and run linux/install.sh
# there. The board is recognised from its device tree.
#
#   scripts/deploy.sh [BOARD]        (default BOARD=192.168.7.1)
#
# Environment:
#   MICROPYTHON, HUB, DTB  files in dist/ (default: the newest of each; the DTB the board's)
#   M4_ELF                 the Cortex-M4 firmware for the STM32MP157C-DK2 (scripts/build_m4.sh)
#   dist/retro/            the games frontend and its cores, installed when present
#   dist/kmods/            gamepad kernel modules (STM32MP157C-DK2), installed when present
#                          (scripts/build_retro.sh)
#   MPY_SSH_ALIAS          name to check the board's SSH host key against, for boards that share
#                          an address (both answer at 192.168.7.1), for example stm32mp157c-dk2
#   MPY_HOSTNAME, MPY_AUTOSTART  passed to install.sh when set
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
BOARD=${1:-192.168.7.1}
SSH_OPTIONS=(-o BatchMode=yes)
[[ -n ${MPY_SSH_ALIAS:-} ]] && SSH_OPTIONS+=(-o "HostKeyAlias=$MPY_SSH_ALIAS")
on_board() { ssh "${SSH_OPTIONS[@]}" "root@$BOARD" "$@"; }

newest() { ls -t "$REPO"/dist/$1 2>/dev/null | grep -v '\.sha256$' | head -1; }
MICROPYTHON=${MICROPYTHON:-$(newest 'micropython-STM32MP135F_DK-lvgl-*')}
HUB=${HUB:-$(newest 'mpy-repl-hub-*')}
SHOT=${SHOT:-$(newest 'mpy-screenshot-*')}
for file in "$MICROPYTHON" "$HUB" "$SHOT"; do
    [[ -n $file && -f $file ]] || { echo "deploy: build first (scripts/build_mp135.sh)" >&2; exit 1; }
done

COMPATIBLE=$(on_board "tr '\\0' ' ' < /proc/device-tree/compatible")
case $COMPATIBLE in
    *st,stm32mp135f-dk*) DTB_NAME=stm32mp135f-dk.dtb; DTB_BUILD=scripts/build_mp135_usb.sh ;;
    *st,stm32mp157c-dk2*) DTB_NAME=stm32mp157c-dk2.dtb; DTB_BUILD=scripts/build_mp157_usb.sh
        M4_ELF=${M4_ELF:-$(newest 'mpy_m4-*.elf')}
        [[ -n $M4_ELF && -f $M4_ELF ]] || { echo "deploy: build the M4 firmware first (scripts/build_m4.sh)" >&2; exit 1; } ;;
    *) echo "deploy: unknown board: $COMPATIBLE" >&2; exit 1 ;;
esac
DTB=${DTB:-$(newest "${DTB_NAME%.dtb}-usb-composite-*.dtb")}
[[ -n $DTB && -f $DTB ]] || { echo "deploy: build the DTB first ($DTB_BUILD)" >&2; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
stage() { # source name
    cp "$1" "$STAGE/$2"
    if [[ -f $1.sha256 ]]; then
        printf '%s  %s\n' "$(cut -d' ' -f1 "$1.sha256")" "$2" > "$STAGE/$2.sha256"
    fi
}
stage "$MICROPYTHON" micropython
stage "$HUB" mpy-repl-hub
stage "$SHOT" mpy-screenshot
stage "$DTB" "$DTB_NAME"
[[ -n ${M4_ELF:-} ]] && stage "$M4_ELF" mpy_m4.elf
for file in mpy_launcher.py mpy_tap.py mpy-usb-composite.sh mpy-usb-composite.service mpy-repl.service bluetooth-main.conf \
    usbotg.default journald-volatile.conf install.sh uninstall.sh; do
    cp "$REPO/linux/$file" "$STAGE/$file"
done
cp "$REPO/examples/shell-linux/edit.py" "$REPO/examples/shell-linux/vi.py" "$STAGE/"
# Kernel modules for gamepads, when built for this board's kernel (scripts/build_kmods.sh).
if [[ -d $REPO/dist/kmods && $COMPATIBLE == *st,stm32mp157c-dk2* ]]; then
    mkdir -p "$STAGE/kmods"
    cp "$REPO"/dist/kmods/*.ko "$STAGE/kmods/"
fi
# The games frontend and its cores, when built (scripts/build_retro.sh).
if [[ -f $REPO/dist/retro/mpy-retro ]]; then
    stage "$REPO/dist/retro/mpy-retro" mpy-retro
    mkdir -p "$STAGE/retro-cores"
    cp "$REPO"/dist/retro/cores/*_libretro.so "$STAGE/retro-cores/"
fi

echo "deploy: ${COMPATIBLE%% *}: $(basename "$MICROPYTHON"), $(basename "$HUB"), $(basename "$DTB") -> $BOARD"
on_board 'rm -rf /tmp/mpy-install && mkdir -p /tmp/mpy-install'
scp -q -r "${SSH_OPTIONS[@]}" "$STAGE"/* "root@$BOARD:/tmp/mpy-install/"
ENVIRONMENT=""
[[ -n ${MPY_HOSTNAME+set} ]] && ENVIRONMENT+="MPY_HOSTNAME='$MPY_HOSTNAME' "
[[ -n ${MPY_AUTOSTART:-} ]] && ENVIRONMENT+="MPY_AUTOSTART='$MPY_AUTOSTART' "
on_board "${ENVIRONMENT}sh /tmp/mpy-install/install.sh && install -m 0755 /tmp/mpy-install/uninstall.sh /usr/local/mpy/bin/mpy-uninstall"
