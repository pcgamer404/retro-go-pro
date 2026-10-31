# snes-go

Standalone Super Nintendo emulator for ESP32-S3 handhelds, built on **Snes9x** (2010 / libretro snes9x2010 lineage).

Supports save states, screenshots, adjustable audio and several control profiles.

## Features

- Standalone app: one firmware partition, separate from the multi-system core
- Double-buffered display, so the LCD never reads a frame while it is being rendered
- Hot Z-buffer in internal RAM, larger sub-screen buffers in PSRAM
- Audio runs on its own task with a two-buffer queue
- Built with `-O3` plus switch/dispatch optimizations for the ESP32-S3
- Mode 5/6 (hi-res) rendered at half width to fit the 256-pixel framebuffer
- Direct-colour brightness handled through the existing brightness LUT
- Special chips: DSP, C4, OBC1 and S-RTC (SuperFX/SA-1 are not included)

## ROMs

Put `.smc`, `.sfc` or `.zip` files here:

```
/sd/roms/snes/
```

Example:

```
/sd/roms/snes/Super Mario World.sfc
/sd/roms/snes/Zelda.zip
```

- Save states and screenshots are stored by Retro-Go in its usual `saves` and `covers` folders
- Cover art goes in the standard Retro-Go covers folder for `snes`

## Controls

Choose a profile in **Options > Controls**. Hold **MENU** and press a button for the second function where the profile has one.

| Profile | Layout |
|---|---|
| **Regular** | A, B, X, Y, L, R, START, SELECT map one to one (default when the device has X/Y/L/R) |
| **Type A** | A, B, START and SELECT act as SNES A, B, X and Y; MENU plus B/A act as L/R, MENU plus START/SELECT act as START/SELECT (default on 2-button devices) |
| **Type B** | START, A, SELECT and B act as SNES A, B, X and Y; MENU combos as Type A |
| **Type C** | A, B, START and SELECT only |

Other options: **Audio enable** and **Audio filter** (low-pass).

## Credits

- **[pjcau/retro-go](https://github.com/pjcau/retro-go)**: the Retro-Go SNES core this build comes from, including its ESP32-S3 renderer work. Thanks to pjcau and the [ESP32 Emu Turbo](https://github.com/pjcau/esp32-emu-turbo) project.
- **[libretro/snes9x2010](https://github.com/libretro/snes9x2010)**: Snes9x core the port is based on.
- **dking, GBAtemp users BassAceGold, ShadauxCat and Nebuleon**: the Snes9x NDS port the core derives from (see `components/snes9x/src/LICENSE`).
- **The Snes9x team**: the original emulator.
- **[Retro-Go](https://github.com/ducalex/retro-go)** by ducalex and contributors: the frontend it runs on.

## License

The emulator core is under the licenses in `components/snes9x/src/LICENSE` (GPL-2.0-or-later) and the headers of individual source files. Snes9x itself has its own non-commercial terms, so keep this project non-commercial.

ROMs are not included. Use only dumps of games you own.
