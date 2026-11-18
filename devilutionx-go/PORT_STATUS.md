# DevilutionX-Go — Port Status & Technical Architecture

## Current Status: Completed & Playable

The port of DevilutionX for ESP32-S3 under Retro-Go is fully playable from start to finish.

---

## Architecture & Optimizations Implemented

### 1. Memory Optimization for 8MB Octal PSRAM
- **Missile Sprite On-Demand Streaming**:
  - Upstream DevilutionX preloads all ~50 missile CL2/CLX graphics in `InitMissileGFX()`, consuming over 1.2 MB of continuous PSRAM.
  - Implemented on-demand sprite loading in `Source/tables/misdat.cpp:GetMissileSpriteData()`, keeping missile graphics loaded only when needed during gameplay and freeing ~1.18 MB PSRAM.
- **Player Portrait Deduplication**:
  - Reused `player.AnimationData[Stand]` in `Source/player.cpp:GetPlayerPortraitSprite()`, eliminating a redundant 350 KB allocation in `PartyInfoSprites`.
- **Town Load Event Queue**:
  - Handled custom SDL user events with 32-bit types (`Source/interfac.h`), preventing event truncation and false assertion halts during Tristram level load.

### 2. Display Pipeline & Text Sharpness
- **Font Emboldening**:
  - In `Source/engine/render/text_render.cpp:DrawFont()`, glyphs are emboldened across a 2x2 grid `(x+1, y)`, `(x, y+1)`, and `(x+1, y+1)`.
  - When the 640x480 frame is decimated 2:1 down to 320x240 by Retro-Go, 100% of single-pixel strokes are guaranteed to hit the sampling grid, rendering crisp, readable text on 320x240 LCD screens.
- **Double-Buffered Atomic Display Presentation**:
  - `SDL_video.c` uses a dedicated backbuffer decoupled from `rg_screen->data`.
  - Core 0 software-renders Diablo into the backbuffer.
  - On `SDL_Flip()`, the frame is atomically submitted to `display_task` on Core 1 for SPI/DMA transmission, eliminating screen tearing and lower-screen flickering.
  - Redundant 70 FPS limiter loops in `SDL_video.c` were removed to optimize frame pacing toward Diablo's native 20 FPS simulation speed.

### 3. Handheld Controls Mapping (No Shoulder Buttons)
- Direct mapping for handhelds with only D-Pad, A, B, X, Y, START, SELECT.
- Integrated `PadMenuNavigator` onto the **SELECT** button as a modifier key:
  - `SELECT + B`: Quick Health Potion
  - `SELECT + Y`: Quick Mana Potion
  - `SELECT + DPAD_RIGHT`: Inventory
  - `SELECT + DPAD_LEFT`: Character Info
  - `SELECT + A`: Spellbook
  - `SELECT + X`: Quest Log
  - `SELECT + DPAD_DOWN`: Cycle Automap Zoom
  - `SELECT + DPAD_UP`: Options Menu
  - Tap `SELECT`: Toggle Automap
