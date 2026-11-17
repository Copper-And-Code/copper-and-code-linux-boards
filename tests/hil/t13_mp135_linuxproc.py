# SPDX-License-Identifier: MIT
# Exercise Linux process execution on STM32MP135F-DK.

import os

import linuxproc


def run(argv, rows=24, cols=80):
    chunks = []

    def output(data):
        chunks.append(data)

    status = linuxproc.run(argv, output, rows=rows, cols=cols, input_fd=-1)
    return status, b"".join(chunks).replace(b"\r\n", b"\n")


status, output = run(["/bin/echo", "hello Linux"])
assert status == 0
assert output == b"hello Linux\n"

status, output = run(["/usr/bin/printf", "%s", "spaces ; $ and 'quotes'"])
assert status == 0
assert output == b"spaces ; $ and 'quotes'"

status, output = run(["/bin/sh", "-c", "printf error-output >&2; exit 7"])
assert status == 7
assert output == b"error-output"

status, output = run(["/usr/bin/stty", "size"], rows=17, cols=60)
assert status == 0
assert output == b"17 60\n"

status, output = run(["/usr/bin/seq", "1", "2000"])
assert status == 0
assert output.startswith(b"1\n2\n")
assert output.endswith(b"1999\n2000\n")
assert len(output) > 6000

path = "/usr/local/mpy/fs/linuxproc-input.bin"
total = 256 * 1024
with open(path, "wb") as target:
    for _ in range(total // 4096):
        target.write(b"x" * 4095 + b"\n")
received = []


def count_output(data):
    received.append(len(data))


with open(path, "rb") as source:
    status = linuxproc.run(
        ["/bin/sh", "-c", "stty -echo; exec head -c %d" % total],
        count_output,
        input_fd=source.fileno(),
    )
os.remove(path)
assert status == 0, (status, sum(received))
assert sum(received) >= total, (status, sum(received))


class StopOutput(Exception):
    pass


def stop_output(_data):
    raise StopOutput


try:
    linuxproc.run(["/bin/sh", "-c", "printf begin; sleep 30 & wait"], stop_output, input_fd=-1)
except StopOutput:
    pass
else:
    raise AssertionError("output exception did not propagate")

print("MP135_LINUXPROC_TEST_PASS")
