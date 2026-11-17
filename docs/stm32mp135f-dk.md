# STM32MP135F-DK MicroPython/LVGL target

This is a personal project by Nunzio Raciti and is not affiliated with or endorsed by
STMicroelectronics.

Target: STM32MP135F-DK, MB1635, running MicroPython with LVGL and controlled by
LVGL MPY Studio. The ST-LINK virtual COM port remains the Linux console and is never reused as
the MicroPython REPL.

## Current status

- Phase 0 reconnaissance: complete; see `docs/ricognizione.md`.
- Phase 1 flash and initial inventory: complete.
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

## Flash references

The exact package names, SDK names, switch settings and Linux/Windows commands are recorded in
`docs/ricognizione.md`. The validated host executable used for this flash was:

```text
/home/nunzio/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI
```

The Starter Package was extracted below:

```text
/opt/devel/lvgl/lvgl-mpy-mpu/work/starter-package/stm32mp1-openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10/
```
