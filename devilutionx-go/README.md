# DevilutionX-Go (Diablo 1 Port for Retro-Go / ESP32-S3)

An optimized port of **DevilutionX** (Diablo 1 source port) running on the **ESP32-S3** microcontroller under the **Retro-Go** launcher framework.

Experience the dark fantasy world of Sanctuary on a portable, handheld retro console!

---

## Features & Highlights

- **Complete Diablo 1 Campaign**: Explore Tristram, descend through 16 dungeon floors, defeat the Skeleton King, the Butcher, Archbishop Lazarus, and Diablo himself.
- **Engineered for 8MB PSRAM**:
  - On-demand missile sprite loading (saves ~1.2 MB PSRAM).
  - Portrait animation deduplication (saves ~350 KB PSRAM).
  - Single-threaded town asset initialization with robust event loop pacing.
- **Custom Handheld Control Scheme**:
  - Tailored specifically for handhelds with **no L/R shoulder buttons**.
  - Smart **SELECT Modifier Combos** allow instant belt potion drinking (Health & Mana), quick panel toggling (Inventory, Character Sheet, Spellbook, Quests), and automap cycling.
- **Crisp & Sharp Text Rendering**:
  - Diablo 1 natively renders at 640x480.
  - Custom font emboldening ensures all 1px font stems and crossbars survive the 2:1 point decimation downscaling to 320x240 LCD screens with zero missing strokes.
- **Dual-Core Render Pipeline**:
  - Core 0 executes game simulation and software rasterization into a private atomic backbuffer.
  - Core 1 handles display downsampling and high-speed SPI/DMA LCD transmission.
  - Zero-tearing presentation with eliminated redundant frame limiter yield loops.

---

## Hardware Requirements

| Component | Specification |
| :--- | :--- |
| **SoC** | ESP32-S3 (Dual-core Xtensa LX7 @ 240 MHz) |
| **PSRAM** | 8 MB Octal PSRAM (OPI) *— mandatory* |
| **Flash** | 16 MB SPI Flash |
| **Display** | 320x240 SPI LCD (e.g. ST7789, ILI9341) |
| **Controls** | D-Pad, A, B, X, Y, START, SELECT *(L/R shoulder buttons optional / not required)* |
| **Storage** | MicroSD card formatted as FAT32 |

---

## Performance & Optimization Notes

- **Simulation Rate**: Native Diablo 1's engine runs at **20 ticks per second** (50ms per simulation frame, matching Blizzard's original engine timing).
- **Framerate**: Runs between **15–20 FPS** in both Tristram village and multi-monster dungeon floors.
- **Memory Safety**: Free PSRAM typically hovers around ~2.5 MB to 3.0 MB in Tristram, ensuring ample headroom for spell casting, animations, and sound buffers.
- **Atomic Display Sync**: Double-buffering prevents screen tearing and lower-half flicker that can occur when the display task reads PSRAM concurrently with software rasterization.

---

## Current Status & Limitations

### Working:
- [x] Full single-player campaign (all quests, town, cathedral, catacombs, caves, and hell)
- [x] Character classes (Warrior, Rogue, Sorcerer)
- [x] Saving and loading games
- [x] Spells, scrolls, staves, shrines, and missile effects
- [x] Inventory, equipment, and belt items
- [x] Full townspeople interactions, speech, and stores (Griswold, Adria, Pepin, Wirt, Cain)
- [x] Dungeon Automap / Minimap

### Limitations / Missing:
- **Single-Player Only**: Multiplayer networking (ZeroTier, TCP/IP, modem) is stripped out to fit embedded memory constraints.
- **Base Diablo 1 Only**: Hellfire expansion support is disabled to conserve flash memory and RAM.
- **Cinematic Cutscenes Skipped**: Smacker (.smk) video movies are disabled.
- **Sound**: Audio support depends on the Retro-Go board configuration (e.g., I2S DAC, PWM, or USB DAC).

---

## SD Card Setup & Folder Structure

Ensure your MicroSD card is formatted as **FAT32**.

### Folder Layout:
```
SD Card Root (/)
│
├── roms/
     └── diablo/
          ├── DIABDAT.MPQ            <-- Diablo 1 game data (Retail or Shareware)
          ├── devilutionx.mpq        <-- Engine assets & fonts (bundled)
                └── devilutionx.png        <-- Box art preview (optional)


```

