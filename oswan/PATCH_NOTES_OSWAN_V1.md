# Oswan Retro-Go v1 — menu integration + performance pass

Baseline: uploaded `oswan.zip`

## Menu/input fixes

- MENU is handled as a Retro-Go system-key press edge and opens `rg_gui_game_menu()` once.
- OPTION is handled as a Retro-Go system-key press edge and opens `rg_gui_options_menu()` once.
- Holding either key no longer repeatedly re-enters a Retro-Go menu.
- MENU/OPTION are consumed by the frontend and are not passed into WonderSwan input.
- Existing Select -> X/Y pad toggle behavior is preserved.

## Performance changes

### 1. Fast memory for CPU page-map tables

`ROMMap` and `RAMMap` pointer tables are now allocated with `MEM_FAST` instead of `MEM_ANY`.
Only the 256 pointer entries are moved; ROM/RAM page contents remain where they were allocated.

### 2. Renderer scratch buffers moved off the stack

`RefreshLine()` previously created two 256-entry `int` arrays on its stack on every scanline:

- ZBuf: 1024 bytes
- WBuf: 1024 bytes

They are now allocated once in `MEM_FAST` and reused by the renderer. This reduces per-scanline stack pressure and keeps the hot renderer scratch data in fast memory.

### 3. Oswan compiler options

The Oswan component explicitly uses:

- `-O3`
- `-fomit-frame-pointer`
- `-fno-strict-aliasing`
- `-fjump-tables`
- `-ftree-switch-conversion`

The previous empty per-source `COMPILE_FLAGS` override was removed so these component options are not unnecessarily overridden.

## Intentionally NOT changed

- NEC CPU timing
- `WsRun()` slot scheduling
- interrupt timing
- DMA timing
- WonderSwan video timing
- frameskip policy
- audio quality
- game input mappings other than MENU/OPTION frontend handling

These areas are timing-sensitive and should be benchmarked separately before modification.

## Verification

Source-level structure checks completed. The ESP-IDF/ESP32-S3 build and hardware runtime are **not verified in this environment**.

This is a controlled v1 performance/menu patch, not a final release.
