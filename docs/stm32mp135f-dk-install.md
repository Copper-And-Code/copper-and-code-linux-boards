# STM32MP135F-DK installation and recovery

This guide installs the MicroPython/LVGL binary, the USB NCM+CDC-ACM gadget, the REPL hub and the
MCU-style REPL on
OpenSTLinux `6.6-yocto-scarthgap-mpu-v26.06.10`. The instructions are specific to the validated
STM32MP135F-DK/MB1635 configuration. Do not apply the DTB to another board or another OpenSTLinux
release.

The ST-LINK virtual COM port remains the Linux console. The MicroPython REPL uses the separate USB-C
gadget CDC-ACM port or TCP over USB networking.

## Build on the development host

Install the matching OpenSTLinux Developer Package SDK, then run:

```sh
cd /opt/devel/lvgl/lvgl-mpy-linux
scripts/build_mp135.sh lvgl
scripts/build_mp135_usb.sh
REV=$(git describe --always --dirty)
```

The required files are:

- `dist/micropython-STM32MP135F_DK-lvgl-$REV`
- `dist/stm32mp135f-dk-usb-composite-$REV.dtb`

Each has a SHA-256 file. `build_mp135_usb.sh` refuses an unexpected base DTB and checks the final TX
FIFO layout. Use `scripts/build_mp135.sh minimal` instead of `lvgl` only when LVGL is not wanted.

## Prepare SSH access

The installation runs over SSH with a key. Set it up once through the ST-LINK Linux console
(`picocom -b 115200 /dev/ttyACM0` on Linux, the ST-LINK COM port on Windows). Root's home is
`/home/root`, not `/root`:

```sh
install -d -m 0700 /home/root/.ssh
# put the public key in /home/root/.ssh/authorized_keys, then:
chmod 0600 /home/root/.ssh/authorized_keys
```

Only after SSH with the key has been verified, disable password authentication by setting this in
`/etc/default/dropbear`:

```sh
DROPBEAR_EXTRA_ARGS=" -s"
```

Restart `dropbear.socket` after changing that file. The installation scripts do not change SSH.
Before the first installation the board is reachable over the original ST USB network gadget at
`192.168.7.1`.

## Install

From the repository root, after the build:

```sh
scripts/deploy.sh            # BOARD defaults to 192.168.7.1
```

`deploy.sh` copies the newest MicroPython binary, `mpy-repl-hub` and DTB from `dist/` (select
others with `MICROPYTHON=`, `HUB=`, `DTB=`), the launcher, the units and the scripts of `linux/` to
`/tmp/mpy-install` on the board, and runs `linux/install.sh` there. `install.sh`:

- checks that the board is an STM32MP135F-DK and that every file matches its SHA-256;
- copies only what differs, through a temporary name, so a running program is never overwritten;
- keeps the original DTB once as `/boot/stm32mp135f-dk.dtb.st-original`;
- installs `/etc/default/usbotg`, which disables the original ST gadget startup so that it cannot
  race the composite NCM+ACM gadget;
- removes the units of earlier installations (one TCP REPL process per connection);
- disables Weston, `psplash-drm-start.service` (it holds DRM master) and `st-hostname.service` (it
  appends a MAC suffix to the hostname), and sets the hostname to `mp135-mpy` (`MPY_HOSTNAME=` to
  change it, empty to leave it);
- enables `mpy-usb-composite.service` and `mpy-repl.service`;
- says whether a reboot is needed (new DTB or no gadget yet), restarts the REPL when only programs
  changed, or reports that nothing changed.

The uninstaller is installed as `/usr/local/mpy/bin/mpy-uninstall`. Run `deploy.sh` again after
every build: an unchanged installation reports `nothing changed`.

On Windows, `scripts/deploy.sh` runs from Git Bash with the OpenSSH client of Windows. Without
Bash, copy the files listed at the top of `linux/install.sh` to a folder on the board (for example
with `scp` from PowerShell) and run `sh install.sh` there.

The expected DTB SHA-256 values for this exact release are:

```text
bf08a8e8aa6e0e2aacb1e37d76e1045c4702aff2239b8ff32234369b7235c294  original
0c0e3d98a814a9f394056e74c914e00f0ce96b926108d60d55be9e71ee9da2f9  NCM+ACM, B1 on PA14
```

After boot, the board has `192.168.7.1/24` on `usb0`; the host address is assigned dynamically. Both
units should be active and the panel shows "MicroPython ready" while there is no `main.py`:

```sh
systemctl is-active mpy-usb-composite.service mpy-repl.service
systemctl --failed
```

The REPL service uses `MICROPYPATH=.frozen:/usr/local/mpy/fs:/usr/local/mpy/fs/lib`. The `.frozen`
entry is required for `display_driver`, `touch_evdev` and `mpy_ready`, which are compiled into the
binary. A soft reset re-executes `/proc/self/exe`; `install.sh` restarts the service after replacing
the binary, so no process keeps running the old one.

## Connect and test

Install `mpremote` and `pyserial` on the host. On Linux, identify the new gadget port rather than
reusing the ST-LINK console; in the validated setup it was `/dev/ttyACM2`.

```sh
P=/dev/ttyACM2
mpremote connect $P exec "import sys; print(sys.platform)"
mpremote connect $P fs ls
mpremote connect socket://192.168.7.1:8266 fs ls
```

The name that does not change is in `/dev/serial/by-id/`: `usb-Copper-And-Code_MicroPython_LVGL_*`.

