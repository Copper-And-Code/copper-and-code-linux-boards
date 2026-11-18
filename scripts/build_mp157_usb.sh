#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the STM32MP157C-DK2 DTB for the composite USB gadget (TX FIFOs of the OTG controller).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK_DIR=${WORK_DIR:-$REPO/work}
SDK_RELEASE=openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10
SDK_DIR=${SDK_DIR:-$WORK_DIR/sdk/$SDK_RELEASE}
TOOLS=$SDK_DIR/sysroots/x86_64-ostl_sdk-linux/usr/bin
BASE=$SDK_DIR/sysroots/cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi/boot/stm32mp157c-dk2.dtb
BASE_SHA256=e6e7e256e353df650b77dd99fc377be6ce786ed4708b04acc134177733dba343
NODE=/soc/bus@5c007000/usb-otg@49000000

[[ -x $TOOLS/dtc && -x $TOOLS/fdtoverlay && -x $TOOLS/fdtget ]] || {
    echo "error: OpenSTLinux SDK device-tree tools not found in $SDK_DIR" >&2
    exit 1
}
[[ -f $BASE ]] || { echo "error: base DTB not found: $BASE" >&2; exit 1; }
echo "$BASE_SHA256  $BASE" | sha256sum -c -

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$TOOLS/dtc" -I dts -O dtb -o "$TMP/composite.dtbo" "$REPO/linux/stm32mp157c-dk2-usb-composite.dtso"
REV=$(git -C "$REPO" describe --always --dirty 2>/dev/null || echo unknown)
OUT=$REPO/dist/stm32mp157c-dk2-usb-composite-$REV.dtb
mkdir -p "$REPO/dist"
"$TOOLS/fdtoverlay" -i "$BASE" -o "$OUT" "$TMP/composite.dtbo"
FIFO=$("$TOOLS/fdtget" -t x "$OUT" "$NODE" g-tx-fifo-size)
[[ $FIFO == "80 80 40 10 10 10 10 10" ]] || {
    echo "error: unexpected TX FIFO layout: $FIFO" >&2
    exit 1
}
(cd "$REPO/dist" && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256")
echo "$OUT"
