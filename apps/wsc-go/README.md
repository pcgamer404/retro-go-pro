# wsc-go

A WonderSwan / WonderSwan Color emulator app for Retro-Go on ESP32-S3.

The emulator core is the oswan core from Retro-Go. This app adds fullscreen
scaling, app-owned frameskip, 48 kHz audio, save states, cartridge SRAM/EEPROM
saves, stricter ROM validation, and banked SD streaming for ROMs that are too
large to fit in PSRAM.

## Features

- WonderSwan Color emulation (hardware ID reports `0x87`).
- Fullscreen scaling to the handheld LCD.
- Frameskip option:
  - `Off` - draw every emulated frame.
  - `1 (2/3)` - draw two out of every three frames (default).
  - `2 (1/3)` - draw one out of every three frames.
- 48 kHz stereo audio, submitted once per emulated frame.
- Save states and cartridge SRAM/EEPROM saves through Retro-Go.
- ROM validation for HTML error pages, truncated dumps, invalid sizes,
  512-byte copier headers, and overdumps.
- 8 MB ROM support through banked SD streaming.

## ROM support

- `.ws` and `.wsc` files.
- `.zip` archives are supported when the uncompressed ROM fits in PSRAM.
- ROMs with a 512-byte copier header at the start are accepted.
- ROMs larger than the free PSRAM are streamed from SD in 64 KB banks.
- **8 MB ROMs must be unzipped.** A zipped 8 MB ROM cannot be decompressed into
  PSRAM because the ROM alone is larger than usable PSRAM.

### 8 MB streaming

The emulator maps 12 ROM banks at a time through `C0`, plus one bank each
through `C2` and `C3`. For ROMs that do not fit in PSRAM, the app opens the ROM
file directly from SD and keeps an LRU cache of 64 KB banks in PSRAM (16 to 96
banks, 1 to 6 MB, depending on free memory). Banks referenced by the current CPU
mapping are pinned and are never evicted.

This keeps memory use predictable and leaves at least 1 MB of PSRAM free for
save states, screenshots, and cartridge RAM. The trade-off is that a bank switch
which misses the cache must read 64 KB from the SD card. Games that switch large
ROM windows frequently can pause briefly; games with a small working set run
mostly from the cache.

The first boot of an 8 MB game may take several seconds because the game often
checksums the whole ROM. This is normal.

8 MB streaming is new in this build. Smaller ROMs still use the original
full-buffer path, so their behavior is unchanged.

## Controls

| Retro-Go control | WonderSwan |
| --- | --- |
| D-Pad | X-Pad by default (Y-Pad after toggling) |
| A | B |
| B | A |
| Start | Start |
| Select | Toggle X-Pad / Y-Pad |
| Menu | Retro-Go game menu |
| Option | Options menu |

Notes:

- The WonderSwan has two D-Pads. Select toggles the physical D-Pad between the
  X-Pad and Y-Pad. Select is consumed by this toggle and is not sent to the
  emulator.
- The A/B mapping follows the common Retro-Go layout: the handheld's `A` button
  sends WonderSwan `B`, and the handheld's `B` button sends WonderSwan `A`.
- Menu opens the Retro-Go game menu. Option opens the emulator options menu.

## Frameskip

The WonderSwan refresh rate is about 75.5 Hz. A full 320x240 RGB565 frame over
the 80 MHz SPI display link takes about 15.4 ms, while one emulated frame is
about 13.3 ms, so drawing every frame cannot sustain 75 fps. The default
`1 (2/3)` mode draws two of every three frames and keeps input and audio at the
full emulation rate. The choice is saved in Retro-Go settings.

`Off` is available for games where every frame matters, but it may reduce
overall smoothness and can underrun USB audio on CPU-bound games.

## Saves

Retro-Go stores saves next to its normal saves directory:

- Cartridge SRAM/EEPROM: `<rom>.sram`
- Save states: `<rom>.sav`

The ROM itself is never modified.

## Building

Tested with ESP-IDF 5.3.5 for `esp32s3`.

On Windows PowerShell:

```powershell
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\python_env\idf5.3_py3.11_env'
. C:\Espressif\frameworks\esp-idf-v5.3.5\export.ps1
idf.py build
```

On Linux/macOS, source the ESP-IDF export script for your installation and run:

```sh
idf.py build
```

The app binary is written to:

```
build/wsc-go.bin
```

The project uses the shared Retro-Go build system and the existing
`testing/partitions.csv`; no partition changes are required.

## Flashing

Flash with the usual ESP-IDF or Retro-Go workflow:

```sh
idf.py -p COMx flash
```

or use the `esptool` command printed by `idf.py build`. Back up your SD card
saves before changing firmware.

## Known limitations

- Fullscreen 75 fps is not possible over the 80 MHz SPI display link; frameskip
  is required for smooth output.
- If the emulator runs below 75.5 fps, the 48 kHz audio stream can underrun on a
  USB DAC because audio is submitted once per emulated frame.
- Some tearing is possible because the LCD has no TE/vsync signal exposed.
- 8 MB games can stutter when they switch large ROM windows that are not in the
  bank cache. The cache is intentionally bounded to leave PSRAM for the rest of
  the system.
- Zipped 8 MB ROMs are not supported; unzip them first.
- 16 MB ROMs can be streamed in theory, but they will be slower and are not a
  target for this build.
- A clean build may show the pre-existing `nec.c` `iram1` section warnings, an
  unused-variable warning in `rg_input.c`, and unknown-kconfig-symbol notices
  from the target defaults. None affect the built app.

## Credits

- oswan WonderSwan emulator core.
- Retro-Go and its shared ESP32 components.
- Mednafen `wswan` as a hardware-behavior reference.
