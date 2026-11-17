# STM32MP135F-DK MicroPython/LVGL target

This is a personal project by Nunzio Raciti and is not affiliated with or endorsed by
STMicroelectronics.

Target: STM32MP135F-DK, MB1635, running MicroPython with LVGL and controlled by
LVGL MPY Studio. The ST-LINK virtual COM port remains the Linux console and is never reused as
the MicroPython REPL.

## Current status

- Phase 0 reconnaissance: complete; see `docs/ricognizione.md`.
- Phase 1 flash and initial inventory: complete.
- Phase 2 system preparation: complete.
- Phase 3 Cortex-A7 MicroPython/LVGL cross-build: complete.
- Phase 4 MCU-style REPL: Linux HIL tests, performance, stress and reboot validation complete.
- Phase 5 DRM display, Goodix touch and LVGL MPY Studio demo: complete, including rendering
  performance and repeated soft-reset resource checks.
- The physical B1 safe-mode check passes; the Windows HIL checkpoint remains open.
- Later phases: not started.

## Phase 1 verified configuration

The microSD was flashed through CN7/USB_OTG with STM32CubeProgrammer 2.23.0 and the default
OP-TEE layout `FlashLayout_sdcard_stm32mp135f-dk-optee.tsv`. STM32CubeProgrammer reported
`Flashing service completed successfully`. The board then booted from microSD and showed the
Weston/GTK demonstration on its panel.

The inventory was collected through the ST-LINK console on host `/dev/ttyACM0` at 115200 8N1.
The captured command output, with serial CR/LF line endings normalized to LF, is in
`docs/log/stm32mp135f-dk-phase1-inventory.txt`.

| Item | Verified value |
|---|---|
| Distribution | `5.0.17-openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10` |
| Kernel | Linux 6.6.129, ARMv7, SMP PREEMPT |
| Board-side Linux console | `/dev/ttySTM0`, also present in the kernel command line |
| Memory | 437 MiB total, 320 MiB available at inventory time; no swap |
| Root filesystem | `/dev/mmcblk0p10`, 3.8 GiB, 3.1 GiB available |
| `/usr/local` | `/dev/mmcblk0p11`, 11 GiB, 9.6 GiB available |
| Display device | DRM `/dev/dri/card0`; **no `/dev/fb0`** |
| Touch | `Goodix Capacitive TouchScreen`, currently `/dev/input/event1` |
| Other input | `gpio-keys`, currently `/dev/input/event0` |
| Backlight | `/sys/class/backlight/panel-backlight` |
| USB Type-C | `/sys/class/typec/port0` |
| USB gadget | Existing configfs gadget `/sys/kernel/config/usb_gadget/g1` |
| Network at inventory time | `end0`, `end1`, `wlan0` and `usb0` present but without an active IP link |

Consequences for subsequent phases:

- Phase 5 must implement a DRM dumb-buffer backend; fbdev cannot be assumed.
- Touch discovery must use the Goodix device name or stable by-path identity, not a hard-coded
  event number.
- Phase 2 must characterize the touch axes, backlight behavior and boot timing before changing
  services.
- The existing USB network gadget must be inspected and extended with ACM in Phase 4, not
  replaced.
- Weston and its graphical session are still enabled. No service, network setting, password or
  boot configuration was changed during Phase 1.

## Phase 2 configuration and installation notes

These are the approved board changes to preserve when creating the final user installation guide
and installer. They are not yet a complete installation procedure.

### Network and SSH

- CN7/USB_OTG provides the existing NCM interface `usb0`; no gadget replacement is required.
- The board has `192.168.7.1/24` and runs a DHCP server on this interface. The development host
  received `192.168.7.58/24`; this host address is dynamic and must not be hard-coded.
- SSH is provided by `dropbear.socket`, not OpenSSH. The key-only configuration was verified after
  a reboot; an attempt without the public key was rejected.
- Root's actual home is `/home/root`, not `/root`. The authorized key must therefore be installed
  as `/home/root/.ssh/authorized_keys`, with mode `0600` and parent directory mode `0700`.
