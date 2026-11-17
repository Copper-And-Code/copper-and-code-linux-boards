#!/bin/sh
# Restore the OpenSTLinux configuration on the STM32MP135F-DK: original device tree, original USB
# gadget, Weston and its demo. Run on the board as root, then reboot.
#
#   uninstall.sh [--purge]    --purge also removes /usr/local/mpy (programs and the fs/ folder)
set -eu

PREFIX=/usr/local/mpy
UNITS=/etc/systemd/system
DTB=/boot/stm32mp135f-dk.dtb

say() { printf 'uninstall: %s\n' "$*"; }
[ "$(id -u)" = 0 ] || { echo "uninstall: run as root" >&2; exit 1; }

for unit in mpy-repl.service mpy-usb-composite.service mpy-repl-tcp.socket; do
    systemctl disable --now "$unit" >/dev/null 2>&1 || true
done
rm -f "$UNITS/mpy-repl.service" "$UNITS/mpy-usb-composite.service" \
    "$UNITS/mpy-repl-tcp.socket" "$UNITS/mpy-repl-tcp@.service"
say "removed the MicroPython services"

if [ -e /etc/systemd/journald.conf.d/mpy-volatile.conf ]; then
    rm -f /etc/systemd/journald.conf.d/mpy-volatile.conf
    systemctl restart systemd-journald
    say "journal on the microSD again"
fi
# The original gadget startup uses its defaults again.
rm -f /etc/default/usbotg

if [ -e "$DTB.st-original" ]; then
    cp -p "$DTB.st-original" "$DTB"
    say "restored the original DTB"
else
    say "no $DTB.st-original: the DTB was left as it is"
fi

for unit in seatd-weston.service weston-checkgpu.service weston-graphical-session.service \
    psplash-drm-start.service st-hostname.service; do
    systemctl enable "$unit" >/dev/null 2>&1 && say "enabled $unit" || true
done
systemctl daemon-reload

if [ "${1:-}" = "--purge" ]; then
    rm -rf "$PREFIX"
    say "removed $PREFIX"
else
    say "$PREFIX kept (use --purge to remove it)"
fi
say "done; reboot to start Weston with the original device tree"
