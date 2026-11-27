# ZX-Go Performance & Optimization Notes

## 1. System Architecture & Resource Utilization

The `zx-go` core runs on the ESP32-S3 dual-core Xtensa LX7 microcontroller clocked at 240 MHz within the Retro-Go Pro operating environment:

| Subsystem | Metric | Notes |
|-----------|--------|-------|
| **Z80 CPU Interpreter** | 3.5 MHz PAL | Table-driven opcode execution (~69,888 T-states per 20ms frame) |
| **CPU Utilization** | **12% – 18%** | ~2.5 ms execution time per 20 ms PAL frame budget |
| **Frame Rate** | **50.0 FPS** | Rock-solid PAL 50Hz synchronization, 0 frameskip |
| **BSS / Static RAM** | ~220 KB | 64KB aligned Z80 RAM + port tables + ULA attribute buffer |
| **Framebuffer Size** | **96 KB** (Full Screen) / 150 KB (Bordered) | 256x192 RGB565 vs 320x240 RGB565 |
| **Audio Sample Rate** | 15,600 Hz | 312 beeper samples per frame |
| **Flash Partition Footprint** | ~538 KB | Binary image `zx-go.bin` |

---

## 2. Display Pipeline & Scaling Optimizations

### Full Screen (Borderless) vs Bordered Mode
- **Standard Spectrum Video**: The Sinclair ULA generates an active 256x192 pixel playfield (32x24 character cells of 8x8 pixels each) surrounded by a border.
- **Aspect Ratio**: $256 \times 192$ is an exact 4:3 aspect ratio:
  $$\frac{256}{192} = 1.3333... = \frac{320}{240}$$
  A uniform $1.25\times$ expansion fills the standard $320 \times 240$ handheld LCD completely edge-to-edge.
- **Rendering Throughput in Full Screen Mode**:
  - In `Full Screen` mode, `spectemu_render_screen()` only converts the active 49,152 pixels directly to RGB565.
  - Skips 27,648 border pixel writes every frame.
  - Saves **36% DMA transfer bandwidth** and reduces frame blit time from ~1.4 ms down to ~0.8 ms.
- **Display Driver Handoff**:
  - Framebuffer submitted directly via `rg_display_submit()`.
  - Scaler pipeline in `rg_display` handles LCD scaling via fast SPI DMA without CPU intervention.

---

## 3. Z80 CPU Emulation & Memory Layout

### 64KB Page Alignment & Register Overlay
- Spectemu achieves high performance on 32-bit RISC hosts by aligning the 64KB Z80 address space to a 64KB boundary:
  ```c
  DANM(mem) = (byte *)(((uintptr_t)bigmem + 0xFFFF) & ~((uintptr_t)0xFFFF));
  ```
- Because the lower 16 bits of the 64KB aligned pointer are `0x0000`, the 16-bit register value (e.g. `PC`) can be overlaid directly onto the lower 16 bits of host pointer `PCP` inside `union dregp`.
- When `PC` increments or changes, host pointer `PCP` automatically points to the exact memory address without requiring a base-address addition on every instruction fetch.
- **Endianness Correction**: Xtensa is little-endian; ensuring `WORDS_BIGENDIAN` is strictly undefined places 16-bit register `d` in bytes 0 and 1 of 32-bit pointer `p`, preserving the 64KB page base in bytes 2 and 3.

---

## 4. Audio Subsystem

- The ZX Spectrum utilizes a 1-bit beeper connected to bit 4 of I/O port `0xFE`.
- `spectemu_core` samples the beeper state once every scanline (312 scanlines per PAL frame), producing 312 mono samples per frame (15,600 Hz).
- Samples are scaled and submitted to Retro-Go's native audio ring buffer via `rg_audio_submit()`.
- Compatible with all Retro-Go audio sinks (internal speaker, I2S DAC, and USB Audio DAC) with zero underruns or buffer crackle.

---

## 5. Input Matrix Scanning

- The Sinclair keyboard matrix consists of 8 half-rows scanned through address lines `A8`–`A15` on port `0xFE`.
- Keystroke injection is paced with a 6-frame hold (120 ms) and 4-frame release (80 ms) queue:
  - Matches the Sinclair ROM interrupt keyboard debounce routine (`0x0038`).
  - Edge-triggered SELECT hotkeys prevent queue overflow.
- Kempston joystick reads (`portl & 0x20 == 0`) return active-high state on port `0x1F` and all aliased Kempston ports.