- The selected host public key is `id_rsa.pub`, fingerprint
  `SHA256:Qak5m6jc+Adn/q9ouKuO/q9Yd2lnJzq4f0c9ocACwrU`.
- `/etc/default/dropbear` contains `DROPBEAR_EXTRA_ARGS=" -s"` to disable SSH password
  authentication. The local root password was not changed.
- `/etc/hosts` maps `127.0.1.1` to `mp135-mpy`.

### Services and hostname

The following units were explicitly approved and disabled:

```sh
systemctl disable --now weston-graphical-session.service weston-checkgpu.service seatd-weston.service
systemctl disable st-hostname.service
hostnamectl set-hostname mp135-mpy
```

`st-hostname.service` must remain disabled for this configuration: its original
`/usr/bin/st-hostname.sh` script appends the `end0` MAC suffix to a hostname on every boot. The
script itself was not modified. All other services, including networking, Dropbear and the USB
gadget, remain enabled.

After the first reboot, all three Weston units were disabled and inactive, no systemd units were
failed, and Nunzio confirmed a black panel with no ST demo. SSH remained available. A static
OpenSTLinux boot logo can remain visible on later boots; without Weston it has no touch handling and
is not an interactive application.

### Measurements and physical validation

| Item | Verified value |
|---|---|
| Original boot time | 2 min 10.670 s: kernel 1 min 17.307 s, userspace 53.363 s |
| Boot without Weston | 37.445 s: kernel 12.430 s, userspace 25.015 s |
| Second boot without Weston | 36.915 s: kernel 12.367 s, userspace 24.547 s |
| Memory without Weston | 437 MiB total, 352 MiB available at measurement time |
| DRM connector | DPI-1, connected, 480×272 at 59.99 Hz |
| Primary plane formats | Includes `RG16` (RGB565) and `XR24` (XRGB8888) |
| Goodix X ranges | `ABS_X` and `ABS_MT_POSITION_X`: 0–479 |
| Goodix Y ranges | `ABS_Y` and `ABS_MT_POSITION_Y`: 0–271 |
| Goodix multitouch | Five slots (`ABS_MT_SLOT` 0–4), `INPUT_PROP_DIRECT` |
| Backlight | Range 0–10; 0 visually off, 5 intermediate, 10 maximum |

The raw corner events are saved in `docs/log/stm32mp135f-dk-phase2-touch-corners.txt` and
`docs/log/stm32mp135f-dk-phase2-touch-bottom-corners.txt`. The bottom pair was repeated in strict
order after the lower contacts in the first sequence were reversed.

| Physical contact | Observed X,Y |
|---|---|
| Top-left | 52,45 |
| Top-right | 417,50 |
| Bottom-right, repeated check | 399,172 |
| Bottom-left, repeated check | 59,221 |

`ABS_X/Y` matched the corresponding multitouch coordinates. X increases to the right and Y increases
downward: no axis swap or inversion is required. These are observed finger-contact positions, not
new calibration limits; the driver retains its full 0–479 by 0–271 range.

## Phase 3 cross-build and on-board validation

The x86-64 Developer Package SDK archive
`SDK-x86_64-stm32mp1-openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10.tar.gz` was extracted and its
installer completed successfully. The installed environment reports GCC 13.4.0, `ARCH=arm`,
`CROSS_COMPILE=arm-ostl-linux-gnueabi-` and the expected `OECORE_SDK_VERSION`.

The SDK and board both provide libffi 3.4.6, so `MICROPY_PY_FFI=1` is retained. They also provide
libdrm 2.4.120 and SDL2 2.30.1, but the final MicroPython binaries do not link SDL. The SDK
environment does not export `SIZE`; the build script therefore sets `SIZE`, `STRIP` and `READELF`
explicitly with the cross-tool prefix.

Reproducible builds use `scripts/build_mp135.sh`. It builds `mpy-cross` for the host before loading
the SDK environment and pins MicroPython v1.29.0, lvgl-micropython `58d18ce` and lvgl-bindings
`7740c2f` (LVGL 9.5.0). The validated commands in this development workspace are:

