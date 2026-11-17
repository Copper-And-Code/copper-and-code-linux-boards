# STM32MP157C-DK2: plan (not started)

Second board of this project, after the STM32MP135F-DK is complete. Product page:
<https://www.st.com/en/evaluation-tools/stm32mp157c-dk2.html>.

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

## To verify on the board (rule 6 of the plan: device tree first)

- OpenSTLinux release and kernel for STM32MP15 in the release chosen for the STM32MP135F-DK.
- Display: panel, interface, resolution and touch controller, from the device tree of
  `stm32mp157c-dk2` and the user manual. Whether `linuxfb` (DRM dumb buffer) works unchanged.
- USB gadget: which port is dual-role, UDC name; whether `mpy-usb-composite.sh` applies.
- remoteproc: `/sys/class/remoteproc/remoteproc0` (`firmware`, `state`), where firmware files are
  looked up (`/lib/firmware`), and the RPMsg devices the kernel offers (`rpmsg_tty`:
  `/dev/ttyRPMSG*`, or `rpmsg_char`).
- Which pins and peripherals (GPIO, I2C, SPI, PWM, ADC, timers) are assigned to the Cortex-M4 in the
  device tree (resource manager), and which the connectors expose.
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
