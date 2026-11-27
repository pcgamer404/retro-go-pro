# Commodore Amiga (`amiga-go`) Performance & Architecture Notes

## 1. Target Hardware & Constraints

- **SoC:** Espressif ESP32-S3 (Dual-core Xtensa® 32-bit LX7 @ 240 MHz).
- **RAM:** 512 KB Internal SRAM (~380 KB available after system heap/IRAM reservations), 8 MB Octal SPI PSRAM.
- **Storage:** 16 MB Quad SPI Flash.
- **Target Frame Rate:** PAL 50.00 Hz (20,000 µs frame budget).
- **Native Screen:** 320 × 240 RGB565 (ILI9341 / ST7789 parallel/SPI display controller).

---

## 2. Emulation Bottleneck Breakdown

Through continuous profiling via `rg_system_timer()`, frame execution time was broken down into four distinct subsystems:

```
Total Frame Time: ~33.3 ms (30.0 FPS)
├── Emulation Core (amiga_run_frame):   ~25.5 ms (76.5%)
├── Screen Rasterization (render_surface): ~7.2 ms (21.6%)
├── Audio Mixing & Drainage (audio_drain): ~0.4 ms (1.2%)
└── Display DMA Dispatch (display_submit): ~0.01 ms (<0.1%)
```

---

## 3. Major Optimizations & Engineering Solutions

### A. Opcode Jump Table: Eliminating External Octal PSRAM Latency
- **Problem:** Stock Musashi `m68k_use_table_copies()` allocated 320 KB in external SPI PSRAM, copying the 256 KB opcode jump table (`65,536` function pointers) and 64 KB cycle table into PSRAM. Because external Octal PSRAM accesses incur 20–30 CPU cycles of SPI bus latency, every single M68000 instruction dispatch suffered a bus wait, contending with Chip RAM and Slow RAM traffic.
- **Solution:** Modified `m68kcpu.c` so `instruction_jump_table` remains pointing directly to `m68ki_static_instruction_jump_table` located in Flash `.rodata`. The ESP32-S3 hardware instruction cache automatically caches hot opcode function pointers in high-speed internal cache RAM, eliminating external SPI bus round-trips.

### B. CPU Cycle Count Table in Zero-Wait-State Internal SRAM (DIRAM)
- **Problem:** Musashi reads the instruction cycle count on every executed instruction via `CYC_INSTRUCTION(REG_IR)`.
- **Solution:** Allocated the 64 KB `cycles` table directly in **Fast Internal SRAM** (`heap_caps_malloc(0x10000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`). Cycle lookup now executes with zero wait states.

### C. Fast Bus Timing Mode (Bypassing Bus Slot Contention Math)
- **Problem:** On every memory access (millions per second), `mem.c` called `bus_cycle(a)` -> `slot_wait(at)` -> `work_out_waits()`, calculating Amiga custom chip DMA cycle contention math against even/odd color clock slots.
- **Solution:** Implemented `s_fast_bus` mode (default: Enabled, selectable in options as `Bus Timing: Fast / Turbo`). In Fast / Turbo mode, `bus_cycle()`, `bus_take()`, and `bus_waited_over()` return immediately, removing substantial CPU cycle calculation overhead from memory reads and writes.

### D. Immediate Blitter Mode (`amiga_blitter_set_fast`)
- **Problem:** Authentic Amiga blitter operations interleave with the CPU across scanlines, requiring cycle scheduling and blitter steal calculations (`blitter_steal_line`).
- **Solution:** In Fast Blitter mode, blits complete immediately upon initiation (`blit.busy = false`), instantly raising `INT_BLIT`. This dramatically accelerates screen clears, sprite draws, and scrolling playfields in games like *Lotus 2*.

### E. Scaled Display Line Deduplication
- **Problem:** The Amiga renders 200–256 scanlines per field. Scaling a 200-line screen to the 240-line handheld display repeats approximately 1 line in every 6. Re-rasterizing the duplicate line with `amiga_get_line_rgb565()` wasted ~1.2 ms per frame.
- **Solution:** In `render_surface()`, line index tracking detects identical source rows and copies the previous output scanline with a fast 32-bit word-aligned `memcpy`, bypassing color translation and sprite composite logic.

### F. Single-Buffer Audio Drainage
- **Problem:** Multiple small audio buffer submissions caused FreeRTOS task context switching and occasional USB audio driver underruns.
- **Solution:** Paula sound is batched and submitted in a single atomic chunk per frame via `s_audio_buffer` (1024 frames @ 44.1 kHz), with excessive backlog discarded during menu pauses.

---

## 4. Performance Milestones Summary

| Milestone / State | Lotus 2 FPS | Emulation Time | Render Time | Total Frame Time | Notes |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **Initial Port** | Crash / 0 | N/A | N/A | N/A | Out of memory / 2.88KB stack overflow |
| **First Playable** | ~19 FPS | ~45.0 ms | ~7.8 ms | ~53.0 ms | Jump table in PSRAM, slot math active |
| **Window Detect Fix** | ~21 FPS | ~40.0 ms | ~7.6 ms | ~48.0 ms | Auto DIW detection, black border resolved |
| **Line Deduplication** | ~24 FPS | ~34.0 ms | ~7.2 ms | ~41.5 ms | Duplicate lines copied via `memcpy` |
| **Fast Bus + SRAM Cycles** | **30 FPS** | **~25.5 ms** | **~7.2 ms** | **~33.3 ms** | Opcode table in cache, SRAM cycles, Fast Bus |

---

## 5. Compatibility & Performance Tuning Guide

If an individual game experiences timing anomalies or loader issues, use the in-game options menu (`START + B`):

1. **Floppy Speed:**
   - `8x (Turbo)`: Default. Drastically reduces floppy loading times from minutes to seconds.
   - `1x (Authentic)`: Use if a game uses a sensitive custom MFM trackloader that fails to load under turbo speeds.
2. **Bus Timing:**
   - `Fast / Turbo`: Default. Maximum emulation performance.
   - `Authentic`: Cycle-exact DMA slot wait timing. Recommended for timing-critical demos or games that glitch under turbo bus timing.
3. **Blitter:**
   - `Fast / Immediate`: Default. Highest framerate.
   - `Authentic`: Cycle-exact blitter interleaving.
4. **Model:**
   - `A500`: Default. Kickstart 1.3 + OCS (512 KB Chip + 512 KB Slow). Maximum compatibility with 1985–1992 floppy games.
   - `A1200`: Kickstart 3.1 + AGA (2048 KB Chip). For AGA-specific titles.
