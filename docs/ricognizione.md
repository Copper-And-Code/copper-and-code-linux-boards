# Phase 0 reconnaissance: STM32MP135F-DK, Linux, MicroPython and LVGL

Date: 2026-11-17

Scope: the no-hardware reconnaissance defined by the project plan. No board was flashed or
contacted. No upstream source was modified.

This is a personal project by Nunzio Raciti and is not affiliated with or endorsed by
STMicroelectronics.

## 1. Outcome and decisions still required

The selected starting release is **Yocto-based OpenSTLinux ecosystem v6.2.1**, confirmed by
Nunzio on 2026-11-17. It is the current release on the official wiki and explicitly supports
STM32MP135F-DK. No published v7 ecosystem release was found during this reconnaissance.

The host test shows that MicroPython v1.29.0 unix already provides the normal REPL, raw REPL,
raw-paste and the filesystem operations needed by `mpremote` when the connection uses `resume`.
It does **not** emulate an MCU soft reset: Ctrl-D terminates the process, without printing
`soft reboot`, and the default `mpremote` command sequence therefore fails. Phase 4 must add and
test process supervision, reset-mode restoration and boot/application startup. The final choice
among options A, B and C remains open pending that spike and Nunzio's approval.

The studio analysis also identified two integration requirements that must not be overlooked:

- The board driver must implement **display_driver contract 1**. The old `init()`, `poll()`,
  `tick()` and `wait()` interface belongs to the legacy/simulator path, not to current board
  application generation.
- Absolute paths such as `/lms_apps` are real Linux absolute paths on the unix port. A systemd
  `WorkingDirectory` alone does not map them below `/usr/local/mpy/fs`. Phase 4/6 must either
  provide explicit filesystem confinement or use a reviewed board-profile path mapping.

## 2. Evidence legend

- **Verified by execution**: a command was run on this host and its output is reported below.
- **Read**: information comes from the cited source code or official documentation.
- **Inferred**: a consequence of verified/read facts; it still needs an implementation test.
- **Deferred to hardware**: cannot be verified until the corresponding board phase.

## 3. OpenSTLinux release and packages

### 3.1 Release selection (read from official ST sources)

| Item | Value |
|---|---|
| Ecosystem | v6.2.1 |
| OpenSTLinux distribution | `openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10` |
| Linux kernel | `v6.6-stm32mp-r3.1`, LTS v6.6.129 |
| OpenEmbedded | v5.0.17, Scarthgap |
| SDK GCC | 13.4.0 |
| STM32CubeProgrammer validated for the release | v2.23 |
| Target board | STM32MP135F-DK, MB1635, Cortex-A7, 480x272 display and capacitive touch |
| Graphical image | `st-image-weston` |

The release-note page describes v6.2.1 as the current ecosystem release and lists
STM32MP135F-DK among the supported boards. Searches for a published v7 release returned only
earlier v6 pages. This is not a claim that an announcement or prerelease cannot exist; no v7
release suitable for this plan was found in the official release material consulted.

Sources:

