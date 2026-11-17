# SPDX-License-Identifier: MIT
# linuxproc.Process on STM32MP135F-DK: a Linux player driven from MicroPython without blocking.
# Needs the three test tones in /usr/local/mpy/fs/music (tests/hil/make_test_music.sh). Audio goes
# to a fake sink, so no sound card is needed.
#
#   mpremote connect /dev/ttyACM2 run tests/hil/t21_mp135_process.py

import time

import linuxproc

MUSIC = "/usr/local/mpy/fs/music/"
PLAYER = ["gst-play-1.0", "--audiosink=fakesink sync=true", MUSIC + "tone1.mp3", MUSIC + "tone2.mp3"]


def collect(process, seconds):
    data = b""
    end = time.ticks_add(time.ticks_ms(), int(seconds * 1000))
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        data += process.read()
        time.sleep_ms(20)
    return data.replace(b"\r", b"\n")


def positions(data):
    found = []
    for line in data.split(b"\n"):
        line = line.strip()
        if b" / " in line and line[:2] == b"0:":
            found.append(line.split(b" / ")[0])
    return found


player = linuxproc.Process(PLAYER)
assert player.pid > 0 and player.poll() is None
data = collect(player, 3.5)  # GStreamer prerolls for about a second
assert b"Now playing " + MUSIC.encode() + b"tone1.mp3" in data, data[-200:]
playing = positions(data)
assert playing and playing[-1] >= b"0:00:01.0", playing[-3:]
print("MP135_PROCESS playing", playing[-1])

assert player.write(b" ") == 1  # pause
time.sleep_ms(300)
paused_at = positions(collect(player, 0.3))
time.sleep(1)
after = positions(collect(player, 0.5))
assert not after or after[-1] == (paused_at or playing)[-1], (paused_at[-1:], after[-1:])
print("MP135_PROCESS paused at", (paused_at or playing)[-1])

# One key per read: gst-play ignores a read that holds two keys, as an unknown sequence.
player.write(b" ")  # resume
time.sleep_ms(200)
player.write(b">")  # next track
data = collect(player, 2)
assert b"tone2.mp3" in data, data[-200:]
assert b">" not in data.replace(b"/ ", b""), "the terminal echoed the keys"
print("MP135_PROCESS next track")

player.write(b"q")
for _ in range(100):
    if player.poll() is not None:
        break
    player.read()
    time.sleep_ms(20)
assert player.poll() == 0, player.poll()
player.close()
print("MP135_PROCESS quit, status 0")

sleeper = linuxproc.Process(["/bin/sh", "-c", "sleep 30"])
time.sleep_ms(200)
sleeper.kill()
for _ in range(50):
    if sleeper.poll() is not None:
        break
    time.sleep_ms(20)
assert sleeper.poll() == -15, sleeper.poll()
sleeper.close()

# close() stops a program that is still running, and the object can be dropped.
linuxproc.Process(["/bin/sh", "-c", "sleep 30"]).close()
try:
    sleeper.read()
except OSError:
    pass
else:
    raise AssertionError("read() after close()")
print("MP135_PROCESS_TEST_PASS")
