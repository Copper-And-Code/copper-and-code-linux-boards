#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Three 10-second test tones as MP3 with ID3 titles, copied to the board's music folder.
#   tests/hil/make_test_music.sh [BOARD]
set -euo pipefail
BOARD=${1:-192.168.7.1}
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
for i in 1 2 3; do
    ffmpeg -loglevel error -y -f lavfi -i "sine=frequency=$((220 * i)):duration=10" \
        -metadata title="Test tone $i" -metadata artist="lvgl-mpy-linux" -b:a 128k "$DIR/tone$i.mp3"
done
ssh ${MPY_SSH_ALIAS:+-o HostKeyAlias=$MPY_SSH_ALIAS} "root@$BOARD" 'mkdir -p /usr/local/mpy/fs/music'
scp -q ${MPY_SSH_ALIAS:+-o HostKeyAlias=$MPY_SSH_ALIAS} "$DIR"/*.mp3 "root@$BOARD:/usr/local/mpy/fs/music/"
