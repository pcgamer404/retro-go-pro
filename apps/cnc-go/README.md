# Command & Conquer (Vanilla-Conquer) for Retro-Go

This core ports **Vanilla-Conquer (Tiberian Dawn)** to the **Retro-Go** framework for ESP32-S3 handheld devices (`my-handheld`: 16MB Flash, 8MB Octal PSRAM, 320x240 LCD).

---

## 🎮 Game Data Files (Required)

Command & Conquer requires original `.MIX` game data archives from the DOS or Windows 95 ("Gold") release.

Place the files on your MicroSD card in one of the following directories:
- `/sd/roms/cnc/` *(Recommended)*
- `/sd/cnc/`

### Essential Files
```text
/sd/roms/cnc/
 ├── CONQUER.MIX   (Core game assets - required)
 ├── LOCAL.MIX     (DOS UI fonts & language strings - or CCLOCAL.MIX, required)
 ├── TEMPERAT.MIX  (Temperate theater palette and tiles - required)
 ├── DESERT.MIX    (Desert theater palette and tiles)
 ├── WINTER.MIX    (Winter theater palette and tiles)
 ├── GENERAL.MIX   (Scenario definitions & mission data - required)
 ├── SOUNDS.MIX    (Game sound effects)
 ├── SPEECH.MIX    (EVA speech voice lines)
 ├── SCORES.MIX    (Music tracks - optional)
 └── TRANSIT.MIX   (Mission transition assets - optional)
```

> **Note:** File lookup includes case-insensitive fallback, so filenames can be uppercase, lowercase, or mixed-case. If `SCORES.MIX` is omitted, background music is cleanly disabled while sound effects and EVA speech remain active.

---

## 🕹️ Controls

| Gamepad Button | C&C Action | Description |
|---|---|---|
| **D-Pad** | Move Cursor | Smooth cursor acceleration; move left (GDI) or right (NOD) on Team Select |
| **L / R Triggers** | Cursor Turbo | Speed multiplier for quick map & UI navigation |
| **A** | Left Click / Confirm | Select units, issue move/attack orders, build items, confirm team |
| **B** | Right Click / Cancel | Deselect units, cancel placement or production |
| **X** | Force Attack | Ctrl + Click (attack terrain or friendly units) |
| **Y** | Guard Mode | Set selected units to Guard (`G` key) |
| **SELECT** | Toggle Sidebar | Tab key (show/hide command and build sidebar) |
| **START** | Enter / Options | Confirm dialogs / open in-game options |
| **MENU** | Retro-Go Menu | System settings, volume, brightness, exit to launcher |

---

## ⚙️ Port Architecture & Key Adaptations

1. **Memory Layout (`linker.lf`):**
   - Large static `.bss` game tables and buffers from `libvanilla-conquer.a` are mapped into 8MB Octal PSRAM (`extram_bss`), preserving internal SRAM for DMA, Wi-Fi/USB, and display transfers.
2. **Display Pipeline (`display.c` & `cnc_video.cpp`):**
   - Native 320x240 8-bit paletted DOS rendering (`DOSMode = true`).
   - Double-buffered `rg_surface_t` instances in PSRAM using `RG_PIXEL_565_BE` and `rg_display_submit()` for tear-free, color-accurate output.
   - Instant palette application (`Fade_Palette_To` mapped directly to `Set_Palette`) to avoid legacy DOS timer stalls.
3. **Cutscenes (`intro.cpp`, `conquer.cpp`, `vqatask.cpp`):**
   - VQA briefing/intro movies are skipped on ESP32-S3 to save ~4 MB of PSRAM and eliminate VQA audio-sync stalls, transitioning straight from team selection into mission gameplay.
4. **Audio Subsystem (`cnc_sound.cpp`):**
   - Dedicated FreeRTOS mixer task on Core 0 mixing up to 8 channels at 22,050 Hz 16-bit stereo into `rg_audio_submit()`.

---

## 📊 Performance & Behavior Notes

- **Memory Footprint:**
  - Uses ~5.5–6.0 MB of the 8 MB Octal PSRAM during active gameplay, leaving ~1.9–2.5 MB free PSRAM and ~30 KB free internal SRAM.
- **Mission Loading Times (SD Card On-Demand Caching):**
  - `.MIX` archives are cached on demand from the MicroSD card (`no mixcache partition` fallback).
  - **NOD Mission 1 (`SCB01EA` / Desert):** ~4–5 seconds to load.
  - **GDI Mission 1 (`SCG01EA` / Temperate):** ~10–15 seconds to load (`TemplateTypeClass::Init` reads ~200+ tile/template files individually over SPI). The screen remains dark during this load before the battlefield appears—this is normal.
- **Frame Rates:**
  - Dedicated `videoTask` handles 8-bit-to-RGB565 conversion and SPI DMA display submission independently of the game logic loop (`15–30+ FPS` UI/LCD updates depending on scene activity).
- **Audio:**
  - If `SCORES.MIX` is not present on the SD card, music is skipped automatically without stalling `ThemeClass::AI()`.
- **Theater Palette Fallback:**
  - If a theater `.PAL` (e.g., `DESERT.PAL`) is missing from its archive, the engine safely falls back to `TEMPERAT.PAL` / `WINTER.PAL` instead of crashing.

---

## 🚀 Building & Flashing

- **Build Core:**
  ```powershell
  python build_cnc.py
  ```
  Produces `cores/cnc-go.bin`.

- **Flash Core via USB (Core partition slot):**
  ```powershell
  python -m esptool --chip esp32s3 -b 1152000 write_flash 0x2e0000 cores/cnc-go.bin
  ```
  *(Alternatively, copy `cnc-go.bin` to `/sd/retro-go/cores/cnc-go.bin` or `/sd/cores/cnc-go.bin` on your SD card to launch via the Retro-Go Launcher).*
