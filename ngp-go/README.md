# ngp-go

Neo Geo Pocket and Neo Geo Pocket Color emulator for ESP32-S3 handhelds, running the **RACE** core (libretro port).

Supports save states, cartridge battery saves, screenshots and automatic frameskip.

## Features

- Standalone app in its own partition, so RACE's static tables don't use the internal RAM of the other emulators
- Native 160x152 RGB565 output
- Core memory (RAM, ROM, palette, sprite buffers) allocated in PSRAM only while the app runs
- Sound chip emulated at 16 kHz and interpolated to 32 kHz output, to keep CPU load down on busy games
- Cartridge flash (`.ngf`) is committed on save state, reset and shutdown
- No BIOS file required

## ROMs

Put your ROM files here:

```
/sd/roms/ngp/
```

Supported extensions: `.ngp`, `.ngc` (`.zip` is listed by the launcher, but uncompressed ROMs are the safe choice).

Example:

```
/sd/roms/ngp/Metal Slug 1st Mission.ngc
/sd/roms/ngp/Sonic Pocket Adventure.ngc
```

### Where files are stored

```
/sd/retro-go/saves/    <- save states and cartridge saves (.ngf)
```

Screenshots and cover art follow the standard Retro-Go folders for `ngp`.

## Controls

| Handheld button | Neo Geo Pocket |
|---|---|
| D-pad | D-pad |
| A | A |
| B | B |
| START | Option |
| MENU | Game menu (save/load state, reset, quit) |

The console is always emulated in colour mode.

## Credits

- **[pjcau/retro-go](https://github.com/pjcau/retro-go)**: the Retro-Go integration this app is based on, including the video, audio, input and save-state glue in `main/main_ngp.c`, from the [ESP32 Emu Turbo](https://github.com/pjcau/esp32-emu-turbo) project. Thanks to pjcau.
- **RACE** (Neo Geo Pocket emulator by Flavor, with the NeoPop code it descends from) and the **[libretro](https://github.com/libretro) team** for the libretro port used here.
- **[Retro-Go](https://github.com/ducalex/retro-go)** by ducalex and contributors: the frontend it runs on.

## License

The RACE core is under the **GNU GPL v2 or later** (`components/race/src/license.txt`). Other files keep the licenses in their headers.

ROMs are not included. Use only dumps of games you own.
