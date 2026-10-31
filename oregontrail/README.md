# Oregon Trail — ESP32-S3 / Retro-Go

A native C++ reimplementation of the classic **Oregon Trail**, adapted to run as a Retro-Go application on ESP32-S3 handheld hardware.

The project is based on the original [BruteSource/oregontrail-s3](https://github.com/BruteSource/oregontrail-s3) project, but the current version has been adapted from the original Arduino/PlatformIO + LovyanGFX application to a **native Retro-Go application**.

## Hardware

- ESP32-S3 N16R8
- 16 MB Flash
- 8 MB Octal PSRAM
- 320×240 ILI9341 SPI display
- microSD
- Retro-Go custom target: `RG_TARGET_MY_HANDHELD`

The application runs in landscape at **320×240**.

## What is included

The game is a native application, not a DOS/Windows emulator. It includes the Oregon Trail game flow, travel, events, river crossings, hunting, stores, landmarks, scoring, saves, music and artwork support.

### Quality-of-life improvements for the handheld

- Native D-pad navigation instead of relying on touchscreen interaction.
- A/B controls for selection and back/navigation.
- Left/right D-pad support on screens where it is useful, including stores and choice screens.
- Visible selection highlight on the preset-name picker.
- Double-buffered rendering to eliminate the previous display flicker.
- Cached/pre-converted artwork for faster repeated rendering.
- Faster integer-scaled rendering for common artwork paths.
- Native Retro-Go storage/settings integration.
- Game save/resume support.
- Retro-Go launcher integration.

## SD-card files

The game expects its data under:

```text
/roms/oregontrail/
```

The supplied project contains an **empty `OregonTrail.trail` file** at:

```text
sd/roms/oregontrail/OregonTrail.trail
```

Keep this file in the directory. It acts as the launcher entry/marker for the application; it does not contain the game data itself.

The required SD-card layout is:

```text
/roms/oregontrail/
├── OregonTrail.trail          # empty launcher file
├── art/
│   ├── banner.png
│   ├── family.png
│   ├── map.png
│   ├── tombstone.png
│   ├── landmarks/
│   │   ├── p0.png ... p17.png
│   └── sprites/
│       ├── animals/            # 01.png ... 48.png
│       ├── events/             # 01.png ... 07.png
│       ├── hunter/             # 01.png ... 24.png
│       ├── scenery/            # 01.png ... 17.png
│       ├── terrain/            # 01.png ... 14.png
│       └── travelox/           # 01.png ... 05.png
└── music/
    ├── tombstone.json
    └── landmarks/
        └── 18 landmark .json music files
```

The landmark music loader reads the JSON files from `music/landmarks/` and requires 18 valid songs. The artwork paths are used directly by the native renderer.

## Building

Build from the Retro-Go project root using the ESP-IDF environment:

```text
idf.py app -DRG_PROJECT_APP=oregontrail -DRG_PROJECT_VER=1.4x-MEGAPACK-24-gc61ca-dirty -DRG_BUILD_TARGET=RG_TARGET_MY_HANDHELD -DRG_BUILD_RELEASE=0 -DRG_ENABLE_PROFILING=0 -DRG_ENABLE_NETWORKING=1
```

The Oregon Trail application is located at:

```text
oregontrail/
```

and integrates with the Retro-Go component in:

```text
components/retro-go/
```

## Project structure

```text
oregontrail/
├── main/                       # Retro-Go application entry point
├── components/oregontrail/
│   ├── game/                   # Trail, simulation, events, rivers, store, scoring
│   ├── screens/                # Game screens and menus
│   ├── ui/                     # Screen stack, widgets and theme
│   ├── hw/                     # Retro-Go input, audio, storage and battery adapters
│   ├── compat/                 # Arduino/LovyanGFX compatibility layer
│   ├── art/                    # Artwork interface/helpers
│   ├── Music.cpp/.h             # Landmark and tombstone music loading
│   ├── SaveGame.cpp/.h          # Journey save/resume
│   └── AssetPaths.cpp/.h        # /roms/oregontrail data paths
└── sd/roms/oregontrail/
    └── OregonTrail.trail       # empty launcher file
```

## Artwork and music

The renderer uses Retro-Go's existing LodePNG implementation and preserves RGBA artwork support. Artwork loaded from the SD card is cached and converted for the 16-bit display so repeated drawing is cheaper.

The project does not require a second copy of LodePNG; Retro-Go supplies the decoder.

## Credits / references

This project is an inspired-by reimplementation of Oregon Trail. The original project and development direction can be found at:

- https://github.com/BruteSource/oregontrail-s3
- https://github.com/Maxwolf/OregonTrail
- https://github.com/ducalex/retro-go

Original MECC/Oregon Trail artwork and game concepts remain the property of their respective rights holders. This repository is intended as an educational/homebrew project.