On a PC with ModemManager (most Linux desktops), install the board's udev rule once. Without it,
ModemManager takes the board's serial port for a modem whenever the board starts and sends it AT
commands; while it probes, the port echoes the board's output back as input, and the shell on the
screen runs that text as commands (`AT: command not found`) and may leave:

```sh
sudo cp linux/host/70-mpy-lvgl-board.rules /etc/udev/rules.d/
sudo udevadm control --reload
```

Run the full HIL test only after backing up any existing `:main.py`, because the test temporarily
replaces it and removes it during cleanup:

```sh
P=$P TCP=socket://192.168.7.1:8266 tests/hil/test_repl.sh
```

Run the physical display checks and the 10-second full-screen benchmark from this repository root:

```sh
mpremote connect $P run tests/hil/t11_mp135_display.py
mpremote connect $P run tests/hil/t12_mp135_performance.py
```

To upload and run the interactive Studio example, use the matching Studio executable and board
profile `stm32mp135f_dk_480x272`:

```sh
/opt/devel/lvgl/lvgl-mpy-studio/.venv/bin/lvgl-mpy-studio --language en device run \
    examples/stm32mp135f-dk/project.json --port $P
```

The panel shows a dark page with a `TOUCH ME` button. A successful press changes the label to
`Touch works!` and prints `STM32MP135_TOUCH_OK` on the REPL console. The same project can be opened
in the Studio GUI; connect to the CDC-ACM gadget port, not the ST-LINK Linux console.

On Windows, identify the new CDC-ACM COM port in Device Manager and install the Python packages with
`py -m pip install mpremote pyserial`. The individual commands accept `P=COM<n>` and the same socket
address. The Bash HIL script can run from Git Bash with native Windows Python tools on `PATH`; it uses
`cygpath` for fixture paths. The Windows checkpoint is still pending.

The TCP REPL has no authentication or encryption and listens only on `192.168.7.1:8266`. Do not expose
it on Ethernet, Wi-Fi or an untrusted network without a separate security design. Serial and TCP reach
the same MicroPython process; as with dupterm on the MCU ports, two clients typing at once see each
other's output.

## Install the `shell-linux` application

The optional `examples/shell-linux` application adds the ANSI shell, `edit`, `vi` and Linux program
execution on a PTY. Install its runtime modules in `/usr/local/mpy/fs` and the two Python editors in
`/usr/local/mpy/fs/bin`:

```sh
ssh root@$BOARD 'install -d /usr/local/mpy/fs/bin'
scp examples/shell-linux/shell_linux.py examples/shell-linux/ansiterm.py \
    examples/shell-linux/ansifont.py examples/shell-linux/OFL.txt \
    root@$BOARD:/usr/local/mpy/fs/
scp examples/shell-linux/edit.py examples/shell-linux/vi.py \
    root@$BOARD:/usr/local/mpy/fs/bin/
```

Start it from the MicroPython REPL with:

```py
import shell_linux
shell_linux.main()
```

The application is not added to `main.py` by default. See `examples/shell-linux/README.md` for command
lookup, the `linux` override, Ctrl-C behavior, tests and current pipeline/exit-status limitations.
Linux commands inherit the REPL service's root privileges.

## Working from afar: screenshots and taps

Two tools let a board be checked and driven without looking at it or touching it:

```sh
scripts/screen.sh screen.png          # what the panel shows now, turned as the user sees it
scripts/tap.sh 400 300                # a tap at screen coordinates (x, y)
```

`mpy-screenshot` (`tools/mpy-screenshot/`, installed in `/usr/local/mpy/bin`) reads the
framebuffer the display controller shows through DRM and writes a PNG; it does not disturb the
program that draws it or the REPL. `mpy_tap.py` writes a press and a release into the
touchscreen's own event device, turned back from screen to panel coordinates, so `display_driver`
reads them as a finger. Both need root. With two boards at the same address, set `MPY_SSH_ALIAS`
as for `scripts/deploy.sh`.

## Uninstall and recovery

To return to the original OpenSTLinux configuration (original DTB and USB gadget, Weston and the
demo), run on the board and reboot:

```sh
/usr/local/mpy/bin/mpy-uninstall            # --purge also removes /usr/local/mpy
reboot
```

The board is then reachable again over the original ST USB network gadget at `192.168.7.1`; running
`scripts/deploy.sh` installs everything again.

If the board does not boot or the USB gadget fails, use the ST-LINK Linux console and do the same by
hand:

```sh
cp /boot/stm32mp135f-dk.dtb.st-original /boot/stm32mp135f-dk.dtb
systemctl disable --now mpy-repl.service mpy-usb-composite.service
rm -f /etc/default/usbotg
reboot
```

## Safe-mode validation

B1/USER1 is `PA14`, active-low with a pull-up, device-tree key `BTN_1` (`0x101`). B2/USER2 is
`PA13` and is exposed separately as `BTN_2` (`0x102`). The DTB overlay disables the blue heartbeat
LED because it shares PA14 with B1. The launcher checks B1 through
`/dev/input/by-path/platform-gpio-keys-event` and skips `main.py` when pressed.

Physical validation observed three complete `BTN_1` press/release cycles. A normal service restart
executed the temporary `main.py` marker; holding B1 during another service restart printed
`Safe mode: main.py skipped` and did not execute the marker. Releasing B1 returned
`mpyrepl.user_button_pressed()` to false. B2's mechanical switch on the validation board was damaged by a knock and is to be
replaced; B2 was not tested.

To enter safe mode, hold B1 while restarting `mpy-repl.service`, then release it after the message.
Do not hold B1 during a full board reset unless the U-Boot USB programming mode is intended.
