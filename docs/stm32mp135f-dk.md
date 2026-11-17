# STM32MP135F-DK MicroPython/LVGL target

This is a personal project by Nunzio Raciti and is not affiliated with or endorsed by
STMicroelectronics.

Target: STM32MP135F-DK, MB1635, running MicroPython with LVGL and controlled by
LVGL MPY Studio. The ST-LINK virtual COM port remains the Linux console and is never reused as
the MicroPython REPL.

## Current status

- Phase 0 reconnaissance: complete; see `docs/ricognizione.md`.
- Phase 1 flash and initial inventory: complete.
- Phase 2 system preparation: in progress.
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
failed, and Nunzio confirmed a black panel with no ST demo. SSH remained available.

### Measurements and remaining physical checks

| Item | Verified value |
|---|---|
| Original boot time | 2 min 10.670 s: kernel 1 min 17.307 s, userspace 53.363 s |
| Boot without Weston | 37.445 s: kernel 12.430 s, userspace 25.015 s |
| Memory without Weston | 437 MiB total, 352 MiB available at measurement time |
| DRM connector | DPI-1, connected, 480×272 at 59.99 Hz |
| Primary plane formats | Includes `RG16` (RGB565) and `XR24` (XRGB8888) |
| Goodix X ranges | `ABS_X` and `ABS_MT_POSITION_X`: 0–479 |
| Goodix Y ranges | `ABS_Y` and `ABS_MT_POSITION_Y`: 0–271 |
| Goodix multitouch | Five slots (`ABS_MT_SLOT` 0–4), `INPUT_PROP_DIRECT` |
| Backlight range | 0–10; restored to 10 after the initial check |

The four-corner touch capture and visual backlight checks at 0, 5 and 10 are still pending. A
sysfs write of 0 was accepted (`actual_brightness=0`), but visual extinction has not been confirmed
and must not be treated as verified.

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
