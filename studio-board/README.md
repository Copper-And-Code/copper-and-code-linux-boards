# STM32MP135F-DK board definition for LVGL MPY Studio (local, experimental)

This folder keeps the LVGL MPY Studio board definition of the STM32MP135F-DK outside the Studio
repository while the Linux board is experimental. Nothing here is published.

- `boards/stm32mp135f_dk_480x272.json`: the board profile (480×272, RGB565, `display_driver`,
  `mpremote` over the USB CDC-ACM REPL).
- `studio-commit-7175b3e.patch`: the complete Studio change that had added the profile (profile,
  `stm32mp135_linux` adapter, documentation, test). It was reverted in the Studio repository.

## Using it locally

The profile needs the `stm32mp135_linux` adapter in the Studio code (`ADAPTERS` in
`lvgl_mpy_studio/core/boards.py`): a custom profile with an unknown adapter is rejected as invalid.
To try the board with a local Studio checkout, on a branch that is never pushed:

```sh
cd /opt/devel/lvgl/lvgl-mpy-studio
git switch -c local/stm32mp135f-dk
git apply /opt/devel/lvgl/lvgl-mpy-linux/studio-board/studio-commit-7175b3e.patch
```

## Adding it to the Studio later

Options, to be decided when the board is no longer experimental:

1. Apply the patch again as a built-in profile (what was done in 7175b3e).
2. Give the Studio a way to load board definitions from outside its repository, for example a
   folder of profiles named by a setting or an environment variable, with the adapter's module
   list taken from the profile's `firmware.modules` instead of the fixed `ADAPTERS` table. This
   folder would then be loaded as it is, with no MP135-specific code in the Studio.
