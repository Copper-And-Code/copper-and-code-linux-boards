# STM32MP135F-DK board definition for LVGL MPY Studio (local, experimental)

This folder keeps the LVGL MPY Studio board definition of the STM32MP135F-DK outside the Studio
repository while the Linux board is experimental. Nothing here is published.

- `boards/stm32mp135f_dk_480x272.json`: the board profile (480×272, RGB565, `display_driver`,
  `mpremote` over the USB CDC-ACM REPL).

## Using it in the Studio

The Studio does not load board definitions from a file yet: the work is described for its agent in
`lvgl-mpy-studio/prompts/import-board-esterne.md`. Once it is done, this JSON file is imported as it
is. The earlier built-in version of the profile (Studio commit 7175b3e, reverted) is kept as a patch
in the history of this repository, commit a382b74.

## Adding it to the Studio later

Give the Studio a way to load board definitions from outside its repository, for example a
folder of profiles named by a setting or an environment variable, with the adapter's module
list taken from the profile's `firmware.modules` instead of the fixed `ADAPTERS` table. This
folder would then be loaded as it is, with no MP135-specific code in the Studio.
