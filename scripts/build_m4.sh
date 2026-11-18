#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the Cortex-M4 firmware of the STM32MP157C-DK2 (machine.I2C and machine.ADC) into dist/.
# Needs arm-none-eabi-gcc (Debian/Ubuntu: gcc-arm-none-eabi); fetches STM32CubeMP1 into work/.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
CUBE=${CUBE:-$REPO/work/STM32CubeMP1}
CUBE_TAG=1.7.0

command -v arm-none-eabi-gcc >/dev/null || { echo "error: arm-none-eabi-gcc not found" >&2; exit 1; }
if [[ ! -d $CUBE ]]; then
    git clone --depth 1 --branch "$CUBE_TAG" https://github.com/STMicroelectronics/STM32CubeMP1.git "$CUBE"
fi
[[ $(git -C "$CUBE" describe --tags 2>/dev/null) == "$CUBE_TAG" ]] || echo "warning: $CUBE is not STM32CubeMP1 $CUBE_TAG" >&2

make -C "$REPO/mcu/stm32mp157c-dk2" CUBE="$CUBE" -j"$(nproc)" >/dev/null
REV=$(git -C "$REPO" describe --always --dirty 2>/dev/null || echo unknown)
OUT=$REPO/dist/mpy_m4-$REV.elf
mkdir -p "$REPO/dist"
cp "$REPO/mcu/stm32mp157c-dk2/build/mpy_m4/mpy_m4.elf" "$OUT"
(cd "$REPO/dist" && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256")
arm-none-eabi-size "$OUT" | tail -1
echo "$OUT"
