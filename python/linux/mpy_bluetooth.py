# SPDX-License-Identifier: MIT
# Bluetooth controllers (gamepads, keyboards, mice) through BlueZ: bluetoothctl runs in the
# background on a pseudo-terminal (linuxproc.Process), so the screen never waits. A controller,
# once paired and trusted, connects again by itself when it is switched on.
#
#   import mpy_bluetooth as bt
#   job = bt.scan(30)          # look for devices in pairing mode for 30 seconds
#   job.poll()                 # None while it runs, then True
#   bt.devices()               # [(address, name, paired), ...], known and found
#   job = bt.pair(address)     # pair, trust and connect; job.poll(): None, then True or False
#   job.text                   # what bluetoothctl said
#   bt.forget(address)
#
# The gamepad's own pairing mode first: for an 8BitDo SN30 Pro+, Start with B held (Android,
# D-input) or Y held (Switch), then the pair button. The kernel needs hidp and uhid
# (scripts/build_kmods.sh) for a Bluetooth controller to become an input device.
#
# An 8BitDo SN30 Pro+ in Switch mode, once paired, connects again by itself when switched on: it
# then answers no command, and our hid-nintendo (scripts/kmods/) reads its simple reports.

import time

import linuxproc

PAIR_SECONDS = 30


class Job:
    """bluetoothctl with commands, in the background. poll() returns None while it runs."""

    def __init__(self, commands, until, seconds, then=None):
        self.text = ""
        self.result = None
        self._until = until  # (texts that all mean success, texts meaning failure)
        self._commands = commands
        self._then = then  # (texts to wait for, more commands): sent once one of them appears
        self._started = time.ticks_ms()
        self._deadline = time.ticks_add(self._started, seconds * 1000)
        self._process = linuxproc.Process(["bluetoothctl"], rows=50, cols=200)

    def poll(self):
        if self.result is not None:
            return self.result
        data = self._process.read()
        if data:
            self.text = (self.text + data.decode("utf-8", "replace"))[-6000:]
        # The commands once bluetoothctl reaches bluetoothd (its prompt), not before: an agent
        # registered too early is refused.
        if self._commands and ("[bluetooth]" in self.text or time.ticks_diff(time.ticks_ms(), self._started) > 3000):
            self._process.write(("\n".join(self._commands) + "\n").encode())
            self._commands = None
        if self._commands is None and self._then and any(mark in self.text.upper() for mark in self._then[0]):
            self._process.write(("\n".join(self._then[1]) + "\n").encode())
            self._then = None
        success, failures = self._until
        if success and all(text in self.text for text in success):
            self._finish(True)
        elif any(failure in self.text for failure in failures):
            self._finish(False)
        elif time.ticks_diff(self._deadline, time.ticks_ms()) <= 0:
            self._finish(success is None)  # a scan ends with its time; a pairing fails
        return self.result

    def _finish(self, result):
        self.result = result
        try:
            self._process.write(b"quit\n")
            time.sleep_ms(100)
        except OSError:
            pass
        self._process.kill()


def scan(seconds=30):
    """Looks for devices in pairing mode. A classic controller (a Switch Pro, an 8BitDo) takes
    a while: inquiry and name lookup alternate with Bluetooth LE."""
    return Job(["power on", "scan on"], (None, ()), seconds)


def pair(address):
    """Pairs, trusts (it will connect again by itself) and connects. The search runs again until
    the controller shows up (it must still be in pairing mode), then the pairing starts."""
    return Job(
        ["power on", "agent NoInputNoOutput", "default-agent", "pairable on", "scan on",
         "info " + address],
        (("Paired: yes", "Connected: yes"),
         ("Failed to pair", "Failed to connect", "AuthenticationFailed", "ConnectionAttemptFailed")),
        PAIR_SECONDS,
        # Known again: "info" describes it ("Device ADDRESS (public)"), or the search finds it.
        then=(("DEVICE %s (" % address.upper(), "NEW] DEVICE " + address.upper()),
              ["scan off", "pair " + address, "trust " + address, "connect " + address]),
    )


def connect(address):
    return Job(["power on", "connect " + address], (("Connection successful",), ("Failed to connect",)), 15)


def forget(address):
    return Job(["remove " + address], (("Device has been removed",), ("not available",)), 10)


def _lines(command):
    process = linuxproc.Process(["bluetoothctl"] + command, rows=50, cols=200)
    text = b""
    deadline = time.ticks_add(time.ticks_ms(), 3000)
    while process.poll() is None and time.ticks_diff(deadline, time.ticks_ms()) > 0:
        text += process.read() or b""
        time.sleep_ms(20)
    text += process.read() or b""
    process.kill()
    return text.decode("utf-8", "replace").replace("\r", "").split("\n")


def devices():
    """(address, name, paired) of the devices BlueZ knows, paired first."""
    paired = set()
    for line in _lines(["devices", "Paired"]):
        parts = line.split(" ", 2)
        if len(parts) >= 2 and parts[0] == "Device":
            paired.add(parts[1])
    found = []
    for line in _lines(["devices"]):
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[0] == "Device" and parts[2] != parts[1].replace(":", "-"):
            found.append((parts[1], parts[2].strip(), parts[1] in paired))
    found.sort(key=lambda d: (not d[2], d[1]))
    return found
