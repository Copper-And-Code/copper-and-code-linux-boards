# STM32MP157C-DK2 unix variant (not needed)

The STM32MP157C-DK2 runs the variant of `../STM32MP135F_DK/` unchanged: both boards have Cortex-A7
cores with the same ABI, the same OpenSTLinux release and SDK, and what differs between them is
found at run time (display, touch, USB controller) or set by `linux/install.sh` (screen rotation,
audio output, device tree, Cortex-M4 firmware). This folder stays for a variant of its own, should
one be needed. Board notes: `docs/stm32mp157c-dk2.md`.