```sh
cd /opt/devel/lvgl/lvgl-mpy-linux
scripts/build_mp135.sh minimal
scripts/build_mp135.sh lvgl
```

`WORK_DIR` defaults to `work/` inside this repository and can be set to another workspace; `SDK_DIR` defaults to the matching
SDK directory below it. Outputs and SHA-256 files are written to `dist/`.

| Build | text | data | bss | Total |
|---|---:|---:|---:|---:|
| Minimal | 401,870 | 1,144 | 2,060 | 405,074 bytes |
| LVGL | 1,134,629 | 2,396 | 2,152 | 1,139,177 bytes |

Both are stripped ARM EABI5 32-bit executables using `/lib/ld-linux-armhf.so.3`. The Phase 3 dynamic
`NEEDED` entries were `libm.so.6`, `libffi.so.8` and `libc.so.6`; Phase 5 adds `libdrm.so.2` to the
LVGL build for the DRM panel backend.

On the board, the LVGL binary reports MicroPython 1.29.0, `sys.platform=linux`, build
`STM32MP135F_DK` and LVGL 9.5.0. The existing PyDevices fake-display smoke test,
`tests/hil/t02_lvgl_smoke.py`, passes twice including after `lv.deinit()`; the second scene renders
successfully and the MicroPython heap returns to its initial value. Output is in
`docs/log/stm32mp135f-dk-phase3-smoke.txt`.

The fast official `basics` suite ran on the board: 545 tests performed, 544 passed and 28 skipped.
Three tests requiring the optional, non-frozen `unittest` package were explicitly excluded. The only
failure, `int_64_basics.py`, is a 32-bit reference mismatch: the board's 32-bit CPython raises
`OverflowError` while creating the expected output, whereas MicroPython completes the test. The full
output and diff are in `docs/log/stm32mp135f-dk-phase3-tests-basics.txt`.

PyDevices reports that this build does not include the optional `jpegio` module: JPEG images will not
draw, while PNG and LVGL BIN remain available. JPEG support was not required for this phase.

## Phase 4 MCU-style REPL

The ST-LINK virtual COM port remains the Linux console. MicroPython uses the CDC-ACM function added
to the existing USB gadget, preserving NCM networking, DHCP and SSH. On the validated host the gadget
is `/dev/ttyACM2`, but the number is dynamic; the ST-LINK console must not be selected. The board-side
device is `/dev/ttyGS0`. The REPL filesystem root is `/usr/local/mpy/fs`.

The composite gadget requires the TX FIFO layout supplied by
`linux/stm32mp135f-dk-usb-composite.dtso`. `scripts/build_mp135_usb.sh` verifies the exact original
DTB hash, applies the overlay and validates the resulting layout. Do not rebind the gadget while it is
active: install the DTB and reboot. The original and modified DTB SHA-256 values are respectively
`bf08a8e8aa6e0e2aacb1e37d76e1045c4702aff2239b8ff32234369b7235c294` and
`1f8a0bdafc669ee05ca19c3fda920df197a1503ca4471b5d4a305bc99884d939`.

The board-specific `mpyrepl` user C module calls the upstream friendly/raw REPL implementations,
executes `boot.py` and `main.py`, preserves the REPL mode and restarts in place with `execve` on soft
reset. A linker wrapper around `mp_hal_set_interrupt_char()` enables terminal `ISIG` only while code
runs. This is required for Ctrl-C in both normal raw REPL and raw-paste execution; no upstream
MicroPython source is modified.

| Requirement | Validated result |
|---|---|
| Friendly banner and `>>>` prompt | Pass |
| Ctrl-A / Ctrl-B raw/friendly switching | Pass |
| Ctrl-C during `mpremote run` raw-paste execution | Pass; `KeyboardInterrupt` and REPL available |
| Ctrl-D soft reset | Pass; raw mode restored and `main.py` executed |
| Filesystem `:` rooted at `/usr/local/mpy/fs` | Pass; copy, list, import and delete tested |
| `main.py` exception | Pass; traceback shown and REPL remains available |
| B1 safe mode skips `main.py` | **Pass**: three `BTN_1` cycles; safe-mode marker observed and temporary `main.py` skipped |
| TCP REPL on `192.168.7.1:8266` | Pass; command, filesystem and soft reset tested |

