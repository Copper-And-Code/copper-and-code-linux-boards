#!/bin/sh
# SPDX-License-Identifier: MIT
# Restore the OpenSTLinux configuration on the STM32MP135F-DK or STM32MP157C-DK2: original device
# tree, original USB gadget, Weston and its demo. Run on the board as root, then reboot.
#
#   uninstall.sh [--purge]    --purge also removes /usr/local/mpy (programs and the fs/ folder)
set -eu

PREFIX=/usr/local/mpy
UNITS=/etc/systemd/system

say() { printf 'uninstall: %s\n' "$*"; }
[ "$(id -u)" = 0 ] || { echo "uninstall: run as root" >&2; exit 1; }
case $(tr '\0' ' ' < /proc/device-tree/compatible 2>/dev/null) in
    *st,stm32mp157c-dk2*) DTB=/boot/stm32mp157c-dk2.dtb ;;
    *) DTB=/boot/stm32mp135f-dk.dtb ;;
esac

# The Cortex-M4 firmware of mpy_m4 (STM32MP157C-DK2): stopped and removed.
M4=/sys/class/remoteproc/remoteproc0
if [ -e /lib/firmware/mpy_m4.elf ]; then
    if [ "$(cat $M4/firmware 2>/dev/null)" = mpy_m4.elf ] && [ "$(cat $M4/state)" = running ]; then
        echo stop > $M4/state
    fi
    rm -f /lib/firmware/mpy_m4.elf
    say "removed the Cortex-M4 firmware"
fi

if grep -q "lvgl-mpy-linux" /etc/bluetooth/main.conf 2>/dev/null; then
    rm -f /etc/bluetooth/main.conf
    say "removed the Bluetooth configuration"
fi
if [ -d "/lib/modules/$(uname -r)/updates/mpy" ]; then
    rm -rf "/lib/modules/$(uname -r)/updates/mpy"
    depmod -a
    say "removed the gamepad kernel modules"
fi

for unit in mpy-repl.service mpy-usb-composite.service mpy-repl-tcp.socket; do
    systemctl disable --now "$unit" >/dev/null 2>&1 || true
done
rm -f "$UNITS/mpy-repl.service.d/board.conf"
rmdir "$UNITS/mpy-repl.service.d" 2>/dev/null || true
rm -f "$UNITS/mpy-repl.service" "$UNITS/mpy-usb-composite.service" \
    "$UNITS/mpy-repl-tcp.socket" "$UNITS/mpy-repl-tcp@.service"
say "removed the MicroPython services"

if [ -e /etc/systemd/journald.conf.d/mpy-volatile.conf ]; then
    rm -f /etc/systemd/journald.conf.d/mpy-volatile.conf
    systemctl restart systemd-journald
    say "journal on the microSD again"
fi
# Wi-Fi configured by the Settings app (mpy_wifi): only files that carry its marker.
for file in /etc/systemd/network/25-mpy-wifi.network /etc/wpa_supplicant/wpa_supplicant-*.conf; do
    if [ -f "$file" ] && grep -q "^# written by mpy_wifi" "$file"; then
        unit=$(basename "$file" .conf | sed -n "s/^wpa_supplicant-//p")
        [ -n "$unit" ] && systemctl disable --now "wpa_supplicant@$unit" >/dev/null 2>&1
        rm -f "$file"
        say "removed $file"
    fi
done
# The original gadget startup uses its defaults again: ST's file back where the image had one.
if [ -e /etc/default/usbotg.st-original ]; then
    mv /etc/default/usbotg.st-original /etc/default/usbotg
else
    rm -f /etc/default/usbotg
fi

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
