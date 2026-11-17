#!/usr/bin/env bash
# Copy the build outputs to the STM32MP135F-DK over SSH and run linux/install.sh there.
#
#   scripts/deploy_mp135.sh [BOARD]        (default BOARD=192.168.7.1)
#
# Environment: MICROPYTHON, HUB, DTB select files in dist/ (default: the newest of each);
# MPY_HOSTNAME is passed to install.sh.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
BOARD=${1:-192.168.7.1}
newest() { ls -t "$REPO"/dist/$1 2>/dev/null | grep -v '\.sha256$' | head -1; }
MICROPYTHON=${MICROPYTHON:-$(newest 'micropython-STM32MP135F_DK-lvgl-*')}
HUB=${HUB:-$(newest 'mpy-repl-hub-*')}
DTB=${DTB:-$(newest 'stm32mp135f-dk-usb-composite-*.dtb')}
for file in "$MICROPYTHON" "$HUB" "$DTB"; do
    [[ -n $file && -f $file ]] || { echo "deploy: build first (scripts/build_mp135.sh, build_mp135_usb.sh)" >&2; exit 1; }
done

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
stage "$DTB" stm32mp135f-dk.dtb
for file in mpy_launcher.py mpy-usb-composite.sh mpy-usb-composite.service mpy-repl.service \
    usbotg.default install.sh uninstall.sh; do
    cp "$REPO/linux/$file" "$STAGE/$file"
done

echo "deploy: $(basename "$MICROPYTHON"), $(basename "$HUB"), $(basename "$DTB") -> $BOARD"
ssh "root@$BOARD" 'rm -rf /tmp/mpy-install && mkdir -p /tmp/mpy-install'
scp -q "$STAGE"/* "root@$BOARD:/tmp/mpy-install/"
ssh "root@$BOARD" "MPY_HOSTNAME=${MPY_HOSTNAME-mp135-mpy} sh /tmp/mpy-install/install.sh && install -m 0755 /tmp/mpy-install/uninstall.sh /usr/local/mpy/bin/mpy-uninstall"