`tests/hil/test_repl.sh` automates the serial checks, the real `main.py` crash and TCP smoke tests. It
temporarily replaces `:main.py` and removes it on completion or failure; back up any user application
before running it. Example:

```sh
P=/dev/ttyACM2 TCP=socket://192.168.7.1:8266 tests/hil/test_repl.sh
```

Set `MPREMOTE` and `PYTHON` if they are not on `PATH`. Set `TCP=` to skip TCP. For Git Bash on Windows,
use `P=COM<n>`; fixture paths are converted with `cygpath`. The Windows checkpoint has not yet been
run.

Performance and stability measured on the board:

| Test | Result |
|---|---|
| Soft reset to raw prompt, 10 samples | min 0.060 s, mean 0.068 s, max 0.073 s |
| Raw-paste copy of 204,800 bytes | 11.778 s, 17.0 KiB/s, copied size verified |
| 50 copy + soft-reset cycles | No errors; one PID throughout |
| Process RSS during stress | 1,524–1,804 KiB; delta 280 KiB |
| `mpremote mount . exec ...` | Pass; host directory visible as `/remote` |
| Two complete reboots | NCM, SSH, ACM and TCP recovered; final boot had no failed units or FIFO/Oops errors |

A standalone `mpremote mount .` mounts the directory and then enters the interactive REPL, so it needs
a terminal; a non-interactive shell should append a command such as `exec` as shown above.

While code runs the CDC-ACM terminal has `ISIG` on for Ctrl-C only: Ctrl-Z and Ctrl-\ are
disabled, both by the service `stty` and by `mpyrepl`, so they cannot stop or kill the REPL process.
`StartLimitIntervalSec=0` keeps `Restart=always` working through a crash loop.

`mpy-repl.service` runs `mpy-repl-hub` (`tools/mpy-repl-hub/`): one MicroPython process on a
pseudo-terminal, shared by the CDC-ACM port and one TCP client on `192.168.7.1:8266`. The terminal's
line discipline turns Ctrl-C into SIGINT for both channels. Output goes to both clients, as with
MicroPython's dupterm on the MCU ports, and input from both reaches the REPL; a new TCP connection
replaces the previous one. A client that stops reading for 1 s has its output dropped until it reads
again, so a closed serial port never blocks the REPL. When MicroPython exits the hub starts it again.
The TCP listener has no authentication or encryption and is bound only to the USB gadget address.
Results: `docs/log/stm32mp135f-dk-repl-hub.txt`.

UM2993 maps B1/USER1 to PA14 and B2/USER2 to PA13. The original configuration exposed PA13 as
`BTN_1` and drove PA14 as the blue heartbeat LED, so B1 presses produced no input events. The DTB
overlay now disables that LED, exposes B1/PA14 as `BTN_1` (`0x101`) and preserves B2/PA13 as
`BTN_2` (`0x102`) through `/dev/input/by-path/platform-gpio-keys-event`. Three complete physical B1
press/release cycles were observed. Holding B1 during an `mpy-repl.service` restart printed
`Safe mode: main.py skipped` and prevented the temporary `main.py` marker from running; the normal
restart control executed it. B2 is physically broken on the validation board and was not tested.

Logs are saved as `docs/log/stm32mp135f-dk-phase4-hil.txt`,
`docs/log/stm32mp135f-dk-phase4-performance.txt`,
`docs/log/stm32mp135f-dk-phase4-stress.txt`,
`docs/log/stm32mp135f-dk-phase4-reboot.txt` and
`docs/log/stm32mp135f-dk-phase4-safe-mode.txt`. See
`docs/stm32mp135f-dk-install.md` for installation and recovery instructions.

## Phase 5 display, touch and Studio integration