> [!IMPORTANT]
> The filename **`DIABDAT.MPQ`** must be in **UPPERCASE** on systems with case-sensitive filesystem checks.

---

## How to Get the Required Files

### 1. `devilutionx.mpq` (Engine Assets)
- **Included in this repository**: You can grab `devilutionx.mpq` directly from this repository root or from the GitHub Releases page.
- **Or from official DevilutionX releases**: Download any release of [DevilutionX](https://github.com/diasurgical/devilutionX/releases) (Windows, Linux, Switch, 3DS, etc.) and copy `devilutionx.mpq`.

### 2. `DIABDAT.MPQ` (Diablo 1 Game Data)
You need the original game assets to play:
- **Digital Release**: Purchase Diablo 1 from [GOG.com](https://www.gog.com/game/diablo). After installation, copy `DIABDAT.MPQ` from the game directory to `/sd/roms/diablo/`.
- **Original CD**: Copy `DIABDAT.MPQ` from your original Blizzard Diablo 1 retail CD-ROM.
- **Free Shareware Demo**: Download `spawn.mpq` (the free Diablo Shareware demo released by Blizzard) and place it at `/sd/roms/diablo/spawn.mpq` (or rename it to `DIABDAT.MPQ`). This lets you play the first 2 dungeon levels as the Warrior.

---

## Controls Reference Guide

Configured specifically for consoles without shoulder buttons:

### Direct Gameplay Controls
| Button | In-Game Action | Menu / UI Action |
| :--- | :--- | :--- |
| **D-Pad** | Walk / Run / Aim | Navigate Menus / Move Cursor |
| **A** | **Primary Action**: Attack monsters, talk to townspeople | Lift & place inventory items, Confirm menu selection |
| **B** | **Cancel / Back**: Close panels | Go back / Exit sub-menu |
| **X** | **Spell Action**: Cast active spell or skill | - |
| **Y** | **Secondary Action**: Open doors, open chests, search shrines, loot items | - |
| **START** | **Game Menu**: Open Pause / Save / Load / Options menu | Confirm / Close menu |
| **SELECT (tap)** | **Toggle Automap**: Show or hide dungeon minimap | - |

### SELECT Modifier Combos (Hold SELECT + Press Button)
*Holding **SELECT** acts as a modifier button. The on-screen HUD guide will appear automatically:*

| Combo | Action | Description |
| :--- | :--- | :--- |
| **SELECT + B** | **Quick Health Potion** | Instantly drinks a healing potion from your belt |
| **SELECT + Y** | **Quick Mana Potion** | Instantly drinks a mana potion from your belt |
| **SELECT + D-Pad RIGHT** | **Toggle Inventory** | Opens / closes your inventory grid |
| **SELECT + D-Pad LEFT** | **Toggle Character Sheet** | Opens / closes your character stats and attributes |
| **SELECT + A** | **Toggle Spellbook** | Opens / closes the full spellbook |
| **SELECT + X** | **Toggle Quest Log** | Opens / closes the active quests journal |
| **SELECT + D-Pad DOWN** | **Cycle Automap** | Cycles minimap zoom / modes |
| **SELECT + D-Pad UP** | **Game Options** | Quick shortcut to the options menu |

---

## Credits & Acknowledgements

- **[Diasurgical / DevilutionX Team](https://github.com/diasurgical/devilutionX)**: For reverse-engineering, modernizing, and maintaining the DevilutionX source engine.
- **[Retro-Go](https://github.com/ducalex/retro-go) & Retro-Go Pro Team**: For the launcher, driver architecture, and hardware abstraction layer for ESP32 handhelds.
- **Blizzard North**: For creating Diablo (1996), one of the greatest action RPGs of all time.
- Port maintenance and embedded ESP32-S3 optimizations by the Retro-Go community.

---

## License

- **DevilutionX**: Covered by its original license (see `components/devilutionx/engine/LICENSE.md`).
- **Retro-Go**: Covered by the GPL-3.0 / Retro-Go license.
- **Diablo Game Assets**: All original assets (`DIABDAT.MPQ`) remain the intellectual property of Blizzard Entertainment. You must own a legitimate copy of Diablo 1 to use game data.
