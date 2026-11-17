# SPDX-License-Identifier: MIT
# The TCP REPL through mpy-repl-hub, run on the PC:
#
#   python3 tests/hil/test_tcp_hub.py [/dev/ttyACM2] [192.168.7.1:8266]
#
# Checks that Ctrl-C over TCP interrupts running code, that serial and TCP reach the same
# MicroPython process (so the display is shared, not fought over), and that a new TCP
# connection replaces the previous one.

import os
import shutil
import socket
import subprocess
import sys
import time

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM2"
HOST, TCP_PORT = (sys.argv[2] if len(sys.argv) > 2 else "192.168.7.1:8266").rsplit(":", 1)
MPREMOTE = os.environ.get("MPREMOTE") or shutil.which("mpremote") or "mpremote"


def connect():
    sock = socket.create_connection((HOST, int(TCP_PORT)), timeout=5)
    sock.settimeout(0.2)
    return sock


def read_until(sock, marker, timeout=10):
    data = b""
    deadline = time.monotonic() + timeout
    while marker not in data and time.monotonic() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            break
        data += chunk
    if marker not in data:
        raise AssertionError("missing %r in %r" % (marker, data[-300:]))
    return data


def mpremote(target, code):
    out = subprocess.run([MPREMOTE, "connect", target, "exec", code], capture_output=True, text=True, timeout=60)
    if out.returncode:
        raise AssertionError("mpremote %s failed: %s%s" % (target, out.stdout, out.stderr))
    return out.stdout.strip()


# Ctrl-C over TCP stops a running loop and returns to the friendly REPL.
sock = connect()
sock.sendall(b"\x03\x03\x02")
read_until(sock, b">>> ")
time.sleep(0.3)
sock.sendall(b"import time\r")
read_until(sock, b">>> ")
sock.sendall(b"while True: time.sleep_ms(1)\r\r")
time.sleep(1.0)
sock.sendall(b"\x03")
data = read_until(sock, b"KeyboardInterrupt")
if b">>> " not in data[data.index(b"KeyboardInterrupt"):]:
    read_until(sock, b">>> ", timeout=5)
print("PASS: Ctrl-C over TCP interrupts running code")

# A second connection replaces the first.
other = connect()
other.sendall(b"\x03\x02")
read_until(other, b">>> ")
try:
    closed = sock.recv(1) == b""
except (socket.timeout, OSError):
    closed = False
assert closed, "the first TCP connection was not closed"
sock.close()
other.close()
print("PASS: a new TCP connection replaces the previous one")

# Serial and TCP reach one process, which owns the display once.
tcp = "socket://%s:%s" % (HOST, TCP_PORT)
serial_pid = mpremote(PORT, "print(open('/proc/self/stat').read().split()[0])")
tcp_pid = mpremote(tcp, "print(open('/proc/self/stat').read().split()[0])")
assert serial_pid == tcp_pid, (serial_pid, tcp_pid)
print("PASS: serial and TCP share MicroPython process", serial_pid)

shown = mpremote(tcp, "import display_driver; print(display_driver.WIDTH, display_driver.running())")
assert shown == "480 True", shown
shown = mpremote(PORT, "import display_driver; print(display_driver.WIDTH, display_driver.running())")
assert shown == "480 True", shown
print("PASS: display_driver imported from TCP and serial in the same process")

print("PASS: TCP hub HIL tests completed")