The panel is driven directly through DRM/KMS on `/dev/dri/card0`; this board has no `/dev/fb0`.
The board-specific `linuxfb` user C module creates and maps a 480×272 RGB565 dumb buffer with a
1,024-byte stride, sets the DPI-1 CRTC and copies LVGL dirty rectangles into it. Weston and
`psplash-drm-start.service` must be disabled so that MicroPython can become DRM master. The LVGL
binary therefore has `libdrm.so.2` as an additional dynamic dependency.

`python/linux/display_driver.py` implements display-driver contract 1 with one 255 KiB full-screen
buffer in DIRECT mode. Its service thread calls `lv.timer_handler()` every 5 ms, while the LVGL
refresh timer is limited to 16 ms. The variant enables the MicroPython GIL
(`MICROPY_PY_THREAD_GIL := 1`): the unix port has none by default, and without it the service thread
and `main.py` or the REPL would call LVGL at the same time. With the GIL each LVGL call is atomic, as
with the scheduled callbacks of the MCU ports; the full-screen animation measured 57.09 FPS (58.14
without the GIL). LVGL event handlers run in the service thread, so Ctrl-C does not stop a handler
that never returns. `stop()` waits for the service thread to exit, preventing an
immediate `start()` from creating overlapping service threads. Backlight control uses
`/sys/class/backlight/panel-backlight` and defaults to 100%.

`python/linux/touch_evdev.py` discovers `Goodix Capacitive TouchScreen` by name in
`/proc/bus/input/devices`; the event number is not fixed. It polls the numeric file descriptor and
decodes the verified 16-byte ARM32 `input_event` records with `llHHi`. `ABS_MT_POSITION_X/Y`,
`ABS_X/Y`, tracking ID and `BTN_TOUCH` are handled. X increases rightward and Y downward, so no axis
swap or inversion is needed. The board's `FileIO` does not expose `read1()` and its `os` module does
not expose `read()`. After polling, each read therefore requests exactly one 16-byte event: the
kernel supplies a complete event without MicroPython waiting to fill a larger buffer. Input is
delivered one `SYN_REPORT` frame at a time and the LVGL `continue_reading` flag drains frames queued
in the driver or kernel. A quick press and release therefore remain two distinct LVGL states instead
of collapsing into a release with no click. `tests/host/test_touch_evdev.py` covers this quick-tap
regression, partial frames, queued motion and the restricted `FileIO` method surface;
`tests/hil/t15_mp135_touch.py` provides a visible click counter and callback timing on the panel.

Both Python modules are frozen by the board manifest. The serial and TCP REPL services set
`MICROPYPATH=.frozen:/usr/local/mpy/fs:/usr/local/mpy/fs/lib`; omitting `.frozen` makes
`display_driver` unavailable to applications. On soft reset, `mpyrepl` closes inherited descriptors
above stderr before `execve`, preventing DRM and input descriptors from accumulating across process
restarts.

Validation results on the physical panel:

| Test | Result |
|---|---|
| Red, green, blue and white bars, orientation and title | Visually confirmed |
| LVGL MPY Studio project upload and execution | `LVGL_MPY_STUDIO_APP_READY buffers=1 touch=True` |
| Studio button press changes the label and runs its handler | 24 `STM32MP135_TOUCH_OK` events after the final evdev correction; no errors |
| Quick-tap press/release delivery | 11 matched `DOWN`/`UP`/`CLICK` sequences; callback approximately 0.4–2.0 ms |
| Full-screen color animation | 582 frames in 10.011 s: **58.14 FPS**, 100% full-frame coverage |
| Process CPU during steady-state full-screen rendering | Approximately 10–12% of one Cortex-A7 CPU |
| Process memory | 2.6 MiB RSS, 4.3 MiB virtual size, two threads |
| Ctrl-C during an application | `KeyboardInterrupt` reached the application; REPL remained available |
| Three Ctrl-D/display reinitialization cycles | No extra threads or DRM/input descriptors |
| `tests/hil/t11_mp135_display.py` | `MP135_DISPLAY_TEST_PASS`, including `stop()`/`start()` |
| `tests/hil/t12_mp135_performance.py` | `MP135_PERF_TEST_PASS` |

