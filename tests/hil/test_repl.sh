#!/usr/bin/env bash
set -euo pipefail

P=${P:-/dev/ttyACM2}
TCP=${TCP:-socket://192.168.7.1:8266}
MPREMOTE=${MPREMOTE:-mpremote}
if [[ -z ${PYTHON:-} ]]; then
    if command -v python3 >/dev/null 2>&1; then
        PYTHON=python3
    else
        PYTHON=python
    fi
fi
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if command -v cygpath >/dev/null 2>&1; then
    HERE=$(cygpath -m "$HERE")
fi
export P MPREMOTE HERE

cleanup() {
    "$MPREMOTE" connect "$P" fs rm :main.py >/dev/null 2>&1 || true
}
trap cleanup EXIT

printf 'Serial target: %s\n' "$P"
out=$("$MPREMOTE" connect "$P" exec "import sys; print(sys.platform)")
printf '%s\n' "$out"
[[ $out == *linux* ]]

"$MPREMOTE" connect "$P" fs cp "$HERE/hello.py" :hello.py
out=$("$MPREMOTE" connect "$P" fs ls)
printf '%s\n' "$out"
[[ $out == *hello.py* ]]
out=$("$MPREMOTE" connect "$P" exec "import hello")
printf '%s\n' "$out"
[[ $out == *"Hello from STM32MP135F-DK"* ]]

"$PYTHON" - <<'PY'
import os
import time
import serial

port = serial.Serial(os.environ["P"], 115200, timeout=0.1)

# Output left by the previous mpremote session (a trailing ">>> ") would satisfy the waits below
# before the board answers: let it arrive, then discard it.
time.sleep(0.3)
port.reset_input_buffer()

def read_until(marker, timeout=10):
    data = bytearray()
    deadline = time.monotonic() + timeout
    while marker not in data and time.monotonic() < deadline:
        data.extend(port.read(port.in_waiting or 1))
    if marker not in data:
        raise RuntimeError(f"missing {marker!r}: {bytes(data)!r}")
    return bytes(data)

try:
    port.write(b"\x02")
    friendly = read_until(b">>>")
    if b"MicroPython" not in friendly:
        raise RuntimeError(f"invalid friendly REPL banner: {friendly!r}")
    port.write(b"\x01")
    read_until(b"raw REPL; CTRL-B to exit\r\n>")
finally:
    port.close()
print("PASS: friendly/raw REPL mode switching")
PY

"$PYTHON" - <<'PY'
import os
import subprocess
import time
import serial

proc = subprocess.Popen(
    [os.environ["MPREMOTE"], "connect", os.environ["P"], "run", os.path.join(os.environ["HERE"], "loop_forever.py")],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
)
try:
    time.sleep(2)
    if proc.poll() is not None:
        out, _ = proc.communicate()
        raise RuntimeError(f"mpremote run exited early: {out}")
    with serial.Serial(os.environ["P"], 115200, timeout=1) as port:
        port.write(b"\x03")
        port.flush()
    out, _ = proc.communicate(timeout=10)
finally:
    if proc.poll() is None:
        proc.kill()
        proc.wait()
print(out)
if "KeyboardInterrupt" not in out:
    raise SystemExit("Ctrl-C did not interrupt the raw-paste program")
print("PASS: Ctrl-C interrupts mpremote raw-paste execution")
PY

"$MPREMOTE" connect "$P" soft-reset
"$MPREMOTE" connect "$P" fs cp "$HERE/crash_main.py" :main.py
"$PYTHON" - <<'PY'
import os
import time
import serial

port = serial.Serial(os.environ["P"], 115200, timeout=0.1)

# Output left by the previous mpremote session (a trailing ">>> ") would satisfy the waits below
# before the board answers: let it arrive, then discard it.
time.sleep(0.3)
port.reset_input_buffer()
data = bytearray()
deadline = time.monotonic() + 15
try:
    port.write(b"\x04")
    markers = (b"raw REPL; CTRL-B to exit\r\n>", b">>> ")
    while not any(marker in data for marker in markers) and time.monotonic() < deadline:
        data.extend(port.read(port.in_waiting or 1))
finally:
    port.close()
print(data.decode(errors="replace"))
if b"intentional main.py crash" not in data or not any(marker in data for marker in markers):
    raise SystemExit("main.py crash did not return to a REPL")
print("PASS: main.py crash leaves the REPL available")
PY

"$MPREMOTE" connect "$P" fs rm :main.py
trap - EXIT
"$MPREMOTE" connect "$P" soft-reset

if [[ -n $TCP ]]; then
    printf 'TCP target: %s\n' "$TCP"
    out=$("$MPREMOTE" connect "$TCP" exec "import sys; print(sys.platform)")
    printf '%s\n' "$out"
    [[ $out == *linux* ]]
    "$MPREMOTE" connect "$TCP" fs ls
    "$MPREMOTE" connect "$TCP" soft-reset
fi

printf 'PASS: REPL HIL tests completed\n'
