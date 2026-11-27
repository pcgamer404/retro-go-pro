# Castlevania: Symphony of the Night for Retro-Go (`sotn-go`)

Native ESP32-S3 Retro-Go port of **Castlevania: Symphony of the Night**, built directly on the C decompilation (`sotn-decomp-esp32-port`) and the **psyz** software rasterization layer.

This port runs natively on bare-metal ESP32-S3 without emulation. It integrates the decompilation core into Retro-Go's system architecture, supporting target displays (320x240 SPI LCD), gamepads/keymaps, power management, storage, audio mixing, and in-game options menus.

---

## 1. System Requirements & Target Hardware

- **SoC:** ESP32-S3 (Dual-core Xtensa LX7 @ 240 MHz).
- **RAM:** 8 MB Octal SPI PSRAM (`0x3C000000`).
- **Storage:** 16 MB SPI Flash + MicroSD card formatted to FAT32 / exFAT.
- **Display:** 320x240 ST7789 / ILI9341 SPI LCD with DMA.
- **ESP-IDF:** v5.3.5 or compatible.

---

## 2. Controls & Keybindings

| Button | Action / PSX Function |
| :--- | :--- |
| **D-Pad** | D-Pad (Directional Movement) |
| **A** | Cross (Jump / Confirm) |
| **B** | Square (Attack / Main Weapon) |
| **X** | Circle (Subweapon / Special) |
| **Y** | Triangle (Backdash) |
| **L / R** | L1 / R1 (Mist / Bat Transform) |
| **SELECT + L / R** | L2 / R2 (Wolf Transform / Chords) |
| **START** | Start (Pause / Inventory Menu) |
| **SELECT** | Select (Map Screen) |
| **MENU** *(hold ~0.45 s)* | Retro-Go Game Menu (Resume, Reset, Quit) |
| **OPTION** *(hold ~0.45 s)* | Retro-Go Options Menu (Scaling, Volume, Backlight) |

*Note: Short taps on Menu/Option are filtered to prevent accidental pauses during gameplay.*

---

## 3. Directory Layout & SD Card Assets

The core expects game assets placed under `/sd/roms/sotn/`:

```text
/sd/roms/sotn/
├── stage.txt                  (Stage boot descriptor: "nz0" or "boot")
├── disks/
│   └── us/
│       ├── BIN/               (F_GAME.BIN, F_TITLE1.BIN, etc.)
│       ├── BOSS/              (Boss overlays and scripts)
│       ├── DRA/               (DRA.BIN core archive)
│       ├── ST/
│       │   ├── NP3/           (Castle Entrance)
│       │   ├── NZ0/           (Alchemy Laboratory)
│       │   ├── SEL/           (Title & File Select)
│       │   └── WRP/           (Warp Rooms)
│       └── VAB/               (Sound banks and effects)
└── sotn.us.cue                (Optional: CD-DA soundtrack CUE + BIN)
```

- **Quick Boot:** Set `stage.txt` to `nz0` to boot directly into the Alchemy Laboratory gameplay, skipping intro sequences. Set to `boot` or leave empty for normal title/menu sequence.
- **Memory Card:** Game saves are written directly to virtual PSX memory card blocks on the SD card under `roms/sotn/`.

---

## 4. Architecture & Technical Highlights

### A. Dual-Core Pipeline
- **Core 0 (Game Logic & Rasterizer):** Executes the decompiled game loop, enemy AI, physics, stage streaming, and the `psyz` software triangle/sprite rasterizer writing directly to 1024x512 RGB5551 PSX VRAM.
- **Core 1 (Display & Audio Service):** Handles asynchronous VRAM conversion and scaling, FreeRTOS FreeAudio / SPU mixer audio tasks, and 80 MHz SPI DMA display transmission.

### B. Optimized Frame Scaling Pipeline (`rg_lcd.c`)
- **Double-Buffered PSRAM Surfaces:** Eliminates frame tearing and drops caused by LCD transmission stalls.
- **Fast 4:5 Gameplay Scaling (256x240 $\to$ 320x240):** An unrolled sequential streaming pipeline in `IRAM_ATTR` reads 32-bit PSRAM words and writes 32-bit destination pixels, dropping conversion time from 10 ms to < 0.5 ms per frame.
- **Fast 6:5 Menu Downscaling (384x240 $\to$ 320x240):** A dedicated 32-bit unrolled streaming path avoids memory lookup tables on 384-width menus.

### C. Software Rasterizer Optimizations (`soft_raster.inc.c`)
- **8bpp Texture Fast-Path:** Employs aligned 32-bit pixel stores and halfword texture reads to halve PSRAM bus traffic during sprite rendering.
- **Texture Row Hoisting:** Hoists texture page math and clipping logic outside the inner raster loops.

---

## 5. Performance Notes & Telemetry

- **Target Framerate:** ~30 to 35 FPS during active combat and stage exploration.
- **Menu / UI Framerate:** ~60 FPS.
- **Audio Output:** SPU sound effects mixing at 44.1 kHz resampled to device audio sink.

---

## 6. Known Limitations

1. **Name Entry Alphabet Display:** Character selection and cursor navigation in the Name Entry screen are functional and allow proceeding to gameplay, but the 16x16 font atlas rendering on this screen is currently not displayed.
2. **Linked Stages:** NZ0 (Alchemy Laboratory), NP3 (Castle Entrance), WRP (Warp Rooms), and SEL (Title/Select) are linked in this binary build. Entering non-linked stages halts at stage loading.
3. **Save States:** Save states are not supported due to the 8 MB PSRAM heap footprint; standard PSX in-game save rooms / memory cards are used.

---

## 7. Build Instructions

To build the core using Retro-Go tools:

```bash
# In the retro-go cores directory:
python rg_tool.py build-core sotn-go --target my-handheld
```

The resulting staged binary will be generated at:
`cores/cores/sotn-go.bin` (~2.1 MB).
