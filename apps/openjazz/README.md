# OpenJazz for Retro-Go (ESP32-S3)

Jazz Jackrabbit 1 running as a Retro-Go app on an ESP32-S3 handheld, using the
[OpenJazz](https://github.com/AlisterT/openjazz) engine.

| Works | Not supported |
|---|---|
| Intro, menus, levels, bonus levels, music and sound effects | Retro-Go save states (use Jazz's own Save/Load in the pause menu) |
| Retro-Go menu (screenshot, scaling, volume, reset, quit) | Network play, movies, Jazz 2 |
| Settings and saves on the SD card | |

## Requirements

- A Retro-Go tree (ESP-IDF 5.3.x) with this folder placed next to `launcher/`
  (the app uses `../base.cmake`).
- ESP32-S3 with PSRAM (a level keeps about 1.3 MB of sprites/tiles in memory).
- The original Jazz Jackrabbit 1 data files (shareware or registered).

## Build and flash

From the Retro-Go root (the folder that contains `rg_tool.py`):

```
python rg_tool.py build openjazz --target my-handheld
python rg_tool.py flash openjazz --target my-handheld --port COM4
```

The app needs a partition named `openjazz` (the image is about 1 MB).

OpenJazz needs C++ exceptions. They are enabled by `sdkconfig` (ready-made) and by
`sdkconfig.defaults`, which `CMakeLists.txt` adds on top of the target's defaults, so
`rg_tool.py clean openjazz` (which deletes `sdkconfig`) is safe.

## SD card

```
/sd/roms/openjazz/                 Jazz 1 data files, directly in this folder
                                   (BLOCKS.000, PANEL.000, MENU.000, SOUNDS.000, *.PSM ...)
/sd/retro-go/config/openjazz/      created automatically: openjazz.cfg and saved games (SAVE.0-3)
```

## Controls

| Button | Action |
|---|---|
| D-pad | Move / menu up-down |
| A | Jump (menus: select) |
| B | Fire |
| X | Change weapon |
| Y | Start (Enter) |
| R | Pause |
| SELECT | Escape: in-level menu / back |
| START | Enter |
| SELECT + START | Retro-Go menu |
| START + B | Retro-Go options |

SELECT and START act when released, so that pressing both for the Retro-Go menu does not
also open Jazz's pause menu. All mappings are in one table in
`openjazz/porting/retrogo_input_adapter.c`.

Jazz's own menu (SELECT) has Save Game and Load Game with four slots.

## Layout

```
CMakeLists.txt              app project (same pattern as the other Retro-Go apps)
sdkconfig, sdkconfig.defaults   C++ exceptions on
main/main.cpp               Retro-Go glue: 32 KB engine task, menu handlers, PSRAM policy, 22.05 kHz audio
openjazz/                   engine component
  porting/                  Retro-Go side
    retrogo_video_adapter.c   8-bit frame + palette -> RGB565 -> rg_display_submit
    retrogo_input_adapter.c   Retro-Go buttons -> SDL key events, opens the Retro-Go menu
    sdl_compat/               small SDL subset: 8-bit surfaces, events, timing, audio
  upstream-openjazz/        OpenJazz sources (GPL-2.0) and libxmp, see "Changes to upstream"
```

## How it works

- OpenJazz draws into 8-bit indexed surfaces. `sdl_compat` implements just the SDL calls it
  uses. Blits between surfaces with different palettes map colours to the nearest match,
  like SDL 1.2; OpenJazz relies on this for highlighted menu text and tinted sprites.
- The screen has two palettes, as in SDL 1.2: a logical one for blits and a physical one for
  the display. Fades and palette animation only touch the physical one.
- Each frame is converted with the physical palette to RGB565 and submitted to Retro-Go.
- Audio uses OpenJazz's own `sound.cpp` (sound effects plus PSM music through libxmp). The
  SDL audio shim runs a small task that calls the engine's audio callback and passes the
  16-bit stereo result to `rg_audio_submit()`. Music is mixed with nearest-neighbour
  interpolation and no DSP, which costs very little CPU.
- `main.cpp` sets `mallocAlwaysInternal = 256`: allocations above 256 bytes go to PSRAM,
  otherwise internal RAM runs out while loading a level.
- The engine runs on its own 32 KB task. The Font loaders use 5-7 KB of stack each, which
  overflows the default 8 KB main task.
- OpenJazz reports file errors with exceptions, which the ESP-IDF turns into `abort()` when
  they are disabled. Empty save slots no longer use an exception at all.

## Changes to upstream OpenJazz

All under `#ifdef OJ_RETROGO`:

| File | Change |
|---|---|
| `src/SDL_wrapper.h` | use the compat headers instead of SDL |
| `src/main.cpp` | `openjazz_main()` entry, SD paths, pump Retro-Go input each frame |
| `src/setup.cpp` | missing config file is a normal first boot, not an exception |
| `src/io/gfx/video.cpp` | indexed frame buffer, present through Retro-Go, no window/cursor/movie code |
| `src/io/file.cpp` | `File::exists()` probes with `fopen` (`access()` is unreliable on the FAT VFS) |
| `src/jj1/save/jj1save.cpp` | check that a save slot exists before opening it |

`src/io/sound.cpp` and `ext/xmp` are unchanged upstream files. Other platforms, Jazz 2,
docs and scale2x are removed.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Reboot loop, "stack overflow in task ..." | engine task stack too small (`OPENJAZZ_TASK_STACK_SIZE` in `main/main.cpp`) |
| `abort()` right after a "Could not open file" warning | exceptions off: check `CONFIG_COMPILER_CXX_EXCEPTIONS=y` in the generated `sdkconfig` |
| Out-of-memory while loading a level | PSRAM not enabled, or `mallocAlwaysInternal` raised |
| Log stops after the first "Opened file" lines or says a file is missing | Jazz data is not directly inside `/sd/roms/openjazz/` |
| No music, sound effects work | `*.PSM` files missing from `/sd/roms/openjazz/` |
| No sound at all | `config.sampleRate` in `main/main.cpp` must match `SOUND_FREQ` in `openjazz/CMakeLists.txt` (22050) |

## License

OpenJazz is GPL-2.0 (`openjazz/upstream-openjazz/COPYING`). Bundled `argparse`, `miniz`,
`stb_rect_pack` and libxmp keep their own licenses in `openjazz/upstream-openjazz/ext/`. The
port code follows Retro-Go's license.
