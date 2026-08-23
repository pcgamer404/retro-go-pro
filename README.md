# Retro-Go Pro

A custom handheld build based on [ducalex/retro-go](https://github.com/ducalex/retro-go), the open-source firmware powering most ESP32 retro handhelds. This fork adds a built-in ROM store, extra emulator cores and ports, and a handful of quality-of-life control/UI tweaks.

For the full feature set, build system, supported hardware list, and general documentation, see the original repo: **https://github.com/ducalex/retro-go**

## Highlights

- Built-in **ROM Store** — browse and download games directly on-device
- Extra **turbo-fire** and **combo button** controls
- Simplified **ROM art** placement (drop art right next to your ROMs)
- Additional emulator cores and game ports beyond stock retro-go

## Supported Systems

**Nintendo** — NES, SNES *(slow)*, GB, GBC, GBA *(slow)*, Game & Watch

**Sega** — Game Gear, Master System, Genesis, SG-1000

**Atari** — Lynx, 2600

**Other** — MSX, PC Engine, ColecoVision, WonderSwan, Neo Geo Pocket (+ Color), Commodore 64, PICO-8 *(slow)*

## Ports

DOOM, Duke3D-Go, Celeste, OpenLara, Wolf4SDL, Cannonball, ClassiCube, SM64-Go

> **Note:** Ports require you to provide your own legally-obtained game files. None are included.

## Additional Features

### ROM Store

Browse and download ROMs directly from the device menu.

1. Set up your own backend using [pcgamer404/RG-Store-Backend](https://github.com/pcgamer404/RG-Store-Backend)
2. Set your store URL in `components/targets/<device>.cfg`:
   ```c
   #define RG_STORE_BASE_URL "https://your-store-url.vercel.app/roms"
   ```

### Minor Improvements

- **Select + Start** opens the menu
- Added **turbo** A (X) and B (Y) buttons for NES games
- **ROM art** can now be placed directly in the `roms` folder alongside the ROM

  **Priority order:** `roms folder` → `romart folder` → CRC-based art

  Art file names must match the ROM file name (e.g. `game1.nes` → `game1.png`)

  For romart dimensions/format specs, see [ducalex/retro-go](https://github.com/ducalex/retro-go)

## Credits

- [ducalex](https://github.com/ducalex) and contributors — original [retro-go](https://github.com/ducalex/retro-go) firmware
- [pcgamer404](https://github.com/pcgamer404) — [RG-Store-Backend](https://github.com/pcgamer404/RG-Store-Backend)
- All upstream emulator core and port authors