# STM32MP157C-DK2 unix variant (not started)

Placeholder for the second board. It will hold, as `../STM32MP135F_DK/` does, the out-of-tree unix
variant of MicroPython for the Cortex-A7 cores: `mpconfigvariant.h`, `mpconfigvariant.mk` and
`manifest.py`. It is not a buildable variant yet, so `scripts/build_mp135.sh` is not affected.

Plan and open questions: `docs/stm32mp157c-dk2.md`. The firmware of the Cortex-M4 goes in
`mcu/stm32mp157c-dk2/`.