The interactive example is `examples/stm32mp135f-dk/project.json`, using Studio board profile
`stm32mp135f_dk_480x272`. Installation, launch and recovery commands are in
`docs/stm32mp135f-dk-install.md`.

## `shell-linux` application

`examples/shell-linux` extends the board with a separate Linux-capable shell without modifying the
portable `examples/shell` application. It provides the MicroPython file and text commands, the `edit`
and `vi` editors, and Linux executable lookup. Built-ins take precedence over Python programs in
`/usr/local/mpy/fs/bin`, which in turn take precedence over executables in the Linux `PATH`; the
`linux` command forces Linux execution.

The board-specific `linuxproc` user C module starts each program with `fork`/`execvp` on a controlling
PTY. It forwards serial keyboard input and terminal output, sets the 60x17 panel dimensions, handles
non-blocking backpressure while continuing to drain output, and kills the whole process group if a
MicroPython callback raises. `MICROPY_PY_OS_DUPTERM` is enabled for the shared panel/serial ANSI
terminal. The terminal alternate screen saves character cells and redraws them on exit rather than
duplicating the 255 KiB framebuffer, allowing both editors to run within the MicroPython heap.

Installation, usage, limitations and HIL commands are documented in `examples/shell-linux/README.md`
and linked from `docs/stm32mp135f-dk-install.md`. Autostart is optional and is not installed by
default.

## Portable `machine` API decision

The project will expose one MicroPython-compatible public `machine` API across Linux-only MPU boards
and boards with a real-time MCU. User code should not change when the underlying hardware backend
changes.

- On STM32MP135F-DK and similar Linux-only boards, classes such as `Pin`, `I2C`, `SPI`, `PWM`, `ADC`
  and `Timer` will use kernel interfaces and drivers. Direct peripheral register access from Linux
  user space is not part of this design.
- On MPU boards with a programmable MCU, an equivalent backend may communicate with MCU firmware
  through remoteproc and RPMsg/OpenAMP. Native MicroPython MCU ports should retain their existing
  `machine` API wherever possible.
- Public method signatures, units, ranges, return values, exceptions and resource cleanup semantics
  should be shared across backends. Board-specific names, device discovery and capabilities must be
  kept out of application code.
- Unsupported peripherals and operations must be reported explicitly rather than emulated silently.
  Board capabilities must be discoverable and documented.
- API compatibility does not imply equivalent timing guarantees. Linux-only backends must not claim
  hard real-time behavior; future real-time control-loop offload requires a separate, explicitly
  defined API.
- The different backends should run the same hardware-in-the-loop contract tests where the board
  provides the corresponding peripheral.

STM32MP135F-DK has no programmable real-time MCU. Its initial implementation will therefore use the
Linux kernel backend. The first class is `machine.Pin` (`modules/linuxmachine/`), on the GPIO
character device (uAPI v2): lines by kernel name (`Pin("PA4")`) or `(chip, offset)`, modes `IN`,
`OUT`, `OPEN_DRAIN`, pulls `PULL_UP`, `PULL_DOWN`, `value()`, `on()`, `off()`, `toggle()`, `init()`,
`deinit()`. A line used by the kernel raises `OSError(EBUSY)`; `irq()` is not provided.
`tests/hil/t17_mp135_pin.py` checks names, errors, inputs with pulls and release without driving any
line; outputs need two free connector pins joined by a jumper (`tests/hil/t18_mp135_pin_loopback.py`,
not run yet). This section records an architectural decision; it does not imply that all of
these classes are already implemented.

## Flash references

The exact package names, SDK names, switch settings and Linux/Windows commands are recorded in
`docs/ricognizione.md`. The validated host executable used for this flash was:

```text
<STM32CubeProgrammer install directory>/bin/STM32_Programmer_CLI
```

The Starter Package was extracted below:

```text
/opt/devel/lvgl/lvgl-mpy-linux/work/starter-package/stm32mp1-openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10/
```
