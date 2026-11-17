#!/usr/bin/env bash
# Build the STM32MP135F-DK DTB with FIFO space for composite USB gadgets.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK_DIR=${WORK_DIR:-$REPO/.work/mp135}
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
REV=$(git -C "$REPO" describe --always --dirty 2>/dev/null || echo unknown)
OUT=$REPO/dist/stm32mp135f-dk-usb-composite-$REV.dtb
mkdir -p "$REPO/dist"
"$TOOLS/fdtoverlay" -i "$BASE" -o "$OUT" "$TMP/composite.dtbo"
FIFO=$("$TOOLS/fdtget" -t x "$OUT" "$NODE" g-tx-fifo-size)
[[ $FIFO == "80 80 40 10 10 10 10 10" ]] || {
    echo "error: unexpected TX FIFO layout: $FIFO" >&2
    exit 1
}
(cd "$REPO/dist" && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256")
echo "$OUT"
