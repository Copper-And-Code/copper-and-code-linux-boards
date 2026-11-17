# STM32MP135F-DK installation and recovery

This guide installs the MicroPython/LVGL binary, the USB NCM+CDC-ACM gadget and the MCU-style REPL on
OpenSTLinux `6.6-yocto-scarthgap-mpu-v26.06.10`. The instructions are specific to the validated
STM32MP135F-DK/MB1635 configuration. Do not apply the DTB to another board or another OpenSTLinux
release.

The ST-LINK virtual COM port remains the Linux console. The MicroPython REPL uses the separate USB-C
gadget CDC-ACM port or TCP over USB networking.

## Build on the development host

Install the matching OpenSTLinux Developer Package SDK, then run:

```sh
cd /opt/devel/lvgl/mpy-lvgl-stm32/mpy-lvgl-boards
export WORK_DIR=/opt/devel/lvgl/lvgl-mpy-mpu/work
export SDK_DIR=$WORK_DIR/sdk/openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10
scripts/build_mp135.sh lvgl
scripts/build_mp135_usb.sh
REV=$(git describe --always --dirty)
```

The required files are:

- `dist/micropython-STM32MP135F_DK-lvgl-$REV`
- `dist/stm32mp135f-dk-usb-composite-$REV.dtb`

Each has a SHA-256 file. `build_mp135_usb.sh` refuses an unexpected base DTB and checks the final TX
FIFO layout. Use `scripts/build_mp135.sh minimal` instead of `lvgl` only when LVGL is not wanted.

## Prepare OpenSTLinux

Run these commands on the board through the ST-LINK Linux console or SSH. Weston is intentionally
excluded from this project configuration:

```sh
systemctl disable --now weston-graphical-session.service weston-checkgpu.service seatd-weston.service
systemctl disable --now psplash-drm-start.service
systemctl disable st-hostname.service
hostnamectl set-hostname mp135-mpy
```

The original `st-hostname.service` must remain disabled because it otherwise appends a MAC suffix to
the hostname at every boot. `psplash-drm-start.service` must also remain disabled: it holds DRM
master and prevents the MicroPython `linuxfb` backend from opening the panel. A static OpenSTLinux
boot logo can remain on the panel; without Weston it is not an interactive application.

For key-only SSH access, install the selected public key in `/home/root/.ssh/authorized_keys` (root's
home is `/home/root`, not `/root`):

```sh
install -d -m 0700 /home/root/.ssh
chmod 0600 /home/root/.ssh/authorized_keys
```

Only after SSH with the key has been verified, disable password authentication by setting this in
`/etc/default/dropbear`:

```sh
DROPBEAR_EXTRA_ARGS=" -s"
```

Restart `dropbear.socket` after changing that file. The local root password does not need to be
changed. Ensure `/etc/hosts` maps `127.0.1.1` to `mp135-mpy`.

## Install MicroPython and the REPL services

Run these host commands from the repository root, with `BOARD=192.168.7.1` and the same `REV` used for
the build:

```sh
BOARD=192.168.7.1
ssh root@$BOARD 'install -d /usr/local/mpy/bin /usr/local/mpy/lib /usr/local/mpy/fs/lib'
scp dist/micropython-STM32MP135F_DK-lvgl-$REV root@$BOARD:/usr/local/mpy/bin/micropython.new
scp linux/mpy_launcher.py root@$BOARD:/usr/local/mpy/lib/mpy_launcher.py
scp linux/mpy-usb-composite.sh root@$BOARD:/usr/local/mpy/bin/mpy-usb-composite
scp linux/mpy-repl.service linux/mpy-repl-tcp.socket linux/mpy-repl-tcp@.service \
    root@$BOARD:/etc/systemd/system/
scp linux/usbotg.default root@$BOARD:/etc/default/usbotg
scp dist/stm32mp135f-dk-usb-composite-$REV.dtb root@$BOARD:/boot/stm32mp135f-dk.dtb.new
```

