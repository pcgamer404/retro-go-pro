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

DOOM, Wolfenstein 3D, Duke Nukem 3D, Celeste Classic (PICO-8 port), Outrun, ClassiCube (open-source Minecraft clone/port), OpenLara (Tomb Raider), Super Mario 64 *(very slow)*

> **Note:** Ports require you to provide your own legally-obtained game files. None are included.

## Additional Features

### ROM Store

Browse and download ROMs directly from the device menu.

1. Set up your own backend using [pcgamer404/RG-Store-Backend](https://github.com/pcgamer404/RG-Store-Backend)
2. Set your store URL in `components/targets/<device>.cfg`:
   ```c
   #define RG_STORE_BASE_URL "https://your-store-url.example.com"
   ```

### Minor Improvements

- **Select + Start** opens the menu
- Added **turbo** A (X) and B (Y) buttons for NES games
- **ROM art** can now be placed directly in the `roms` folder alongside the ROM

  **Priority order:** `roms folder` → `romart folder` → CRC-based art

  Art file names must match the ROM file name (e.g. `game1.nes` → `game1.png`)

  For romart dimensions/format specs, see [ducalex/retro-go](https://github.com/ducalex/retro-go)

## Hardware

### Pinout — Buttons

Configured in `components/retro-go/targets/<device>/config.h`:

| Button | GPIO | Pull-up | Active Level |
|---|---|---|---|
| Up | GPIO_NUM_8 | Yes | LOW |
| Down | GPIO_NUM_6 | Yes | LOW |
| Left | GPIO_NUM_7 | Yes | LOW |
| Right | GPIO_NUM_15 | Yes | LOW |
| Select | GPIO_NUM_5 | Yes | LOW |
| Start | GPIO_NUM_2 | Yes | LOW |
| A | GPIO_NUM_47 | Yes | LOW |
| B | GPIO_NUM_40 | Yes | LOW |
| Y | GPIO_NUM_21 | Yes | LOW |
| X | GPIO_NUM_1 | Yes | LOW |

### Pinout — Display (SPI)

Also in `components/retro-go/targets/<device>/config.h`:

| Signal | GPIO |
|---|---|
| MISO | Not connected (-1) |
| MOSI | GPIO_NUM_13 |
| CLK | GPIO_NUM_12 |
| CS | GPIO_NUM_10 |
| DC | GPIO_NUM_11 |
| Backlight | GPIO_NUM_14 |
| RST | Not connected (-1) |

### Pinout — SD Card (SPI)

Also in `components/retro-go/targets/<device>/config.h`:

| Signal | GPIO |
|---|---|
| MISO | GPIO_NUM_9 |
| MOSI | GPIO_NUM_13 |
| CLK | GPIO_NUM_12 |
| CS | GPIO_NUM_18 |

> **Note:** Audio and battery-status GPIO are **disabled by default** and must be enabled/configured manually in the same `config.h` for your specific board.

## Building & Flashing

Built with **ESP-IDF v5.3.5**, using retro-go's own build tool, `rg_tool.py`, which wraps `idf.py` to handle multi-app builds/flashing correctly (`idf.py` alone cannot manage this project's multi-app structure). Run from the project root.

You can set your port and target once as environment variables (`RG_TOOL_PORT`, `RG_TOOL_TARGET`) instead of passing `--port`/`--target` every time — see `python rg_tool.py --help` for all flags.

### Default build apps

`nes`, `gb`, `gbc`, `gw` (Game & Watch), `gg` (Game Gear), `sms` (Master System), `lynx`, `colecovision`, `pce` (PC Engine), `celeste`, `openlara`, `cannonball`, `classicube`, `ngp-go` (Neo Geo Pocket)

### Build commands

**Build everything into a flashable `.img` (serial flash):**
```bash
python rg_tool.py --target=my-handheld build-img
```

**Build everything into a `.fw` (SD-card firmware update file):**
```bash
python rg_tool.py --target=my-handheld build-fw
```

**Build only specific apps** (e.g. launcher + NES + DOOM):
```bash
python rg_tool.py --target=my-handheld build-img launcher nes doom
```

**Clean build (recommended before a release):**
```bash
python rg_tool.py --target=my-handheld release
```

### Flashing

**Flash a single app during development** (fast — skips full repackaging):
```bash
python rg_tool.py --target=my-handheld --port=COM3 flash nes
```

**Flash then immediately open serial monitor:**
```bash
python rg_tool.py --target=my-handheld --port=COM3 run nes
```

**Flash multiple apps in one go:**
```bash
python rg_tool.py --target=my-handheld --port=COM3 flash launcher nes doom celeste
```

**Flash a full built `.img` file directly with esptool.py** (e.g. after `build-img`):
```bash
esptool.py --chip esp32s3 --port COM3 --baud 921600 write_flash --flash_size detect 0x0 build/retro-go_my-handheld.img
```

> On Windows, `./rg_tool.py ...` may invoke the wrong Python interpreter — use `python rg_tool.py ...` instead.

## Credits

- [ducalex](https://github.com/ducalex) and contributors — original [retro-go](https://github.com/ducalex/retro-go) firmware
- [pcgamer404](https://github.com/pcgamer404) — [RG-Store-Backend](https://github.com/pcgamer404/RG-Store-Backend)
- All upstream emulator core and port authors