#!/bin/sh
# SPDX-License-Identifier: MIT
# Install MicroPython, the REPL hub and the services on the STM32MP135F-DK. Run on the board as
# root from a directory that holds the files (scripts/deploy_mp135.sh copies them):
#
#   micropython  mpy-repl-hub  stm32mp135f-dk.dtb  (each with an optional .sha256)
#   mpy_launcher.py  mpy-usb-composite.sh  mpy-usb-composite.service  mpy-repl.service
#   usbotg.default  journald-volatile.conf
#
# It can be run again: unchanged files are left alone. SSH settings are not touched. Environment:
#   MPY_HOSTNAME  hostname to set (default mp135-mpy; empty leaves it unchanged)
set -eu

SOURCE=$(cd "$(dirname "$0")" && pwd)
PREFIX=/usr/local/mpy
UNITS=/etc/systemd/system
DTB=/boot/stm32mp135f-dk.dtb
HOSTNAME_WANTED=${MPY_HOSTNAME-mp135-mpy}
reboot_needed=0

say() { printf 'install: %s\n' "$*"; }
die() { printf 'install: error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root"
grep -q "st,stm32mp135f-dk" /proc/device-tree/compatible 2>/dev/null || die "not an STM32MP135F-DK"
for file in micropython mpy-repl-hub stm32mp135f-dk.dtb mpy_launcher.py mpy-usb-composite.sh \
    mpy-usb-composite.service mpy-repl.service usbotg.default journald-volatile.conf; do
    [ -f "$SOURCE/$file" ] || die "missing $SOURCE/$file"
    if [ -f "$SOURCE/$file.sha256" ]; then
        expected=$(cut -d' ' -f1 "$SOURCE/$file.sha256")
        actual=$(sha256sum "$SOURCE/$file" | cut -d' ' -f1)
        [ "$expected" = "$actual" ] || die "$file does not match its SHA-256"
    fi
done

# Copy only when different; through a temporary name, so a running program is never overwritten.
changed=""
put() { # source target mode
    if [ -f "$2" ] && cmp -s "$1" "$2"; then
        return 0
    fi
    cp "$1" "$2.new"
    chmod "$3" "$2.new"
    mv "$2.new" "$2"
    changed="$changed $2"
    say "installed $2"
}

install -d "$PREFIX/bin" "$PREFIX/lib" "$PREFIX/fs/lib"
put "$SOURCE/micropython" "$PREFIX/bin/micropython" 0755
put "$SOURCE/mpy-repl-hub" "$PREFIX/bin/mpy-repl-hub" 0755
put "$SOURCE/mpy-usb-composite.sh" "$PREFIX/bin/mpy-usb-composite" 0755
put "$SOURCE/mpy_launcher.py" "$PREFIX/lib/mpy_launcher.py" 0644
put "$SOURCE/mpy-usb-composite.service" "$UNITS/mpy-usb-composite.service" 0644
put "$SOURCE/mpy-repl.service" "$UNITS/mpy-repl.service" 0644
put "$SOURCE/usbotg.default" /etc/default/usbotg 0644
install -d /etc/systemd/journald.conf.d
journal_before=$changed
put "$SOURCE/journald-volatile.conf" /etc/systemd/journald.conf.d/mpy-volatile.conf 0644
if [ "$changed" != "$journal_before" ]; then
    # The journal moves to /run now; the logs already on the microSD are removed.
    systemctl restart systemd-journald
    rm -rf /var/log/journal/*
    say "journal kept in RAM only"
fi

# The original DTB is kept once, for uninstall.sh and for recovery from the ST-LINK console.
if [ ! -e "$DTB.st-original" ]; then
    cp -p "$DTB" "$DTB.st-original"
    say "kept the original DTB as $DTB.st-original"
fi
before=$(sha256sum "$DTB" | cut -d' ' -f1)
put "$SOURCE/stm32mp135f-dk.dtb" "$DTB" 0644
[ "$(sha256sum "$DTB" | cut -d' ' -f1)" = "$before" ] || reboot_needed=1

# Earlier installations ran one MicroPython per TCP connection.
if [ -e "$UNITS/mpy-repl-tcp.socket" ] || [ -e "$UNITS/mpy-repl-tcp@.service" ]; then
    systemctl disable --now mpy-repl-tcp.socket 2>/dev/null || true
    rm -f "$UNITS/mpy-repl-tcp.socket" "$UNITS/mpy-repl-tcp@.service"
    say "removed the per-connection TCP REPL units"
fi

# Weston and the boot splash hold DRM master; st-hostname appends a MAC suffix to the hostname.
for unit in weston-graphical-session.service weston-checkgpu.service seatd-weston.service \
    psplash-drm-start.service st-hostname.service; do
    if systemctl is-enabled "$unit" >/dev/null 2>&1; then
        systemctl disable --now "$unit" >/dev/null 2>&1 || true
        say "disabled $unit"
    fi
done
if [ -n "$HOSTNAME_WANTED" ] && [ "$(hostname)" != "$HOSTNAME_WANTED" ]; then
    hostnamectl set-hostname "$HOSTNAME_WANTED"
    say "hostname set to $HOSTNAME_WANTED"
fi

systemctl daemon-reload
for unit in mpy-usb-composite.service mpy-repl.service; do
    if ! systemctl is-enabled "$unit" >/dev/null 2>&1; then
        systemctl enable "$unit" >/dev/null 2>&1
        say "enabled $unit"
    fi
done
[ -e /sys/kernel/config/usb_gadget/g1/functions/acm.0 ] || reboot_needed=1

if [ "$reboot_needed" = 1 ]; then
    say "done; reboot to use the new device tree and USB gadget"
elif [ -n "$changed" ]; then
    systemctl restart mpy-repl.service
    say "done; mpy-repl.service restarted"
else
    say "done; nothing changed"
fi
