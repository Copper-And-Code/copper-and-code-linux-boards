# STM32MP157C-DK2: plan (not started)

Second board of this project, after the STM32MP135F-DK is complete. Product page:
<https://www.st.com/en/evaluation-tools/stm32mp157c-dk2.html>.
Flashing and boot: [Populate the target and boot the image](https://wiki.st.com/stm32mpu/wiki/Getting_started/STM32MP1_boards/STM32MP157x-DK2/Let%27s_start/Populate_the_target_and_boot_the_image)
on the ST wiki.

The goal is the same as on the STM32MP135F-DK (MicroPython unix port with LVGL on the Cortex-A7,
the MCU-style REPL, `display_driver`, LVGL MPY Studio), plus a `machine` backend served by the
board's Cortex-M4. This document separates what was checked from what is still assumed.

## Checked

- MicroPython v1.29.0 has no STM32MP157 board in `ports/stm32/boards`: a MicroPython build for the
  Cortex-M4 would be a new port of the stm32 port, not an existing board.
- MicroPython v1.29.0 has an `openamp` module (`extmod/modopenamp.c`,
  `extmod/modopenamp_remoteproc.c`) with RPMsg endpoints and remoteproc, used today on MCU ports
  (`ports/stm32/mpmetalport.c`, `mpremoteprocport.c`). It is the MCU side of the link, not the
  Linux side.

## Checked in the release's files (no board yet)

The OpenSTLinux release used for the STM32MP135F-DK (`openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10`)
covers this board too: its Starter Package has the microSD flash layouts
(`FlashLayout_sdcard_stm32mp157c-dk2-optee.tsv` and others) and its SDK, the same one, has the device
trees `stm32mp157c-dk2.dtb` and `stm32mp157c-dk2-m4-examples.dtb`. Both CPUs are Cortex-A7 with the
same ABI (`cortexa7t2hf-neon-vfpv4`), so the same toolchain builds for both. From the device trees:

| Topic | stm32mp157c-dk2.dtb |
|---|---|
| Display | DSI panel `orisetech,otm8009a` through `st,stm32-dsi` and `st,stm32-ltdc`; no separate backlight node (the panel driver controls the brightness) |
| Touch | `focaltech,ft6236` on I2C at `0x38`, `touchscreen-size` 480 × 800: a 480×800 portrait panel |
| USB device port | `st,stm32mp15-hsotg` (`dwc2`) at `49000000`, `dr_mode = "otg"`, `usb-role-switch`: the composite serial and network gadget of the STM32MP135F-DK should work |
| Cortex-M4 | `st,stm32mp1-m4` under remoteproc, with reserved memory (`mcuram`, `mcuram2`, `retram`, `vdev0buffer` for RPMsg) and mailboxes `vq0`, `vq1`, `shutdown`, `detach` |
| Peripherals given to the Cortex-M4 (`m4-examples`) | ADC `48003000`, DAC `40017000`, DMA `48001000`, I2C `40015000`, SPI `44005000`, UART `40018000`, timers `40000000` and `44000000`, CRC, CRYP, HASH, RNG |

The `m4-examples` device tree is a ready starting point for `machine`: it already hands the Cortex-M4
the ADC, DAC, I2C, SPI, a UART and two timers (PWM).

Consequences:

- The panel is 480×800 portrait, not 480×272: the LVGL MPY Studio examples need a new board profile
  and a layout for it, or the display turned to 800×480 landscape in software (LVGL rotation, at a
  CPU cost to measure).
- `display_driver` and `touch_evdev` find the resolution, the touchscreen and the backlight by
  themselves; the FT6236 range matches the panel, so no `MPY_TOUCH_RANGE` is expected.

## To verify on the board (rule 6 of the plan: device tree first)

- That the release boots on the board: flash with `FlashLayout_sdcard_stm32mp157c-dk2-optee.tsv`,
  then `uname -a`, `/etc/os-release`, the inventory of the STM32MP135F-DK's phase 1.
- Whether `linuxfb` (DRM dumb buffer) works unchanged on the DSI panel, and how the brightness is
  exposed (a `/sys/class/backlight` entry from the panel driver, or none).
- USB gadget: which port is dual-role, UDC name; whether `mpy-usb-composite.sh` applies.
- remoteproc: `/sys/class/remoteproc/remoteproc0` (`firmware`, `state`), where firmware files are
  looked up (`/lib/firmware`), and the RPMsg devices the kernel offers (`rpmsg_tty`:
  `/dev/ttyRPMSG*`, or `rpmsg_char`).
- Which connector pins the peripherals given to the Cortex-M4 use (the user manual of the board),
  and which of them `machine` will offer first.
- Free GPIO lines and their connector pins for the HIL tests.

## Design (from the portable `machine` API decision in `docs/stm32mp135f-dk.md`)

- `machine.Pin` keeps the interface already implemented for the STM32MP135F-DK
  (`modules/linuxmachine/`). A second backend sends the same operations to the Cortex-M4 over RPMsg;
  which backend a pin uses depends on the board, not on application code.
- First classes: `Pin` and `I2C`. `PWM`, `ADC` and `Timer` later.
- The RPMsg protocol (requests, replies, errors, versions) is to be defined before the firmware.
- Cortex-M4 firmware options, to be decided: written from scratch on STM32Cube or Zephyr with
  OpenAMP; or MicroPython on the Cortex-M4, which needs the port described above.

## Layout

| Path | Content |
|---|---|
| `boards/unix/STM32MP157C_DK2/` | Unix variant for the Cortex-A7 (placeholder) |
| `mcu/stm32mp157c-dk2/` | Cortex-M4 firmware (placeholder) |
| `docs/stm32mp157c-dk2.md` | This plan |
