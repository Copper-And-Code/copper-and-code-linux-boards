# Cortex-M4 firmware for the STM32MP157C-DK2

The firmware that serves `machine.I2C` and `machine.ADC` to MicroPython on Linux, over RPMsg. Linux
loads it through remoteproc (`/lib/firmware/mpy_m4.elf`). Built with `arm-none-eabi-gcc` against
STM32CubeMP1 1.7.0, without STM32CubeIDE:

```sh
scripts/build_m4.sh                                  # fetches STM32CubeMP1 into work/, writes dist/mpy_m4-*.elf
make -C mcu/stm32mp157c-dk2 APP=<STM32CubeMP1>/Projects/STM32MP157C-DK2/Applications/OpenAMP/OpenAMP_TTY_echo \
    NAME=openamp_tty_echo                            # ST's echo example, to check the RPMsg link
```

| Path | Content |
|---|---|
| `firmware/Src/main.c` | Requests: I2C5, ADC1/ADC2, the orange LED |
| `firmware/Inc/mpy_m4_protocol.h` | The protocol, shared with `python/linux/mpy_m4.py` |
| `firmware/Inc/stm32mp1xx_hal_conf.h` | The HAL modules beyond those of ST's echo example |

ST's OpenAMP glue (IPCC mailbox, resource table, interrupts, start-up code, linker script) is used
from STM32CubeMP1 as it is, not copied here. Design and pins: `docs/stm32mp157c-dk2.md`.
