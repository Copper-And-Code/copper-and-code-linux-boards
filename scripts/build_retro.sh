#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the games frontend (mpy-retro) and its libretro cores for the STM32MP1 boards: Snes9x2005,
# fetched, and the C64 core of cores/c64.
#
#   scripts/build_retro.sh
#
# The cores are fetched at a fixed commit and built with the board's SDK; their sources stay in
# work/ and under their own licenses (Snes9x: non-commercial terms, see its copyright file), never
# in this repository. Outputs in dist/retro/: mpy-retro and cores/NAME_libretro.so, which
# scripts/deploy.sh installs when present.
#
# Environment: WORK_DIR, SDK_DIR as for scripts/build_mp135.sh; JOBS (default: nproc).
set -euo pipefail

# name, repository, commit
CORES=(
    "snes9x2005 https://github.com/libretro/snes9x2005.git a79dfe9047e7fec58808aefe48ad2bf499c7af11"
)
SDK_RELEASE=openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10

log() { printf '\n== %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK_DIR=${WORK_DIR:-$REPO/work}
SDK_DIR=${SDK_DIR:-$WORK_DIR/sdk/$SDK_RELEASE}
JOBS=${JOBS:-$(nproc)}
ENV_SETUP=$SDK_DIR/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
[[ -f $ENV_SETUP ]] || die "no SDK environment in $SDK_DIR; install the Developer Package SDK"
unset LD_LIBRARY_PATH
set +u
# shellcheck disable=SC1090
source "$ENV_SETUP"
set -u

OUT=$REPO/dist/retro
mkdir -p "$OUT/cores" "$WORK_DIR/retro"
LIBRETRO_H=

for entry in "${CORES[@]}"; do
    read -r name url commit <<<"$entry"
    log "core $name"
    src=$WORK_DIR/retro/$name
    [[ -d $src/.git ]] || git clone -q "$url" "$src"
    git -C "$src" fetch -q origin "$commit" 2>/dev/null || true
    git -C "$src" checkout -q "$commit"
    make -C "$src" clean >/dev/null 2>&1 || true
    make -C "$src" platform=unix CC="$CC" CXX="$CXX" -j"$JOBS" >/dev/null
    cp "$src/${name}_libretro.so" "$OUT/cores/"
    [[ -z $LIBRETRO_H ]] && LIBRETRO_H=$(dirname "$(find "$src" -name libretro.h -path '*libretro-common/include/*' | head -1)")
done

log "core c64 (cores/c64, in this repository)"
# shellcheck disable=SC2086
$CC $CFLAGS -O2 -fPIC -shared -std=gnu11 -Wall -Wextra -Werror -I"$LIBRETRO_H" -I"$REPO/cores/c64" \
    -o "$OUT/cores/c64_libretro.so" "$REPO/cores/c64/libretro.c" "$REPO"/cores/c64/core/{c64,c1541,crt,sid,vic_render}.c \
    $LDFLAGS -Wl,--no-undefined -lm

log "frontend"
# shellcheck disable=SC2086
$CC $CFLAGS -O2 -Wall -Wextra -Werror -I"$LIBRETRO_H" -I"$SDKTARGETSYSROOT/usr/include/libdrm" \
    -o "$OUT/mpy-retro" "$REPO/tools/mpy-retro/mpy-retro.c" $LDFLAGS -ldrm -lasound -ldl -lpthread -lm
(cd "$OUT" && sha256sum mpy-retro cores/*.so > SHA256SUMS)
log "done"
ls -l "$OUT" "$OUT/cores"
