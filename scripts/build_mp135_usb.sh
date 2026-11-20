#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the STM32MP135F-DK DTB for composite USB gadgets and B1 safe mode, with the peripherals of
# the 40-pin GPIO expansion connector on for machine (linux/stm32mp135f-dk-header.dtso).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK_DIR=${WORK_DIR:-$REPO/work}
SDK_RELEASE=openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10
SDK_DIR=${SDK_DIR:-$WORK_DIR/sdk/$SDK_RELEASE}
TOOLS=$SDK_DIR/sysroots/x86_64-ostl_sdk-linux/usr/bin
BASE=$SDK_DIR/sysroots/cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi/boot/stm32mp135f-dk.dtb
BASE_SHA256=bf08a8e8aa6e0e2aacb1e37d76e1045c4702aff2239b8ff32234369b7235c294
NODE=/soc/bus@5c007000/usb@49000000

[[ -x $TOOLS/dtc && -x $TOOLS/fdtoverlay && -x $TOOLS/fdtget ]] || {
    echo "error: OpenSTLinux SDK device-tree tools not found in $SDK_DIR" >&2
    exit 1
}
[[ -f $BASE ]] || { echo "error: base DTB not found: $BASE" >&2; exit 1; }
echo "$BASE_SHA256  $BASE" | sha256sum -c -

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$TOOLS/dtc" -I dts -O dtb -o "$TMP/composite.dtbo" "$REPO/linux/stm32mp135f-dk-usb-composite.dtso"
"$TOOLS/dtc" -I dts -O dtb -o "$TMP/header.dtbo" "$REPO/linux/stm32mp135f-dk-header.dtso"
REV=$(git -C "$REPO" describe --always --dirty 2>/dev/null || echo unknown)
OUT=$REPO/dist/stm32mp135f-dk-usb-composite-$REV.dtb
mkdir -p "$REPO/dist"
"$TOOLS/fdtoverlay" -i "$BASE" -o "$OUT" "$TMP/composite.dtbo" "$TMP/header.dtbo"
FIFO=$("$TOOLS/fdtget" -t x "$OUT" "$NODE" g-tx-fifo-size)
[[ $FIFO == "80 80 40 10 10 10 10 10" ]] || {
    echo "error: unexpected TX FIFO layout: $FIFO" >&2
    exit 1
}
B1_GPIO=$("$TOOLS/fdtget" -t x "$OUT" /gpio-keys/button-user gpios)
[[ $B1_GPIO == "72 e 11" ]] || {
    echo "error: unexpected B1 GPIO specifier: $B1_GPIO" >&2
    exit 1
}
BLUE_STATUS=$("$TOOLS/fdtget" -t s "$OUT" /leds/led-blue status)
[[ $BLUE_STATUS == "disabled" ]] || {
    echo "error: blue heartbeat LED must be disabled for B1" >&2
    exit 1
}
SPIDEV=$("$TOOLS/fdtget" -t s "$OUT" /soc/bus@5c007000/spi@4c003000/spidev@0 compatible)
UART8=$("$TOOLS/fdtget" -t s "$OUT" /soc/serial@40019000 status)
[[ $SPIDEV == rohm,dh2228fv && $UART8 == okay ]] || {
    echo "error: the expansion connector's SPI5 and UART8 are not on" >&2
    exit 1
}
(cd "$REPO/dist" && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256")
echo "$OUT"
