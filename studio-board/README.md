# STM32MP135F-DK board definition for LVGL MPY Studio (local, experimental)

This folder keeps the LVGL MPY Studio board definition of the STM32MP135F-DK outside the Studio
repository while the Linux board is experimental. Nothing here is published.

- `boards/stm32mp135f_dk_480x272.json`: the board profile (480×272, RGB565, `display_driver`,
  `mpremote` over the USB CDC-ACM REPL).

## Using it in the Studio

LVGL MPY Studio imports board definitions from a file (Studio commit d3aabe9): in the board profile
manager choose **Import…** and select `boards/stm32mp135f_dk_480x272.json`; **Delete** removes it
again. Connect to the CDC-ACM REPL port of the board, not to the ST-LINK console. The earlier
built-in version of the profile (Studio commit 7175b3e, reverted in 928ccdf) is kept as a patch in
the history of this repository, commit a382b74.
