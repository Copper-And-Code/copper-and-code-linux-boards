#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the STM32MP157C-DK2 DTB for the composite USB gadget (TX FIFOs of the OTG controller).
#
# The base is the SDK's "m4-examples" device tree: the same board, with the Arduino connector's
# ADC, DAC, I2C5, SPI5, UART7, TIM1 and TIM2 (and DMA2) given to the Cortex-M4, which serves
# machine.I2C, machine.ADC and the rest. Linux keeps display, touch, Wi-Fi, audio and USB.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK_DIR=${WORK_DIR:-$REPO/work}
SDK_RELEASE=openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10
SDK_DIR=${SDK_DIR:-$WORK_DIR/sdk/$SDK_RELEASE}
TOOLS=$SDK_DIR/sysroots/x86_64-ostl_sdk-linux/usr/bin
BASE=$SDK_DIR/sysroots/cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi/boot/stm32mp157c-dk2-m4-examples.dtb
BASE_SHA256=5d0188f1f8a92f145a2c1ae85f4ce0aa72e50a8dfe4f565e2c2fc66cb7c81bf0
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
