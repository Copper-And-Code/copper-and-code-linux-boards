#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Kernel modules that OpenSTLinux's kernel lacks, for gamepads: xpad (X-input over USB, the
# default of many controllers, 8BitDo's included), hid-nintendo (Switch mode), uhid and hidp
# (Bluetooth controllers, through BlueZ).
#
#   scripts/build_kmods.sh [BOARD_CONFIG]
#
# The kernel is ST's at the tag of the image (6.6.129: v6.6-stm32mp-r3.1), configured with the
# board's own configuration (BOARD_CONFIG, default work/kernel/board.config, saved from the board's
# /proc/config.gz) plus these modules, and built whole once with the SDK's compiler: the kernel
# checks the versions of the symbols a module uses (MODVERSIONS), and the same sources,
# configuration and compiler give the same ones. Outputs in dist/kmods/; scripts/deploy.sh
# installs them when present.
#
# Environment: WORK_DIR, SDK_DIR as for scripts/build_mp135.sh; JOBS (default: nproc).
set -euo pipefail

KERNEL_URL=https://github.com/STMicroelectronics/linux.git
KERNEL_TAG=v6.6-stm32mp-r3.1
KERNEL_RELEASE=6.6.129
SDK_RELEASE=openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10
# The modules, as .ko paths in the tree
MODULES=(
    drivers/input/joystick/xpad.ko
    drivers/hid/hid-nintendo.ko
    drivers/hid/uhid.ko
    net/bluetooth/hidp/hidp.ko
)

log() { printf '\n== %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK_DIR=${WORK_DIR:-$REPO/work}
SDK_DIR=${SDK_DIR:-$WORK_DIR/sdk/$SDK_RELEASE}
JOBS=${JOBS:-$(nproc)}
CONFIG=${1:-$WORK_DIR/kernel/board.config}
[[ -f $CONFIG ]] || die "no board configuration: ssh root@BOARD 'zcat /proc/config.gz' > $CONFIG"
ENV_SETUP=$SDK_DIR/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
[[ -f $ENV_SETUP ]] || die "no SDK environment in $SDK_DIR"
unset LD_LIBRARY_PATH
set +u
# shellcheck disable=SC1090
source "$ENV_SETUP"
set -u
unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS  # the kernel has its own; the SDK's would break it

SRC=$WORK_DIR/kernel/linux
if [[ ! -d $SRC ]]; then
    log "kernel $KERNEL_TAG"
    git clone -q --depth 1 --branch "$KERNEL_TAG" "$KERNEL_URL" "$SRC"
fi
K=(make -C "$SRC" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" LOCALVERSION= -j"$JOBS")

log "configuration"
cp "$CONFIG" "$SRC/.config"
"$SRC/scripts/config" --file "$SRC/.config" \
    --disable LOCALVERSION_AUTO \
    --enable INPUT_JOYSTICK --module JOYSTICK_XPAD --enable JOYSTICK_XPAD_FF --enable JOYSTICK_XPAD_LEDS \
    --module HID_NINTENDO --enable NINTENDO_FF \
    --module UHID --module BT_HIDP
"${K[@]}" olddefconfig >/dev/null
release=$("${K[@]}" -s kernelrelease)
[[ $release == "$KERNEL_RELEASE" ]] || die "kernel release $release, the board runs $KERNEL_RELEASE"

log "kernel and modules (once: the symbol versions come from the whole build)"
"${K[@]}" zImage modules >/dev/null

OUT=$REPO/dist/kmods
rm -rf "$OUT"
mkdir -p "$OUT"
for module in "${MODULES[@]}"; do
    [[ -f $SRC/$module ]] || die "not built: $module"
    cp "$SRC/$module" "$OUT/"
    "${CROSS_COMPILE}strip" --strip-debug "$OUT/$(basename "$module")"
done
(cd "$OUT" && sha256sum ./*.ko > SHA256SUMS)
log "done"
ls -l "$OUT"
