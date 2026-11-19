#!/bin/sh
# SPDX-License-Identifier: MIT
# Install MicroPython, the REPL hub and the services on an STM32MP135F-DK or STM32MP157C-DK2. Run
# on the board as root from a directory that holds the files (scripts/deploy.sh copies them):
#
#   micropython  mpy-repl-hub  mpy-screenshot  (each with an optional .sha256)
#   stm32mp135f-dk.dtb or stm32mp157c-dk2.dtb  (the board's: USB FIFOs; on the STM32MP135F-DK
#                                               also the B1 button)
#   mpy_launcher.py  mpy-usb-composite.sh  mpy-usb-composite.service  mpy-repl.service
#   usbotg.default  journald-volatile.conf  edit.py  vi.py
#
# It can be run again: unchanged files are left alone. SSH settings are not touched. Environment:
#   MPY_HOSTNAME  hostname to set (default mp135-mpy or mp157-mpy; empty leaves it unchanged)
#   MPY_AUTOSTART home (default): the shell and the app menu at power-on, through a main.py
#                 written only when there is none; none: no main.py
set -eu

SOURCE=$(cd "$(dirname "$0")" && pwd)
PREFIX=/usr/local/mpy
UNITS=/etc/systemd/system
AUTOSTART=${MPY_AUTOSTART:-home}
reboot_needed=0

say() { printf 'install: %s\n' "$*"; }
die() { printf 'install: error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root"
# What differs between the boards: the device tree file, the screen's orientation, the hostname.
case $(tr '\0' ' ' < /proc/device-tree/compatible 2>/dev/null) in
    *st,stm32mp135f-dk*)
        BOARD="STM32MP135F-DK"
        DTB_NAME=stm32mp135f-dk.dtb  # with the USB FIFOs of the composite gadget and B1 on PA14
        ROTATION=0
        AUDIO_SINK=""                # GStreamer's default (a USB audio adapter, for example)
        AUDIO_SINK_MONITOR=""        # no HDMI
        M4_FIRMWARE=""               # no Cortex-M4
        REALTIME=""                  # one core: a Python loop at real-time priority would starve Linux
        HOSTNAME_DEFAULT=mp135-mpy ;;
    *st,stm32mp157c-dk2*)
        BOARD="STM32MP157C-DK2"
        # ST's "m4-examples" device tree (the Arduino connector's ADC, I2C5 and more for the
        # Cortex-M4) with the USB FIFOs of the composite gadget
        DTB_NAME=stm32mp157c-dk2.dtb
        ROTATION=90                  # its 480x800 portrait panel as an 800x480 landscape screen
        # The CS42L51 codec (headphone jack) directly: ALSA's default device goes to PulseAudio,
        # which runs only with the graphical session.
        AUDIO_SINK="alsasink device=plughw:0,0"
        # On a monitor the sound goes with the picture: HDMI audio, the SiI9022A's I2S input.
        AUDIO_SINK_MONITOR="alsasink device=plughw:0,2"
        M4_FIRMWARE=mpy_m4.elf       # machine.I2C and machine.ADC; mpy_m4 starts it when needed
        # Real-time priority, low among real-time tasks (the kernel's interrupt threads run at 50):
        # busy Linux programs no longer delay the GUI and the requests to the Cortex-M4 (measured
        # with two busy processes: the slowest 1% of requests from 8 ms to about 1.5 ms). The kernel
        # keeps 5% of every second for normal tasks (sched_rt_runtime_us), the second core stays
        # free for Linux, and the programs MicroPython starts (linuxproc) run at normal priority.
        REALTIME=10
        HOSTNAME_DEFAULT=mp157-mpy ;;
    *) die "not an STM32MP135F-DK or STM32MP157C-DK2" ;;
esac
HOSTNAME_WANTED=${MPY_HOSTNAME-$HOSTNAME_DEFAULT}
say "board $BOARD"
files="micropython mpy-repl-hub mpy-screenshot mpy_launcher.py mpy_tap.py mpy-usb-composite.sh mpy-usb-composite.service
    mpy-repl.service usbotg.default journald-volatile.conf edit.py vi.py $DTB_NAME $M4_FIRMWARE"
for file in $files; do
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

