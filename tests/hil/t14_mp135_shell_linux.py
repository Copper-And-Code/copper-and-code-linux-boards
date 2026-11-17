# SPDX-License-Identifier: MIT
# Run after installing examples/shell-linux on STM32MP135F-DK.

import os

import shell_linux as shell


def execute(args):
    sink = shell.Capture()
    assert shell.run_stage(args, None, False, sink)
    return sink.take().replace(b"\r\n", b"\n")


assert execute(["/bin/echo", "fallback"]) == b"fallback\n"
assert execute(["linux", "echo", "explicit"]) == b"explicit\n"
assert execute(["/usr/bin/printf", "%s", "one two"]) == b"one two"
assert execute(["/bin/sh", "-c", "exit 9"]) == b""
assert shell.find_program("edit") == "/usr/local/mpy/fs/bin/edit.py"
assert shell.find_program("vi") == "/usr/local/mpy/fs/bin/vi.py"
assert shell.find_linux_program("sh") in ("/bin/sh", "/usr/bin/sh")
assert shell.find_linux_program("not-a-real-mp135-command") is None

shell.run_line("/usr/bin/seq 1 5 | grep 3 > /usr/local/mpy/fs/shell-linux-test.txt")
with open("/usr/local/mpy/fs/shell-linux-test.txt") as source:
    assert source.read().strip() == "3"
os.remove("/usr/local/mpy/fs/shell-linux-test.txt")

print("MP135_SHELL_LINUX_TEST_PASS")
