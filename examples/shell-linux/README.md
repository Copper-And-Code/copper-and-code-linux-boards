# shell-linux for STM32MP135F-DK

`shell-linux` is a board-specific derivative of `examples/shell`. The original example is unchanged.
It combines the MicroPython shell, the `edit` and `vi` editors, and execution of Linux programs on a
pseudo-terminal (PTY). The panel displays an ANSI terminal while keyboard input comes from the
MicroPython serial or TCP REPL connection, or from a USB keyboard plugged into the board (layout in
the Settings app).

On the 480x272 panel the terminal has 60 columns and 17 rows. Linux child processes receive the same
terminal size, so commands such as `stty size` report `17 60`.

## Requirements

- The STM32MP135F-DK MicroPython/LVGL installation described in
  `docs/stm32mp135f-dk-install.md`.
- An LVGL binary built from the board variant with the `linuxproc` user C module and `os.dupterm`
  support. Both are enabled by `boards/unix/STM32MP135F_DK`.
- Weston and the DRM boot splash disabled, as required by the display installation.
- A serial terminal connected to the MicroPython CDC-ACM gadget port, not the ST-LINK Linux console.

## Install

Run these commands from the repository root. Adjust `BOARD` and `P` for the local setup:

```sh
BOARD=192.168.7.1
P=/dev/ttyACM2
ssh root@$BOARD 'install -d /usr/local/mpy/fs/bin'
scp examples/shell-linux/shell_linux.py examples/shell-linux/ansiterm.py \
    examples/shell-linux/ansifont.py examples/shell-linux/OFL.txt \
    root@$BOARD:/usr/local/mpy/fs/
scp examples/shell-linux/edit.py examples/shell-linux/vi.py \
    root@$BOARD:/usr/local/mpy/fs/bin/
```

Connect to the MicroPython REPL:

```sh
mpremote connect $P
```

Then start the application:

```py
import shell_linux
shell_linux.main()
```

Use `exit` or Ctrl-D to return to the MicroPython REPL, which stays on the display (a keyboard
plugged into the board goes on typing there); Ctrl-D in the REPL starts the shell again. When a
tool enters the raw REPL the terminal leaves the display. `linux/install.sh` starts the shell at power-on through
`mpy_home` (a `main.py` it writes when there is none), with an app menu on a tap; the shell,
`ansiterm` and `ansifont` are frozen in the firmware and the editors installed in `fs/bin`. Ctrl-A on
an empty line hands the board over to the raw REPL, which is what LVGL MPY Studio and mpremote ask
for when they connect.

## Use

```text
help
ls -l
edit notes.txt
vi notes.txt
uname -a
/bin/echo hello from Linux
linux echo forced Linux lookup
/bin/sh -c 'printf "shell status demo\n"; exit 0'
/bin/cat
```

The last command reads keyboard input until Ctrl-C. Ctrl-C is delivered to the Linux process through
the PTY and returns to `shell-linux`; it does not stop MicroPython. In `edit`, Ctrl-S saves and Ctrl-Q
quits. In `vi`, use the normal insert, escape and `:wq` commands.

Command lookup uses this order:

1. A `shell-linux` built-in command.
2. A Python program in `/usr/local/mpy/fs/bin/<name>.py`.
3. An executable in the Linux `PATH`.

Use `linux <name>` or an explicit path such as `/bin/cat` to bypass the first two levels. This is
useful when a built-in and a Linux program have the same name. Arguments are passed directly as an
argument vector, without an intermediate Linux shell; use `/bin/sh -c` explicitly when shell syntax
inside the Linux command is wanted.

## Current limits

- This is not a POSIX shell. It supports its own quoting, escapes, wildcards, output redirection and
  buffered pipelines, but not the full Linux shell grammar or job control.
- Pipeline stages run sequentially and intermediate output is held in the MicroPython heap. A Linux
  stage reads from the serial keyboard, not from the preceding in-memory pipeline buffer. Use Linux
  built-in equivalents or `/bin/sh -c` for a native Linux pipeline.
- Linux exit statuses are returned by `linuxproc.run()`, but the interactive shell does not yet expose
  `$?` or use a child exit status for pipeline control.
- Python programs run inside the same MicroPython process and heap as the shell. Linux programs run
  in a new session with their own controlling PTY.
- The service currently runs as root, so Linux commands inherit root privileges. The application can
  modify the whole Linux filesystem; use filesystem and removal commands accordingly.
- There is no on-screen keyboard. The touchscreen is not a terminal input device in this example.
- The terminal draws ASCII only: accented letters typed on a keyboard reach the programs but show
  as `?`.
- The ANSI terminal is optimized for text. Its alternate-screen support redraws saved cells instead
  of allocating a second full-screen framebuffer, which is important for `edit` and `vi` on this
  small MicroPython heap.

The font in `ansifont.py` is distributed under the SIL Open Font License; see `OFL.txt`.

## Validate on the board

With the application files installed and no interactive shell already running, execute:

```sh
mpremote connect $P run tests/hil/t13_mp135_linuxproc.py
mpremote connect $P run tests/hil/t14_mp135_shell_linux.py
```

Expected output:

```text
MP135_LINUXPROC_TEST_PASS
MP135_SHELL_LINUX_TEST_PASS
```

The PTY test covers output and errors, argument preservation, terminal dimensions, sustained output,
256 KiB of input with backpressure, callback exceptions and process-group cleanup. The shell test
covers Linux fallback, explicit Linux lookup, Python editor discovery, pipelines and redirection.
Interactive editor rendering and keyboard behavior additionally require a check on the physical
panel and serial terminal.
