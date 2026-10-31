# pokegbsp: Game Boy Advance for Retro-Go (ESP32-S3)

Game Boy Advance emulator app for [Retro-Go](https://github.com/pjcau/retro-go), built on
**gpSP** with an **Xtensa LX7 dynamic recompiler** (ARM7TDMI to Xtensa). Targets the ESP32-S3
handheld (16 MB flash, 8 MB octal PSRAM). The launcher's **GBA** tab runs this app.

## Features

- **Dynarec for ARM and Thumb**, translation caches in executable PSRAM, with guard areas that
  report an overrun instead of corrupting the heap.
- **Interpreter fallback per game**: after three crashes or hangs the dynarec is disabled for
  that ROM (a power cut or an app switch is not counted as a crash, because the reset reason is
  checked). Games that rewrite their own code also drop to the interpreter. Both are recorded
  in `<rom>.jit` and can be overridden by *Fast CPU (dynarec)* in the options menu.
- **Scanline renderer on core 1**, triple-buffered, with a paged copy of VRAM so CPU/DMA writes
  do not wait for the renderer.
- **ROM cache in PSRAM**; ROMs larger than the cache are paged from the SD card.
- **Battery saves** (SRAM, Flash, EEPROM). Flash is configured for 128 KB so Pokemon hacks that
  need the second bank save correctly; the save *type* is still detected per game. Saves are
  copied to a snapshot and written by a background task once the game stops writing, then
  replaced atomically (a power cut cannot destroy the previous save).
- **Frameskip that works with the core-1 renderer**: *Auto* (the default) measures what a drawn and a skipped frame really cost in the running game, raises the level when the speed drops below 96% and lowers it again when the scene can carry it; 0/1/2 can also be pinned in the options menu. A skipped frame still emulates the game at 60 Hz, it just does not draw or submit that one. A scene that gets much lighter all at once (a race ends, a menu opens) gives back a level as soon as a drawn frame fits the 60 Hz budget instead of waiting out the usual hold.
- **Loading screen** while the ROM is copied into the PSRAM cache: the game's name, a progress bar and the percentage, instead of Retro-Go's hourglass sitting alone on a blank screen for the whole load.
- **Save states** and screenshots through the standard Retro-Go menus.
- **Real-time clock** for the games in the gpSP database that have one (Pokemon Ruby, Sapphire
  and Emerald among them): berries, tides and the day/night cycle follow the handheld clock.
- Optional official GBA BIOS, and Audio enable / Frameskip switches (per game).

## Controls

| GBA | Handheld |
|-----|----------|
| D-pad | D-pad |
| A | A |
| B | B |
| L | L or X |
| R | R or Y |
| Start | Start |
| Select | Select |
| Retro-Go menu | Menu (or Option) |

## Files on the SD card

```
/sd/roms/gba/                        ROMs (.gba, or .zip) - the launcher's GBA tab
/sd/roms/pokegbsp/bios/gba_bios.bin  optional official BIOS
/sd/roms/pokegbsp/saves/<rom>.sav    battery save for each ROM
/sd/roms/pokegbsp/cache/<rom>.jit    per-game dynarec state (created on first run)
```

Save states follow Retro-Go's own layout. The GBA save type is detected from the game's
accesses, as in upstream gpSP; delete `<rom>.jit` to give the dynarec another chance.

With debug logging enabled the app writes a `perf:` line every 5 seconds with the average
frame cost split into `exec` (core 0), `wait` (blocked on the core-1 renderer), `disp`
(display submit) and `audio`, plus the number of ROM pages read from the SD card and the
current frameskip level. `page` climbing steadily means the ROM does not fit in the PSRAM
cache and the game is paging.

## Build and flash

Part of the Retro-Go tree (ESP-IDF 5.x). From the tree root:

```
python rg_tool.py build pokegbsp --target my-handheld
python rg_tool.py flash pokegbsp --target my-handheld
```

The app partition is under 1 MB; the default 2 MB partition is plenty.

Environment switches:

| Variable | Effect |
|----------|--------|
| `GBAJIT=0` | Interpreter only: `xjit`/`esp_mm` and the translated-code paths are not built |

## Limitations

- No link cable / RFU multiplayer and no rumble motor support.
- The RTC is the handheld's clock, set in Retro-Go's settings; it runs while the device is off
  only as far as that clock is maintained.
- Games that generate code at run time can force the interpreter (see above).
- Audio uses gpSP's resampler at 32.768 kHz; it is not cycle-exact.

## Credits

- **[pjcau](https://github.com/pjcau)**: the Xtensa gpSP dynarec, `xjit`, the scanline renderer
  and the Retro-Go integration, from
  [xtensa-68000-dynarec](https://github.com/pjcau/xtensa-68000-dynarec) and
  [retro-go](https://github.com/pjcau/retro-go). This app builds on that work.
- **gpSP team**: **Exophase** (original gpSP), **notaz**
  ([gpsp](https://github.com/notaz/gpsp), dynarec and platform work), **davidgfnet**
  ([gpsp libretro](https://github.com/davidgfnet/gpsp), current maintainer), and the libretro
  contributors of `gbsp-libretro`.
- **[Retro-Go](https://github.com/ducalex/retro-go)** by ducalex and contributors, and the
  retro-go-pro fork this app is integrated into.

## License

`components/gbsp-libretro` is GPL-2.0 (see its `COPYING`). `components/xjit` carries its own
`COPYING`. Follow the license of each directory when redistributing.
