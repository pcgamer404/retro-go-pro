# Snes9x port for Retro-Go

## Based on:

I believe it was based on https://github.com/libretro/snes9x2010

## Modifications:



## Retro-Go ESP32-S3 v1 optimization patch

- Double display buffering to prevent the LCD DMA from reading the framebuffer while Snes9x is rendering into it.
- Keep a pointer to the last completed framebuffer for redraw/screenshot callbacks.
- Place the hot main Z-buffer in internal RAM and the larger sub-screen/sub-Z buffers in PSRAM.
- Use a two-entry multicore audio queue with two audio buffers.
- Use GCC O3 plus low-risk switch/dispatch optimizations.
- Render SNES Mode 5/6 at half width on the fixed 256-pixel Retro-Go framebuffer to prevent 512-pixel writes into a 256-pixel surface.
- Apply SNES direct-colour brightness through the existing brightness LUT.

This patch targets the ESP32-S3 Retro-Go display/output layer and the existing Snes9x renderer; it does not replace the emulator core with PocketSNES code.