install -d "$PREFIX/bin" "$PREFIX/lib" "$PREFIX/fs/lib" "$PREFIX/fs/bin"
put "$SOURCE/micropython" "$PREFIX/bin/micropython" 0755
put "$SOURCE/mpy-repl-hub" "$PREFIX/bin/mpy-repl-hub" 0755
put "$SOURCE/mpy-screenshot" "$PREFIX/bin/mpy-screenshot" 0755
# The games frontend and its libretro cores, when deploy.sh brought them.
if [ -f "$SOURCE/mpy-retro" ]; then
    put "$SOURCE/mpy-retro" "$PREFIX/bin/mpy-retro" 0755
    install -d "$PREFIX/retro/cores" "$PREFIX/fs/games"
    for core in "$SOURCE"/retro-cores/*_libretro.so; do
        [ -f "$core" ] && put "$core" "$PREFIX/retro/cores/$(basename "$core")" 0644
    done
fi
put "$SOURCE/mpy-usb-composite.sh" "$PREFIX/bin/mpy-usb-composite" 0755
put "$SOURCE/mpy_launcher.py" "$PREFIX/lib/mpy_launcher.py" 0644
put "$SOURCE/mpy_tap.py" "$PREFIX/lib/mpy_tap.py" 0644
put "$SOURCE/edit.py" "$PREFIX/fs/bin/edit.py" 0644
put "$SOURCE/vi.py" "$PREFIX/fs/bin/vi.py" 0644
if [ -n "$M4_FIRMWARE" ]; then
    put "$SOURCE/$M4_FIRMWARE" "/lib/firmware/$M4_FIRMWARE" 0644
    # A new firmware takes over at once if the M4 runs the old one; otherwise at first use.
    M4=/sys/class/remoteproc/remoteproc0
    case $changed in
        *"/lib/firmware/$M4_FIRMWARE"*)
            if [ "$(cat $M4/state)" = running ] && [ "$(cat $M4/firmware)" = "$M4_FIRMWARE" ]; then
                echo stop > $M4/state
                echo start > $M4/state
                say "Cortex-M4 restarted with the new $M4_FIRMWARE"
            fi ;;
    esac
fi
# The shell is frozen in the firmware now: old copies in fs/ would only confuse.
for old in shell_linux.py ansiterm.py ansifont.py; do
    [ -f "$PREFIX/fs/$old" ] && rm -f "$PREFIX/fs/$old" && say "removed the old $PREFIX/fs/$old"
done
if [ "$AUTOSTART" = home ] && [ ! -e "$PREFIX/fs/main.py" ]; then
    printf '# The shell and the app menu at power-on (mpy_home, frozen in the firmware).\nimport mpy_home\n\nmpy_home.main()\n' > "$PREFIX/fs/main.py"
    changed="$changed $PREFIX/fs/main.py"
    say "installed $PREFIX/fs/main.py: shell and app menu at power-on"
fi
put "$SOURCE/mpy-usb-composite.service" "$UNITS/mpy-usb-composite.service" 0644
put "$SOURCE/mpy-repl.service" "$UNITS/mpy-repl.service" 0644
# The board's own settings of the service, in a drop-in.
install -d "$UNITS/mpy-repl.service.d"
if [ "$ROTATION" != 0 ] || [ -n "$AUDIO_SINK" ] || [ -n "$AUDIO_SINK_MONITOR" ] || [ -n "$REALTIME" ]; then
    {
        printf '[Service]\n'
        [ "$ROTATION" != 0 ] && printf 'Environment=MPY_DISPLAY_ROTATION=%s\n' "$ROTATION"
        [ -n "$AUDIO_SINK" ] && printf 'Environment="MPY_AUDIO_SINK=%s"\n' "$AUDIO_SINK"
        [ -n "$AUDIO_SINK_MONITOR" ] && printf 'Environment="MPY_AUDIO_SINK_MONITOR=%s"\n' "$AUDIO_SINK_MONITOR"
        [ -n "$REALTIME" ] && printf 'CPUSchedulingPolicy=fifo\nCPUSchedulingPriority=%s\n' "$REALTIME"
        :  # the last test may be false: the block still succeeds
    } > "$SOURCE/board.conf"
    put "$SOURCE/board.conf" "$UNITS/mpy-repl.service.d/board.conf" 0644
elif [ -e "$UNITS/mpy-repl.service.d/board.conf" ]; then
    rm -f "$UNITS/mpy-repl.service.d/board.conf"
    changed="$changed board.conf"
fi
# ST's /etc/default/usbotg (when the image has one) is kept once, for uninstall.sh.
if [ -e /etc/default/usbotg ] && [ ! -e /etc/default/usbotg.st-original ] \
    && ! cmp -s "$SOURCE/usbotg.default" /etc/default/usbotg; then
    cp -p /etc/default/usbotg /etc/default/usbotg.st-original
    say "kept the original /etc/default/usbotg as /etc/default/usbotg.st-original"
fi
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
if [ -n "$DTB_NAME" ]; then
    DTB=/boot/$DTB_NAME
    if [ ! -e "$DTB.st-original" ]; then
        cp -p "$DTB" "$DTB.st-original"
        say "kept the original DTB as $DTB.st-original"
    fi
    before=$(sha256sum "$DTB" | cut -d' ' -f1)
    put "$SOURCE/$DTB_NAME" "$DTB" 0644
    [ "$(sha256sum "$DTB" | cut -d' ' -f1)" = "$before" ] || reboot_needed=1
fi

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
