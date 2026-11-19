# C64 core

A PAL Commodore 64 as a libretro core for [mpy-retro](../../docs/retro.md): 6510, the two CIAs,
VIC-II, SID 6581 and 8580, an emulated 1541 with its own 6502, and 8/16 KiB cartridges.

| File | What |
|---|---|
| `core/` | The emulator, unchanged, from the STM32H743 port (c64-stm32h7), which took it from the RP2350 port (c64-rp2350) at commit `dd5db0bbf2c5357ce4190035c636410b758b7fbc`; the STM32 port adds the dummy reads of indexed stores as real bus reads (Billiards) and `vicr_expand_native()` |
| `libretro.c` | The libretro wrapper: ROMs, games, keyboard, joystick, sound, fast disk loading |
| `LICENSE` | MIT, for both |

Built by `scripts/build_retro.sh` into `c64_libretro.so`.

## The SID tables

`core/sid_6581_combined.h`, `core/sid_8580_combined.h` and the `cutoff_*` and `filter_gain_*`
tables of `core/sid.c` are data: measurements of the output of VICE 3.7.1 (reSID 6581 and 8580),
taken with the RP2350 project's own test programs. No VICE or reSID code is included.

## What is not here

- **The Commodore ROMs.** They are copyrighted and come from the user, in the cores' system folder
  on the board, `/usr/local/mpy/retro/system/c64/`: `basic.901226-01.bin`,
  `kernal.901227-03.bin`, `characters.901225-01.bin` and, for disks,
  `dos1541-325302-01+901229-05.bin`.
- **Games and demos.** For example [Nine](https://www.linusakesson.net/scene/nine/) by Linus
  Åkesson (lft), the demo used to check the core, is downloaded from its author's page.

## Games

| File | Starts |
|---|---|
| `.prg` | Loaded at its address; a BASIC program (at $0801) is RUN, another waits for SYS |
| `.d64` | `LOAD"*",8,1` and `RUN` are typed; while the 1541's motor turns, three frames run for each one shown |
| `.crt` | The cartridge, after a reset |

## Measured on the STM32MP157C-DK2

50.1 emulated and 49.9 shown frames a second (the PAL rate; the panel switched to its 50 Hz mode),
5.4 ms to scale and turn a frame on the other CPU. With the disk drive running, about 2.5 times
real speed. On one Cortex-A7 the core alone runs 3.3 times faster than real time.
