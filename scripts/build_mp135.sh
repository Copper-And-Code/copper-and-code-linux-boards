#!/usr/bin/env bash
# Build MicroPython for STM32MP135F-DK, with or without LVGL.
#
#   scripts/build_mp135.sh [minimal|lvgl]
#
# Environment:
#   WORK_DIR  source checkouts and SDK (default: work/ in this repo)
#   SDK_DIR   installed OpenSTLinux SDK (default: $WORK_DIR/sdk/openstlinux-<release>)
#   JOBS      parallel jobs (default: nproc)
set -euo pipefail

MPY_URL=https://github.com/micropython/micropython.git
MPY_REF=v1.29.0
MPY_SHA=0fd6c573ea815774668bbb16b8e197c8822368b2
LVMP_URL=https://github.com/PyDevices/lvgl-micropython.git
LVMP_SHA=58d18ce5b48cd7acce609317991627a046f998f8
BINDINGS_URL=https://github.com/PyDevices/lvgl-bindings.git
BINDINGS_SHA=7740c2fae0daa10992f5f8fe54acf670a70e2d3b
SDK_RELEASE=openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10
SDK_VERSION=5.0.17-$SDK_RELEASE

log() { printf '\n== %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

MODE=${1:-lvgl}
[[ $MODE == minimal || $MODE == lvgl ]] || die "usage: $0 [minimal|lvgl]"
REPO=$(cd "$(dirname "$0")/.." && pwd)
VARIANT_DIR=$REPO/boards/unix/STM32MP135F_DK
[[ -f $VARIANT_DIR/mpconfigvariant.mk ]] || die "missing variant $VARIANT_DIR"
WORK_DIR=$(mkdir -p "${WORK_DIR:-$REPO/work}" && cd "${WORK_DIR:-$REPO/work}" && pwd)
SDK_DIR=${SDK_DIR:-$WORK_DIR/sdk/$SDK_RELEASE}
JOBS=${JOBS:-$(nproc)}
[[ $(uname -m) == x86_64 ]] || die "this SDK requires an x86-64 host"

checkout() {
    local url=$1 ref=$2 sha=$3 dir=$4 head
    if [[ ! -d $dir/.git ]]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$url"
    fi
    if ! git -C "$dir" cat-file -e "$sha^{commit}" 2>/dev/null; then
        git -C "$dir" fetch -q --depth 1 origin "$ref"
    fi
    git -C "$dir" checkout -q "$sha"
    head=$(git -C "$dir" rev-parse HEAD)
    [[ $head == "$sha" ]] || die "$dir is at ${head:0:12}, expected ${sha:0:12}"
    [[ -z $(git -C "$dir" status --porcelain --untracked-files=no) ]] || die "$dir has local changes"
}

log "sources in $WORK_DIR"
checkout "$MPY_URL" "refs/tags/$MPY_REF" "$MPY_SHA" "$WORK_DIR/micropython"
MPY=$WORK_DIR/micropython
make -C "$MPY/ports/unix" submodules
make -C "$MPY/mpy-cross" -j"$JOBS"

MAKE_ARGS=(
    VARIANT_DIR="$VARIANT_DIR"
    BUILD="build-STM32MP135F_DK-$MODE"
    MICROPY_MANIFEST_BOARDS_DIR="$REPO"
)
if [[ $MODE == minimal ]]; then
    MAKE_ARGS+=(FROZEN_MANIFEST="$MPY/ports/unix/variants/standard/manifest.py")
else
    checkout "$LVMP_URL" "$LVMP_SHA" "$LVMP_SHA" "$WORK_DIR/lvgl-micropython"
    checkout "$BINDINGS_URL" "$BINDINGS_SHA" "$BINDINGS_SHA" "$WORK_DIR/lvgl-bindings"
    git -C "$WORK_DIR/lvgl-bindings" submodule update --init --depth 1 lvgl
    MAKE_ARGS+=(
        MICROPY_MANIFEST_LVMP_DIR="$WORK_DIR/lvgl-micropython"
        BINDINGS_DIR="$WORK_DIR/lvgl-bindings"
    )
fi

log "OpenSTLinux SDK"
ENV_SETUP=$SDK_DIR/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
[[ -f $ENV_SETUP ]] || die "no SDK environment in $SDK_DIR; install the Developer Package SDK"
SDK_DIR=$(cd "$SDK_DIR" && pwd)
# shellcheck disable=SC1091
source "$SDK_DIR/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi"
[[ $OECORE_SDK_VERSION == "$SDK_VERSION" ]] || die "unexpected SDK $OECORE_SDK_VERSION"
[[ $CROSS_COMPILE == arm-ostl-linux-gnueabi- ]] || die "unexpected cross prefix $CROSS_COMPILE"
MAKE_ARGS+=(
    CC="$CC"
    CROSS_COMPILE="$CROSS_COMPILE"
    STRIP="${CROSS_COMPILE}strip"
    SIZE="${CROSS_COMPILE}size"
    READELF="${CROSS_COMPILE}readelf"
)

log "MicroPython $MODE"
make -C "$MPY/ports/unix" "${MAKE_ARGS[@]}" -j"$JOBS"
BINARY=$MPY/ports/unix/build-STM32MP135F_DK-$MODE/micropython
file "$BINARY"
${CROSS_COMPILE}size "$BINARY"
${CROSS_COMPILE}readelf -d "$BINARY"

REV=$(git -C "$REPO" describe --always --dirty 2>/dev/null || echo unknown)
OUT=micropython-STM32MP135F_DK-$MODE-$REV
mkdir -p "$REPO/dist"
cp "$BINARY" "$REPO/dist/$OUT"
(cd "$REPO/dist" && sha256sum "$OUT" > "$OUT.sha256")
log "done"
echo "$REPO/dist/$OUT"
