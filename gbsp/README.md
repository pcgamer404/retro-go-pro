# gbsp: Game Boy Advance for Retro-Go (ESP32-S3)

Game Boy Advance emulator app for [Retro-Go](https://github.com/pjcau/retro-go), built on
**gpSP** with an **Xtensa LX7 dynamic recompiler** (ARM7TDMI to Xtensa). Runs on the ESP32-S3
(16 MB flash, 8 MB octal PSRAM) handheld target.

## Layout

| Path | Contents |
|------|----------|
| `main/` | Retro-Go app: frame loop, input, audio, display, save states, battery saves, options menu |
| `components/gbsp-libretro/` | gpSP core (interpreter, dynarec, video, sound, memory); Xtensa backend in `xtensa/` |
| `components/xjit/` | Xtensa JIT helpers: emitter, blocks, executable-memory allocation |

## Features

- Xtensa dynarec for ARM and THUMB code, translation caches in PSRAM mapped executable
- Interpreter fallback: per game, automatically after a crash or hang on the dynarec
  (`<rom>.jit` flag file), after a self-modifying-code storm, or by the "Fast CPU (dynarec)"
  switch in the options menu
- Scanline renderer on core 1, triple-buffered output
- Battery saves (SRAM/Flash/EEPROM) written beside the ROM as `.sav`, flushed on menu and shutdown
- Save states, ROM loading progress display, ROM cache paged from the SD card
- Translation-cache guard areas that log an overrun instead of corrupting the heap

## Build

Part of the Retro-Go tree (ESP-IDF 5.x):

```
python rg_tool.py build gbsp
```

Environment switches (top-level `CMakeLists.txt`):

| Variable | Effect |
|----------|--------|
| `GBAJIT=0` | Interpreter only (no `xjit` / `esp_mm`) |
| `GBAPROF=1` | Per-second profile lines (cpu / render / sound / display) |
| `GBABENCH=1` | Scripted benchmark with frame hash (needs the `perfmon` component) |

Put ROMs in `/sd/roms/gba/`.

## Credits

- **[pjcau](https://github.com/pjcau)**: the Xtensa gpSP dynarec, `xjit`, the line renderer and the
  Retro-Go integration, from
  [xtensa-68000-dynarec](https://github.com/pjcau/xtensa-68000-dynarec) and
  [retro-go](https://github.com/pjcau/retro-go). This app builds on that work.
- **gbsp / gpSP team**: **Exophase** (original gpSP), **notaz**
  ([gpsp](https://github.com/notaz/gpsp), dynarec and platform work), **davidgfnet**
  ([gpsp libretro](https://github.com/davidgfnet/gpsp), current maintainer), and the libretro
  contributors of `gbsp-libretro`.
- **[Retro-Go](https://github.com/ducalex/retro-go)** by ducalex and contributors, and the
  retro-go-pro fork this app is integrated into.
- **FlexE** (Xtensa JIT/emulator project), used as an architecture reference only.

## License

`components/gbsp-libretro` is GPL-2.0 (see its `COPYING`). `components/xjit` carries its own
`COPYING`. Follow the license of each directory when redistributing.
