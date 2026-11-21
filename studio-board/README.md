# Board definitions for Copper and Code Studio

This folder keeps the Copper and Code Studio board definition of the STM32MP135F-DK next to the firmware it
describes, outside the Studio repository: the Studio imports it from the file.

- `boards/stm32mp135f_dk_480x272.json`: the board profile (480×272, RGB565, `display_driver`,
  `mpremote` over the USB CDC-ACM REPL).
- `boards/stm32mp157c_dk2_800x480.json`: the STM32MP157C-DK2, 800×480 (its 480×800 panel turned by
  `display_driver`), checked compatible by the Studio on the board.

## Using it in the Studio

Copper and Code Studio imports board definitions from a file (Studio commit d3aabe9): in the board profile
manager choose **Import…** and select `boards/stm32mp135f_dk_480x272.json`; **Delete** removes it
again. Connect to the CDC-ACM REPL port of the board, not to the ST-LINK console. The earlier
built-in version of the profile (Studio commit 7175b3e, reverted in 928ccdf) is kept as a patch in
the history of this repository, commit a382b74.
