# Commodore Amiga Core for Retro-Go (`amiga-go`)

An optimized, feature-rich Commodore Amiga (A500 / A2000 / A600 / A1200) emulator core for **Retro-Go**, running on the ESP32-S3 with 8 MB Octal PSRAM.

Based on the Amiga core from **[hagbardZ/retro-go (Martendo32)](https://github.com/hagbardZ/retro-go/tree/Martendo32/amiga-go)**, **[rlucencic/esp32ami](https://github.com/rlucencic/esp32ami)**, and Karl Stenerud's **Musashi 68000** CPU engine, integrated, hardened, and optimized for `retro-go-pro`.

---

## Features

### Emulation & Hardware Support
- **Models Supported:**
  - **A500** *(Default)*: OCS, 512 KB Chip RAM + 512 KB Slow RAM (Kickstart 1.3). Highest compatibility with classic 1985–1992 floppy games.
  - **A2000**: OCS, 512 KB Chip RAM + 1024 KB Fast RAM.
  - **A600**: ECS, 1024 KB Chip RAM (Kickstart 2.04).
  - **A1200**: AGA, 2048 KB Chip RAM (Kickstart 3.1).
- **Disk & Storage Formats:**
  - `.adf` / `.adz` (Amiga Disk Files)
  - `.zip` (Direct automatic extraction to RAM cache on launch)
  - `.rp9` (RetroPlatform disk packages with automatic disk layout)
  - `.hdf` (Amiga Hard Disk images with Gayle IDE emulation)
- **Dual Floppy Drives (DF0 & DF1):**
  - Hot-swap disks in both DF0 and DF1 at any time via the in-game options menu (`START + B`).
  - Automatic bootblock validation: informs you if an inserted disk is a bootable game disk or an expansion/data disk.
- **Configurable Floppy Speed:**
  - `8x (Turbo)` *(Default)*: Accelerates floppy track reading, reducing loading times from minutes to seconds.
  - `1x (Authentic)`: For sensitive copy-protected or tight custom MFM trackloaders.
  - `2x (Quick)` & `4x (Fast)` intermediate modes.

### Performance Optimizations (30 FPS in 3D Games)
- **Zero PSRAM Opcode Dispatch Latency:** Opcode jump table is retained in Flash `.rodata`, cached by the LX7 hardware cache instead of incurring external Octal SPI PSRAM bus latency on every instruction.
- **0-Wait-State Internal SRAM Cycles Table:** The 64 KB CPU cycle lookup table is allocated directly in Fast Internal SRAM (`DIRAM`), eliminating CPU wait states.
- **Fast Bus Timing:** Bypasses cycle-exact bus-slot wait math (`s_fast_bus`), significantly reducing emulation time.
- **Fast / Immediate Blitter:** Blitter operations finish immediately, dramatically accelerating scrolling and sprite fills.
- **Scaled Line Deduplication:** Repeated scanlines during 200 $\to$ 240 scaling are copied via 32-bit word-aligned `memcpy`, saving 1.2 ms per frame.
- For detailed technical benchmarks and profiling data, see [PERFORMANCE_NOTES.md](PERFORMANCE_NOTES.md).

### Universal Controls & Cracktro Bypass
- **Universal Cracktro Bypass:** On real Amigas, the mouse was in Port 1 (`$bfe001` bit 6) and the joystick in Port 2 (`$bfe001` bit 7). Most cracktros and trainer screens wait for Left Mouse Click. In `amiga-go`, **Button A** and **START** fire **BOTH Joystick Fire and Left Mouse Button**, instantly bypassing cracktros!
- **Space Bar on Face Button X:** Essential for Amiga intros ("Press Space to Continue") and in-game special weapons (e.g. *Turrican* wheel/lightning line, *Battle Squadron* nova bomb, *Lotus 2* horn).
- **Return / Enter on Face Button Y:** Easily advance menus and title screens.
- **Quick Trainer Shortcuts (Hold SELECT + D-Pad):**
  - `SELECT + UP` $\to$ Key **`1`** (e.g. Toggle Unlimited Lives)
  - `SELECT + RIGHT` $\to$ Key **`2`**
  - `SELECT + DOWN` $\to$ Key **`3`**
  - `SELECT + LEFT` $\to$ Key **`4`**
  - `SELECT + B` $\to$ Key **`Y`** (Yes)
  - `SELECT + X` $\to$ Key **`N`** (No)
  - `SELECT` alone $\to$ Space bar
- **Interactive On-Screen Virtual Keyboard:**
  - Press `SELECT + START` or `SELECT + Y` at any time to open the full virtual keyboard.
  - Type letters, numbers, symbols, space, and backspace for high scores and character creation.
- **Quick Control Mode Switch:** Press `SELECT + A` to toggle between **Joystick Mode**, **Mouse Mode**, and **Both Mode** on the fly.

### Display & Screen Fit
- **Auto Active Window Detection:** Dynamically monitors hardware display registers (`DIWSTRT` / `DIWSTOP`) to detect active game lines and eliminate black bars.
- **Configurable Screen Fit Modes:**
  - `Auto (Detect)`: Automatic active DIW detection (e.g. 200 lines for *Lotus 2* stretched to 240 full screen).
  - `Fit 200 (Full/Lotus)`: 200 lines stretched to 240.
  - `Fit 216 (PAL Std)`: Standard PAL 216 lines.
  - `Fit 240 (1:1 Native)`: 1:1 pixel mapping with no vertical scaling.
  - `Fit 256 (Full PAL)`: Full 256 PAL frame compressed to 240.
  - `Crop 200 (Centered)`: 200 lines centered 1:1 with top/bottom black borders.
- **Vertical Offset Tuning:** Shift the picture vertically by `-12` to `+12` lines.

---

## Directory & File Placement on SD Card

### 1. Kickstart BIOS ROMs
Place Kickstart ROM files in:
```
/sd/retro-go/bios/
```
*(Fallback search location: `/sd/roms/amiga/`)*

Supported Kickstart ROM filenames (automatically detected):
- `kick13.rom` or `kickstart.rom` (Kickstart 1.3 - 256 KB, **recommended for 95% of A500 games**)
- `amiga-os-130.rom`
- `amiga-os-204.rom` (Kickstart 2.04 - 512 KB, for A600 / A2000)
- `amiga-os-310-a1200.rom` (Kickstart 3.1 - 512 KB, for A1200)
- `aros.rom` and `aros-ext.rom` (Open-source AROS Kickstart)

### 2. Disk Images & Games
Place your floppy and hard disk images in:
```
/sd/roms/amiga/
```
Supported formats:
- `.adf` (Standard Amiga Floppy Disk)
- `.zip` (Compressed ADF archives - auto-extracted on launch)
- `.rp9` (RetroPlatform disk packages)
- `.hdf` (Hard disk images)

---

## Controls Reference

### Gameplay Controls
| Physical Button | Joystick Mode | Mouse Mode |
|:---|:---|:---|
| **D-Pad** | Amiga Joystick Directions | Mouse Cursor Movement |
| **A** | **Joystick Fire 1 + Left Mouse Click** | Left Mouse Click |
| **B** | **Joystick Fire 1 + Right Mouse Click** | Right Mouse Click |
| **X** | **Amiga Space Bar** (+ Fire 2) | Space Bar (+ 2x Turbo Speed) |
| **Y** | **Amiga Return / Enter** | Return (+ 2x Turbo Speed) |
| **START** | **Universal Start / Skip** (Left Click + Fire + Return + Space) | Universal Start / Skip |
| **SELECT** | Amiga Space Bar / Trainer Modifier | Amiga Space Bar / Trainer Modifier |
| **L Shoulder** | Dedicated Left Mouse Click | Dedicated Left Mouse Click |
| **R Shoulder** | Dedicated Right Mouse Click | Dedicated Right Mouse Click |

### Special Shortcuts & Chords
| Shortcut | Action |
|:---|:---|
| **SELECT + A** | **Quick Toggle Control Mode** (Joystick ↔ Mouse) |
| **SELECT + START** | **Open Virtual Keyboard** |
| **SELECT + Y** | **Open Virtual Keyboard** |
| **SELECT + UP** | Type key **`1`** (Trainer option 1 / 1 Player) |
| **SELECT + RIGHT** | Type key **`2`** (Trainer option 2 / 2 Players) |
| **SELECT + DOWN** | Type key **`3`** (Trainer option 3) |
| **SELECT + LEFT** | Type key **`4`** (Trainer option 4) |
| **SELECT + B** | Type key **`Y`** (Yes) |
| **SELECT + X** | Type key **`N`** (No) |
| **START + B** | **In-Game Options Menu** (Disk Swap, Speed, Fit, etc.) |
| **START + SELECT** | **Retro-Go System Menu** |

### Virtual Keyboard Controls
| Button | Action |
|:---|:---|
| **D-Pad** | Navigate keyboard cursor |
| **A** | Type highlighted character |
| **X / Y** | Backspace (`\b`) |
| **START** | Enter / Return (`\n`) |
| **SELECT** | Space (` `) |
| **B / MENU** | Close Virtual Keyboard |

---

## In-Game Options Menu (`START + B`)

Press **START + B** (or open Options from the Game Menu) to configure:

1. **Control Mode**: `Joystick`, `Mouse`, or `Both`.
2. **Mouse Speed**: `1x (Slow)`, `2x (Normal)`, `3x (Fast)`, `5x (Turbo)`.
3. **Floppy Speed**: `1x (Authentic)`, `2x (Quick)`, `4x (Fast)`, `8x (Turbo)`.
4. **Frameskip**: `Auto`, `Off (0)`, `1`, `2`.
5. **Blitter**: `Fast / Immediate` vs `Authentic`.
6. **Bus Timing**: `Fast / Turbo` vs `Authentic`.
7. **Screen Fit**: `Auto (Detect)`, `Fit 200`, `Fit 216`, `Fit 240`, `Fit 256`, `Crop 200`.
8. **Vertical Offset**: `-12` to `+12` lines.
9. **Virtual Keyboard**: Launch interactive on-screen keyboard.
10. **Model (restart)**: Switch between `A500`, `A2000`, `A600`, and `A1200`.
11. **Disk 1 (DF0)**: Insert disk image into floppy drive DF0.
12. **Eject DF0**: Eject disk from drive DF0.
13. **Disk 2 (DF1)**: Insert disk image into floppy drive DF1.
14. **Eject DF1**: Eject disk from drive DF1.

---

## Machine Model Profiles

| Model | Chipset | Chip RAM | Slow RAM | Fast RAM | Notes |
|:---|:---|:---|:---|:---|:---|
| **A500** *(Default)* | OCS | 512 KB | 512 KB | 0 KB | Ideal for 1985–1992 floppy games |
| **A2000** | OCS | 512 KB | 0 KB | 1024 KB | Standard A2000 setup |
| **A600** | ECS | 1024 KB | 0 KB | 0 KB | ECS games and 1MB chip titles |
| **A1200** | ECS/AGA | 2048 KB | 0 KB | 0 KB | AGA 2MB chip titles |

---

## The "Hand Holding Floppy Disk with Amiga v1.3" Screen

When you see the animated hand holding an `Amiga v1.3` floppy diskette:
- The emulator is **not stuck or frozen**. This is the authentic Kickstart 1.3 BIOS boot screen waiting for a bootable disk in drive `DF0:`.
- If an expansion disk labeled `[data disk]`, `scenery`, `course`, or `Disk 2` was selected, Kickstart 1.3 rejects it because it lacks an executable bootblock. Insert `Disk 1` of the game into DF0.

---

## Building

From the `cores/` repository root:
```bash
python rg_tool.py build-core amiga-go
```

The compiled binary will be automatically staged to:
```
cores/amiga-go.bin
```

---

## Credits & License

- **Port & Optimizations for Retro-Go:** Antigravity AI Assistant & Retro-Go contributors.
- **Amiga Core Engine:** Hagbard Celine ([hagbardZ/retro-go (Martendo32)](https://github.com/hagbardZ/retro-go/tree/Martendo32/amiga-go)) & Rok Lucencic ([esp32ami](https://github.com/rlucencic/esp32ami)).
- **Musashi 68000 CPU Emulator:** Karl Stenerud (MIT License).
- **Retro-Go Platform:** ducalex and the Retro-Go open-source community.
