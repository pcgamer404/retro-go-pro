# Sinclair ZX Spectrum for Retro-Go (`zx-go`)

Native Sinclair ZX Spectrum (48K / 128K) core for Retro-Go Pro on ESP32-S3 handheld devices.

Based on Miklos Szeredi's classic Spectemu engine, customized and optimized for ESP-IDF v5.3.x and Retro-Go handheld consoles with native RGB565 rendering, borderless 4:3 scaling, automated tape loading, multiple joystick emulations, title-screen shortcut bindings, and an on-screen virtual keyboard.

---

## Supported Formats

Place game files in `/sd/roms/spectrum/` or launch directly from the Retro-Go launcher:

- `.tap` - Tape image files (fast auto-loading supported)
- `.tzx` - Exact tape images (auto-loading supported)
- `.z80` - Standard Z80 snapshot files (48K / 128K)
- `.sna` - Snapshot files (48K)
- `.zip` - Compressed snapshots and tape images (auto-extracted/mounted)

---

## Video & Display Modes

The Sinclair ZX Spectrum active display area is **256x192 pixels** (an exact **4:3 aspect ratio**).

`zx-go` provides two switchable display modes in the in-game options menu:

1. **Full Screen (Borderless)** *(Default)*:
   - Renders only the active 256x192 game playfield.
   - Retro-Go's hardware display pipeline scales the 256x192 frame by $1.25\times$ to fill the $320 \times 240$ LCD edge-to-edge with **zero borders**.
   - Saves 36% memory bandwidth and framebuffer allocation compared to full 320x240 rendering.
2. **With Border**:
   - Renders the classic $320 \times 240$ frame featuring 32 border pixels on the left/right and 24 border lines on the top/bottom.
   - Dynamic border color changes (I/O port `0xFE` / ULA outport) are reflected in real time.

---

## Controls & Keybinding Reference

ZX Spectrum microcomputers did not have standard gamepad controllers; commercial games expect keyboard selections (`1`–`5`, `Space`, `Enter`) on their title screens to select joystick types or start games.

### 1. Default Handheld Controls (Kempston Joystick Mode)

| Physical Button | Primary Action | Description |
|-----------------|----------------|-------------|
| **D-Pad**       | Directions     | Kempston Up, Down, Left, Right |
| **Button A**    | Fire           | Kempston Fire (`KEMPF` on port `0x1F`) |
| **Button B**    | Jump / Up      | Kempston Up (`KEMPU` for platformer jump) |
| **Button X**    | `SPACE`        | Spectrum Space key (universal Start/Action) |
| **Button Y**    | `ENTER`        | Spectrum Enter key |
| **START**       | `ENTER`        | Spectrum Enter key |
| **L Shoulder**  | Key `1`        | Instant single-press **`1`** (Option 1 / Start / Keyboard) |
| **R Shoulder**  | Key `2`        | Instant single-press **`2`** (Option 2 / Kempston Joystick) |

### 2. SELECT Hotkey Chords (Edge-Triggered)

Hold **SELECT** and tap any face or directional button for instant single-keystroke injection:

| Combo | Key Injected | Primary Use Case |
|-------|--------------|------------------|
| **SELECT + UP**    | Key `'1'` | Select Option 1 / Start Game / 1-Player |
| **SELECT + RIGHT** | Key `'2'` | Select Option 2 / Kempston Joystick |
| **SELECT + DOWN**  | Key `'3'` | Select Option 3 / Cursor Joystick |
| **SELECT + LEFT**  | Key `'4'` | Select Option 4 / Sinclair Joystick |
| **SELECT + START** | Key `'5'` | Jetpac *"Press 5 to start"* / Sinclair 2 Fire |
| **SELECT + A**     | Key `'Y'` | "Yes" (confirmation on cheat/trainer menus) |
| **SELECT + B**     | Key `'N'` | "No" (confirmation on cheat/trainer menus) |
| **SELECT + X**     | Key `' '` | Space key |
| **SELECT + Y**     | Virtual Keyboard | Open 40-key on-screen keyboard picker |
| **SELECT Tap**     | Key `' '` | Tapping SELECT alone (< 0.6s) injects Space |
| **SELECT Hold**    | Virtual Keyboard | Holding SELECT alone (≥ 0.6s) opens Virtual Keyboard |

### 3. Switchable Joystick Controller Types

In the in-game options menu (**MENU** $\rightarrow$ **Options**), you can change the active joystick mode:

| Joystick Mode | Left | Right | Down | Up | Fire Button (A / B) |
|---------------|------|-------|------|----|---------------------|
| **Kempston** *(Default)* | Kempston L | Kempston R | Kempston D | Kempston U | Kempston Fire / Jump |
| **Sinclair 1** | Key `6` | Key `7` | Key `8` | Key `9` | Key `0` |
| **Sinclair 2** | Key `1` | Key `2` | Key `4` | Key `3` | Key `5` |
| **Cursor**     | Key `5` | Key `8` | Key `6` | Key `7` | Key `0` |
| **QAOP Space** | Key `O` | Key `P` | Key `A` | Key `Q` | Key `SPACE` |

---

## In-Game Emulator Options

Press the **MENU** button while running a game to access:

- **Screen Mode**: Toggle between `Full Screen` (256x192 borderless) and `With Border` (320x240 classic).
- **Joystick**: Cycle controller modes (`Kempston`, `Sinclair 1`, `Sinclair 2`, `Cursor`, `QAOP Space`).
- **Type Key**: Quickly inject common keys on demand: `[1] [2] [3] [4] [5] [Space] [Enter] [Y] [N]`.
- **Virtual Keyboard**: Opens a full 40-key ZX Spectrum graphic keyboard picker.
- **Type LOAD ""**: Injects `LOAD ""` into Sinclair BASIC manually if needed.
- **Save State / Load State**: Standard Retro-Go `.sna` snapshot save states.

---

## Technical Architecture & Fixes

1. **Little-Endian Xtensa Architecture Fix**:
   - Spectemu originally overlaid 16-bit register `PC` and 32-bit host pointer `PCP` in a `union dregp`.
   - Newlib's `<sys/types.h>` defined `#define _BIG_ENDIAN 4321`, which caused a bogus `#if defined(_BIG_ENDIAN)` to evaluate to true.
   - This erroneously mapped `PC` into the high word of `PCP`, zeroing out base pointers on boot (`PCP = 0x00000000`) and crashing with `LoadProhibited`.
   - Fixed by enforcing GCC compiler endian built-ins (`__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__`) and passing `-DHAVE_CONFIG_H`.
2. **Kempston Port Decoding**:
   - Standard Spectrum Kempston interfaces decode on `A5 == 0`. Full port range matching `!(port & 0x20)` is mirrored in `z80_inports` to ensure 100% compatibility across all Kempston titles.
3. **Automated Tape Injection**:
   - Detects `.tap` / `.tzx` media on boot, waits for Sinclair BASIC ROM initialization (~1.2s), and automatically injects `LOAD ""` keystrokes into the frame buffer.
4. **Clean Launcher Shutdown**:
   - `safe_switch_app()` safely terminates the FreeRTOS background system monitor task before switching apps, eliminating assertion panics during launcher app reboot.

---

## Design Notes & Recommendations

Because the Sinclair ZX Spectrum is a full microcomputer platform, many games (particularly text adventures, RPGs, and strategy titles) require continuous typing of alphanumeric text. While `zx-go` provides comprehensive shortcuts and an on-screen keyboard, microcomputer platforms are inherently best experienced on hardware with physical keyboard input (such as USB HID or Bluetooth keyboard support) or dedicated keyboard handhelds.
