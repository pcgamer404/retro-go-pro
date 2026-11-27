# Fallout-Go: Unified Fallout 1 & 2 Port for Retro-Go (ESP32-S3)

**Fallout-Go** is a native, unified port of **Fallout 1 Community Edition** and **Fallout 2 Community Edition** running on the Retro-Go firmware framework for ESP32-S3 handheld consoles (specifically tailored for `my-handheld`: 8MB Octal PSRAM, 16MB Flash, 320x240 SPI LCD, D-Pad, A/B/X/Y, START/SELECT).

Both iconic CRPG engines are combined into a **single binary (`fallout-go.bin`)**, with isolated engine namespaces (`fo1` and `fo2`), custom PSRAM section mapping, and launcher file detection (`Fallout 1.fo` vs `Fallout 2.fo`).

---

## 📁 SD Card Folder Structure

Format your micro SD card as FAT32. Place the game assets and launcher files into `/sd/roms/fallout/` as follows:

```
sdcard/
└── roms/
    └── fallout/
        ├── Fallout 1.fo                     <-- Launcher for Fallout 1
        ├── Fallout 2.fo                     <-- Launcher for Fallout 2
        │
        ├── fallout1/
        │   ├── master.dat                   <-- From Fallout 1 (Steam / GOG / CD)
        │   ├── critter.dat                  <-- From Fallout 1 (Steam / GOG / CD)
        │   └── fallout.cfg                  (Auto-created on first boot)
        │
        └── fallout2/
            ├── master.dat                   <-- From Fallout 2 (Steam / GOG / CD)
            ├── critter.dat                  <-- From Fallout 2 (Steam / GOG / CD)
            ├── patch000.dat                 (Optional: official/unofficial patch)
            └── fallout.cfg                  (Auto-created on first boot)
```

> **Note on Case Sensitivity:** File names like `master.dat` and `critter.dat` can be either lowercase or uppercase (`MASTER.DAT`, `CRITTER.DAT`). The launcher checks both.

---

## 🎮 Controls

Fallout is traditionally a mouse-and-keyboard CRPG. Fallout-Go features a virtual mouse emulation engine designed specifically for handhelds without shoulder buttons:

| Console Button | Fallout Action | PC Equivalent |
|---|---|---|
| **D-Pad (Up/Down/Left/Right)** | Move virtual mouse cursor (dynamic 3-tier acceleration) | Mouse Movement |
| **A** | Primary Action: Interact, Walk, Attack, Talk, Select | Left Mouse Click |
| **B** | Secondary Action: Toggle cursor mode, Examine/Look, Cancel | Right Mouse Click |
| **X** | Inventory screen | `I` Key |
| **Y** | End Turn (Combat) / Confirm dialog | `Spacebar` |
| **START** | Character Sheet / Skills / Perks | `C` Key |
| **SELECT** | In-game Options Menu / Escape | `ESC` Key |
| **HOLD MENU (0.5s)** | Retro-Go System Menu (Audio, Brightness, Exit) | System Menu |

### Cursor Acceleration
- **Short tap:** Precise single-pixel positioning (for targeting small items and hexes).
- **Hold > 150ms:** Medium speed navigation.
- **Hold > 350ms:** Fast speed panning across the screen.

---

## ⚙️ Technical Architecture & Memory Management

Running two full PC CRPG engines on an ESP32-S3 with 512KB SRAM and 8MB PSRAM required extensive memory optimization:

1. **Namespace Isolation:**
   Fallout 1 and Fallout 2 engines share hundreds of identical symbol and function names. They are compiled with distinct namespace wrappers (`fo1` and `fo2`), allowing both engines to coexist in a single executable without symbol conflicts.

2. **PSRAM BSS Placement via Linker Script (`fallout.lf`):**
   - The Fallout engines declare ~3.15 MB of static lookup tables and global state arrays.
   - Internal SRAM on ESP32-S3 only provides ~340KB of DRAM data segment space.
   - A custom linker fragment redirects `libfallout1.a` and `libfallout2.a` `.bss` and `.common` sections into External Octal PSRAM (`.ext_ram.bss`), leaving **over 180KB of internal SRAM free** for FreeRTOS tasks, DMA transfer descriptors, and audio FIFO buffers.

3. **Read-Only Walkmask Optimization:**
   The 200KB `WALKMASK_MASK_DATA` array was placed into `.rodata` in 16MB SPI Flash rather than writable DRAM, saving 200KB of precious internal RAM.

4. **Engine Cache Control:**
   On first launch, `fallout.cfg` is automatically populated with `art_cache_size=2` MB. This caps sprite caching within safe limits, ensuring plenty of headroom for dynamic heap allocations in 8MB PSRAM.

5. **Display Rendering:**
   The engine renders to an internal 640x480 8-bit paletted surface in PSRAM. Retro-Go downscales the frame buffer to 320x240 in real-time on Core 1 using optimized display routines and SPI DMA.

---

## 🔨 How to Build

From the `retro-go-pro/cores` directory:

```bash
# Set up ESP-IDF environment (v5.3)
export IDF_PATH=/path/to/esp-idf
. $IDF_PATH/export.sh

# Build fallout-go core for your handheld target
python rg_tool.py build-core fallout-go --target my-handheld
```

The output binary is staged to:
`retro-go-pro/cores/cores/fallout-go.bin`

Flash to device:
```bash
python rg_tool.py flash fallout-go --target my-handheld
```

---

## 📜 Credits & Acknowledgments

- **Fallout 1 Community Edition (fallout1-ce)**: [alexbatalov/fallout1-ce](https://github.com/alexbatalov/fallout1-ce)
- **Fallout 2 Community Edition (fallout2-ce)**: [alexbatalov/fallout2-ce](https://github.com/alexbatalov/fallout2-ce)
- **Interplay Entertainment & Black Isle Studios**: Original creators of Fallout (1997) and Fallout 2 (1998).
- **Retro-Go Team**: Handheld retro gaming framework for ESP32.
- **Port Implementation**: retro-go-pro / Antigravity pair programming.