`/etc/default/usbotg` disables the original ST gadget startup so that it cannot race the composite
NCM+ACM service. The new service creates the gadget once at boot; do not manually unbind or rebind the
USB device controller while it is active.

Preserve the original DTB once, then activate the new files and services:

```sh
ssh root@$BOARD '
    if [ ! -e /boot/stm32mp135f-dk.dtb.st-original ]; then
        cp /boot/stm32mp135f-dk.dtb /boot/stm32mp135f-dk.dtb.st-original
    fi
    mv /boot/stm32mp135f-dk.dtb.new /boot/stm32mp135f-dk.dtb
    mv /usr/local/mpy/bin/micropython.new /usr/local/mpy/bin/micropython
    chmod 0755 /usr/local/mpy/bin/micropython /usr/local/mpy/bin/mpy-usb-composite
    systemctl daemon-reload
    systemctl enable mpy-usb-composite.service mpy-repl.service mpy-repl-tcp.socket
    reboot
'
```

A reboot is required. The expected DTB SHA-256 values for this exact release are:

```text
bf08a8e8aa6e0e2aacb1e37d76e1045c4702aff2239b8ff32234369b7235c294  original
1f8a0bdafc669ee05ca19c3fda920df197a1503ca4471b5d4a305bc99884d939  NCM+ACM
```

After boot, the board has `192.168.7.1/24` on `usb0`; the host address is assigned dynamically. All
three units should be active:

```sh
systemctl is-active mpy-usb-composite.service mpy-repl.service mpy-repl-tcp.socket
systemctl --failed
```

The REPL units intentionally use
`MICROPYPATH=.frozen:/usr/local/mpy/fs:/usr/local/mpy/fs/lib`. The `.frozen` entry is required for
`display_driver` and `touch_evdev`, which are compiled into the LVGL binary.

When upgrading an already running installation, copy the binary to `micropython.new`, rename it over
`micropython`, and then run `systemctl restart mpy-repl.service`. A soft reset uses `/proc/self/exe`;
without the service restart it can re-execute the old inode after an atomic binary replacement.

## Connect and test

Install `mpremote` and `pyserial` on the host. On Linux, identify the new gadget port rather than
reusing the ST-LINK console; in the validated setup it was `/dev/ttyACM2`.

```sh
P=/dev/ttyACM2
mpremote connect $P exec "import sys; print(sys.platform)"
mpremote connect $P fs ls
mpremote connect socket://192.168.7.1:8266 fs ls
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
it on Ethernet, Wi-Fi or an untrusted network without a separate security design. Use one REPL channel
to run an application: serial and accepted TCP connections are separate processes, and concurrent
access to LVGL/display/touch peripherals is not supported.

## Recovery

If the board does not boot or the USB gadget fails, use the ST-LINK Linux console. Restore the
original DTB and disable the new services:

```sh
cp /boot/stm32mp135f-dk.dtb.st-original /boot/stm32mp135f-dk.dtb
systemctl disable --now mpy-repl.service mpy-repl-tcp.socket mpy-usb-composite.service
rm -f /etc/default/usbotg
reboot
```

Removing `/etc/default/usbotg` allows the original ST gadget startup to use its defaults. To restore
the graphical demo as well, re-enable `psplash-drm-start.service`,
`weston-graphical-session.service`, `weston-checkgpu.service`, `seatd-weston.service` and, if
desired, `st-hostname.service`. The files under `/usr/local/mpy` can
remain installed while the services are disabled.

## Safe-mode validation status

B1/USER1 is `PA13`, active-low, device-tree key `BTN_1` (`0x101`). The launcher checks it through
`/dev/input/by-path/platform-gpio-keys-event` and skips `main.py` when pressed. During the Phase 4
physical checks, PA13 remained high and no key state was reported, so safe mode is implemented but
not yet physically verified. Do not describe this feature as validated until a `hi -> lo -> hi`
transition and the `Safe mode: main.py skipped` message have been observed.