- [Yocto-based OpenSTLinux ecosystem release note](https://wiki.st.com/stm32mpu/wiki/Yocto-based_OpenSTLinux_ecosystem_release_note)
- [STM32 MPU OpenSTLinux release note](https://wiki.st.com/stm32mpu/wiki/STM32_MPU_OpenSTLinux_release_note)

### 3.2 Starter Package names (read)

Download archive:

```text
FLASH-stm32mp1-openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10.tar.gz
```

Image/flashlayout root after extraction:

```text
stm32mp1-openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10/images/stm32mp1/
```

Default OP-TEE microSD flashlayout:

```text
flashlayout_st-image-weston/optee/FlashLayout_sdcard_stm32mp135f-dk-optee.tsv
```

The package also contains an `extensible` layout. The plan starts with the documented default
`optee` layout; changing partition layout is a separate decision.

The download was not attempted in Phase 0. The ST download may require authentication and
acceptance of SLA0048; if so, Nunzio will place the archive in `work/starter-package/`.

Sources:

- [Populate the target and boot the image](https://wiki.st.com/stm32mpu/wiki/Getting_started/STM32MP1_boards/STM32MP135x-DK/Let%27s_start/Populate_the_target_and_boot_the_image)
- [STM32MP13 Discovery kits Starter Package](https://wiki.st.com/stm32mpu/wiki/STM32MP13_Discovery_kits_-_Starter_Package)

### 3.3 Developer Package SDK names (read)

For an x86-64 Linux host:

```text
SDK-x86_64-stm32mp1-openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10.tar.gz
```

For an aarch64 host:

```text
SDK-aarch64-stm32mp1-openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10.tar.gz
```

The x86-64 installer inside the archive is:

```text
stm32mp1-openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10/sdk/st-image-weston-openstlinux-weston-stm32mp1.rootfs-x86_64-toolchain-5.0.17-openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10.sh
```

Install it with `-d <absolute-sdk-directory>`. The environment script confirmed by the official
page is:

```text
environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
```

Expected environment values are `ARCH=arm`, `CROSS_COMPILE=arm-ostl-linux-gnueabi-` and
`OECORE_SDK_VERSION=5.0.17-openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10`.
These values are documentation-derived and must be checked after SDK installation in Phase 3.

Source: [Install the SDK](https://wiki.st.com/stm32mpu/wiki/Getting_started/STM32MP1_boards/STM32MP135x-DK/Develop_on_Arm%C2%AE_Cortex%C2%AE-A7/Install_the_SDK).

## 4. Flash and boot procedure for Phase 1

### 4.1 Hardware connections and switches (read)

- `SW1` selects the boot mode. The table columns are Boot 0, Boot 1 and Boot 2.
- USB/UART programming: `0 0 0`.
- microSD boot: `1 0 1`.
- On SW1, `0` means **OPEN**; `1` means the other/closed position.
- Flash through **CN7 / USB_OTG**, the USB Type-C flashload and dual-role-port connector.
- Power through **CN12**, USB Type-C, 5 V / 3 A.
- Keep the Linux console on **CN10**, USB Micro-B ST-LINK/V3E. It must not become the
  MicroPython REPL channel.

Source: [STM32MP135x-DK hardware description](https://wiki.st.com/stm32mpu/wiki/STM32MP135x-DK_-_hardware_description).

### 4.2 Commands (read; execute only at the hardware checkpoint)

Run from the `images/stm32mp1` directory of the extracted Starter Package.

Linux:

```bash
STM32_Programmer_CLI -l usb
STM32_Programmer_CLI -c port=usb1 \
  -w flashlayout_st-image-weston/optee/FlashLayout_sdcard_stm32mp135f-dk-optee.tsv
```

Windows uses the same arguments with `STM32_Programmer_CLI.exe`. The USB index must be taken
from the actual `-l usb` output rather than assumed if more than one DFU device is present.

Phase 1 host check on 2026-11-17: `STM32_Programmer_CLI.exe --version` ran successfully and
reported **2.22.0**. The tool must be updated to the release-validated **2.23.0** before flashing;
the older installed version was not used.

After flashing, power/reset as directed by ST, set SW1 to microSD (`1 0 1`) and boot. The
expected startup sequence is U-Boot splash, Linux splash, Weston and the GTK demo launcher.
The expected host console command is:

```bash
picocom -b 115200 /dev/ttyACM0
```

The host device name, the board-side console name and the successful boot are **deferred to
hardware**. Do not assume `ttySTM0` without checking the board inventory.

## 5. lvgl-mpy-studio reconnaissance

### 5.1 Repository and dependency pins (verified locally and read)

Studio repository:

```text
/opt/devel/lvgl/lvgl-mpy-studio
commit fe9638f8dc5e785a2251125717ce18c8d3df10ad
branch main
```

`pyproject.toml` requires `mpremote==1.29.0` and `pyserial==3.5`. The simulator build scripts and
the actual local `.reference` checkouts agree on:

| Component | Pin |
|---|---|
| MicroPython | v1.29.0, commit `0fd6c573ea815774668bbb16b8e197c8822368b2` |
| PyDevices/lvgl-micropython | `58d18ce5b48cd7acce609317991627a046f998f8` |
| PyDevices/lvgl-bindings | `7740c2fae0daa10992f5f8fe54acf670a70e2d3b` |
| LVGL | 9.5.0 |

The simulator uses the bindings' `lv_conf.h` unchanged: RGB565/16-bit color, full-screen DIRECT
rendering and Montserrat 14, 16, 24, 32 and 40. SDL belongs to `lvsim_io`; `LV_USE_DRAW_SDL=0`.
This does not prove that the PyDevices desktop glue avoids SDL at link time; that remains a
Phase 3 cross-build risk.

Primary source files: `runtime/build.sh`, `docs/target.md`, `runtime/lv_conf.h` and
`pyproject.toml` in the studio repository.

### 5.2 How the studio communicates with a normal board (read)

For every adapter except `stm32h7_uart`, `DeviceSession` invokes `python -m mpremote`.
The STM32H7 adapter instead uses the studio's own `UartTransport`; that adapter is not the model
to select for this Linux board.

Important operations:

| Studio action | Command or transport behavior |
|---|---|
| Connect | `break_in(port)`, then the device-information raw-REPL probe |
| Execute source | `mpremote connect PORT resume exec SOURCE` |
| Reset | `mpremote connect PORT soft-reset`, then a 0.5 s host sleep |
| Upload one file | `mpremote connect PORT fs cp LOCAL :REMOTE` (no `resume`) |
| Read one file | `mpremote connect PORT resume fs cp :REMOTE LOCAL` |
| Run generated app | raw-REPL exec which inserts the app directory in `sys.path` and imports `app_main` |
| Stop app | raw-REPL exec which removes app modules and calls `display_driver.stop()` |
| Interactive REPL | `mpremote connect PORT repl` |
| Console | direct `mpremote.transport_serial.SerialTransport`, with a continuous reader |

Before a standard connection, `break_in()` writes `\r\x03`, then `\x03`, then `\x03`, with
0.3 s pauses. With `resume`, mpremote's raw entry writes `\r\x03`, flushes pending input and
writes `\r\x01`. It expects:

```text
raw REPL; CTRL-B to exit\r\n>
```

Raw-paste starts with `\x05A\x01`. A supporting target answers `R\x01`, followed by the
little-endian flow-control window and window-availability bytes. Execution ends with Ctrl-D;
the response contains `OK`, stdout, Ctrl-D, stderr, Ctrl-D and `>`.

Without `resume`, mpremote enters raw mode and immediately sends Ctrl-D. It requires
`soft reboot\r\n`, followed by another raw-REPL banner. This exact behavior explains the failed
standard commands in section 7 and is a mandatory compatibility requirement for Phase 4.

Upload generates and syntax-checks the application, probes firmware capabilities, soft-resets,
writes into `<app>.uploading`, verifies remote file sizes, rotates the previous app to
`.previous`, renames staging into place and optionally imports `app_main`. File copies without
`resume` may themselves request a soft reset, so reset handling must work repeatedly during one
upload, not only from the explicit Restart action.

Sources: `lvgl_mpy_studio/device/service.py`, `lvgl_mpy_studio/device/uart.py`, and mpremote
1.29.0 `transport_serial.py` and `commands.py`.

### 5.3 Board-profile contract (read)

A profile is schema version 1 and includes:

- `id`, `name`, `revision` and one registered `adapter`;
- minimum MicroPython/LVGL versions and required modules;
- physical/logical width and height, scale, RGB565 and orientation `[0]`;
- optional font list;
- `config.driver="display_driver"`, `config.buffers="driver"` and
  `config.eventLoop="driver"`;
- optional `appRoot`, `mainPath` and touch configuration.

The serial port is a local user preference, not a profile field. The current adapter registry
contains only `waveshare_rgbpanel`, `stm32h7_uart` and `esp32_4848s040`. Phase 6 therefore needs
a reviewed Linux adapter/profile change; reusing an MCU adapter name would be misleading.

The default app root is `/lms_apps`, and the default autostart path is `/main.py`. The STM32H7
profile instead uses `/flash/lms_apps` and `/flash/main.py`. On this target the final mapping
must be consistent with `/usr/local/mpy/fs` and with actual unix path semantics.

`machine.unique_id()` is optional during the initial information probe but mandatory for
autostart transactions. The standard unix port does not provide an MCU machine identity. A
stable, reviewed identity solution is therefore required before enabling studio autostart.

`mpremote` supports socket connections, but the studio's direct console is serial-specific and
`break_in()` uses pyserial. TCP upload/exec may be feasible while full console support is not;
this must be tested and, if needed, fixed in the studio in Phase 6 rather than hidden in the
board firmware.

### 5.4 Display driver contract used by generated board apps (read)

The current contract is `CONTRACT = 1` and is documented in this repository in
`docs/display-driver.md`. The required public names are:

```text
CONTRACT, WIDTH, HEIGHT, BUFFERS, PERIOD_MS, indev
start(), stop(), running(), backlight(percent=None)
```

Importing `display_driver` initializes LVGL, display and touch and starts background LVGL
service. The generated `app_main.py` checks resolution, imports `ui` and `app_events`, loads the
initial screen, calls `display_driver.start()` and returns. It does not own an event loop.

The simulator's contract-1 driver (`runtime/program/display_driver.py`) uses:

- one full-screen RGB565 `bytearray`;
- `lv.DISPLAY_RENDER_MODE.DIRECT`;
- a 5 ms background service;
- `lv.tick_inc()` followed by `lv.timer_handler()` in that service;
- a pointer indev fed by the simulator;
- idempotent `start()` and `stop()`, with the last frame retained on stop.

The older `runtime/display_driver.py` and `project_gen.py` legacy path use `init()`, `poll()`,
`tick()`, `wait()` and an application-owned loop. They are not the interface to implement for
new board-generated applications.

## 6. Host build: MicroPython v1.29.0 unix (verified by execution)

Workspace:

```text
/opt/devel/lvgl/lvgl-mpy-linux/work
```

The clone was checked out at tag v1.29.0, commit
`0fd6c573ea815774668bbb16b8e197c8822368b2`. `mpy-cross` and the standard unix binary built
successfully. The correct command sequence initializes submodules before starting a parallel
build:

```bash
make -C work/micropython/mpy-cross -j"$(nproc)"
make -C work/micropython/ports/unix submodules
make -C work/micropython/ports/unix all -j"$(nproc)"
```

An initial attempt using `make ... submodules all -j"$(nproc)"` failed because compilation raced
the submodule checkout and could not find mbedtls/Berkeley DB headers. Splitting the two targets
resolved it without source changes.

Build output:

```text
LINK build-standard/micropython
   text    data    bss     dec     hex filename
 796423   71552   3024   870999   d4a57 build-standard/micropython
```

Binary inspection and runtime output:

```text
ELF 64-bit LSB pie executable, x86-64, dynamically linked, stripped
(name='micropython', version=(1, 29, 0, ''), _machine='linux [GCC 13.3.0] version',
 _mpy=2822, _build='standard', _thread='unsafe')
linux
```

The host's globally installed mpremote was 1.27.0, so tests used an isolated environment with
the same version as the studio:

```bash
python3 -m venv work/host-tools
work/host-tools/bin/pip install mpremote==1.29.0
work/host-tools/bin/mpremote --version
# mpremote 1.29.0
```

## 7. PTY and mpremote tests (verified by execution)

Each test launched a fresh standard unix binary behind a raw PTY, equivalent to:

```bash
socat PTY,link=/tmp/upy,rawer \
  EXEC:$PWD/work/micropython/ports/unix/build-standard/micropython,pty,stderr,onlcr=0 &
```

### 7.1 Default mpremote behavior

Each command below was also rerun with a fresh process so that an earlier exit could not affect
the next result.

| Command | Exit | Result |
|---|---:|---|
| `mpremote connect /tmp/upy exec "print(1+1)"` | 1 | Fails waiting for `soft reboot`; unix exits and the PTY returns EIO |
| `mpremote connect /tmp/upy fs ls` | 1 | Same automatic-soft-reset failure |
| `mpremote connect /tmp/upy soft-reset` | 1 | Same missing soft-reboot emulation |

The final host-side error was `OSError: [Errno 5] Input/output error`. The underlying cause is
visible in `ports/unix/main.c` and `shared/runtime/pyexec.c`: Ctrl-D returns a forced-exit result,
`main()` deinitializes and returns; no `soft reboot` message or in-process reset exists.

### 7.2 Commands with `resume`

| Command | Exit | Observed output/result |
|---|---:|---|
| `... connect /tmp/upy resume exec "import sys; print(1+1); print(sys.platform)"` | 0 | `2` and `linux` |
| `... connect /tmp/upy resume fs ls` | 0 | Listing of the process working directory |
| `... connect /tmp/upy resume fs cp work/phase0-hello.py :phase0-hello.py` | 0 | Copy completed |
| `... connect /tmp/upy resume fs cat :phase0-hello.py` | 0 | Exact test source printed |

A direct `SerialTransport` check entered raw mode with `soft_reset=False` and executed
`print(6*7)`. Output was `42`, and `transport.use_raw_paste` remained `True`. This verifies the
raw-paste path rather than a fallback to normal raw REPL.

### 7.3 Consequences for Phase 4

Verified: upstream raw REPL, raw-paste and filesystem commands work when no automatic reset is
requested. Verified: upstream alone does not meet the soft-reset protocol.

Inferred, still requiring a spike: a process supervisor may be able to preserve the serial
endpoint, print the expected reset banner, restart immediately in raw mode and run the system
boot/application hooks. It must also survive repeated resets during studio upload. If that
contour cannot meet the byte protocol and timing reliably, the C-module option remains the next
candidate. No final A/B/C selection is made by this document.

Not tested in Phase 0: board USB gadget behavior, TCP transport, boot.py/main.py automation,
safe mode, Ctrl-C during a graphical app, 200 KB throughput, reset latency, stress cycles, RSS
stability, `mpremote mount`, Windows hardware behavior or any display/touch function.

## 8. Phase 0 completion checklist

- [x] Current OpenSTLinux release, kernel and STM32MP13 support checked against official sources.
- [x] Starter Package, image, flashlayout, CubeProgrammer and SDK names recorded.
- [x] DFU/microSD switches, CN7 flashing connector, CN12 power and ST-LINK console recorded.
- [x] Studio commands, raw-REPL byte expectations, board profile and driver contract identified.
- [x] Simulator PyDevices/bindings pins read and compared with local checkouts.
- [x] MicroPython v1.29.0 built for x86-64 without source modifications.
- [x] Default and `resume` mpremote behavior tested through fresh PTYs; raw-paste explicitly checked.
- [x] Soft-reset and absolute-path integration risks recorded.
- [x] Nunzio confirms OpenSTLinux ecosystem v6.2.1 for Phase 1.
