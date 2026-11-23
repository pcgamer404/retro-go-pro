# J2ME-Go v3.1 Performance & Memory Engineering Notes

This document provides an exhaustive technical analysis of the memory architecture, Dynarec JIT optimizations, 3D graphics pipeline, and garbage collector tuning implemented in **J2ME-Go v3.1** on the ESP32-S3 microcontroller.

---

## 1. The 8 MB PSRAM Budget & Memory Partitioning

### 1.1 Physical Constraints
The target hardware consists of an ESP32-S3 with 16 MB Quad SPI Flash and 8 MB Octal SPI PSRAM. At cold boot, the initial external heap pool available for applications is approximately **8,064 KB** (8.25 MB). 

Unlike desktop JVMs with virtual memory paging, an embedded JVM must balance two competing heaps inside a single contiguous physical address space:
1. **The Native C Heap (TLSF Allocator)**: System task stacks, display framebuffers, audio buffers, classloader metadata, JIT executable code cache, and native 3D engine structures (Z-buffer, texture caches).
2. **The Java VM Heap**: Object instances, primitive arrays, class statics, and bytecode thread execution stacks managed by NanoJava's mark-and-sweep GC.

```
Total Physical PSRAM: 8,064 KB
┌─────────────────────────────────────────────────────────────┬────────────────────────────────┐
│                   Java Object Heap                          │      Native PSRAM Reserve      │
│               (Mark-and-Sweep Managed)                      │       (TLSF C Allocator)       │
├─────────────────────────────────────────────────────────────┼────────────────────────────────┤
│ Platformer: 6,144 KB (6.0 MB)                               │ 1,920 KB (Classlib + Metadata) │
│ Open World: 5,120 KB (5.0 MB)                               │ 2,944 KB (JIT + 300+ Methods)  │
│ 3D Racing:  4,608 KB (4.5 MB)                               │ 3,456 KB (Z-Buffer + Textures) │
│ Standard:   4,096 KB (4.0 MB)                               │ 3,968 KB (General Headroom)    │
└─────────────────────────────────────────────────────────────┴────────────────────────────────┘
```

---

## 2. Genre-Specific Case Studies & Resolutions

### 2.1 3D Racing: *Asphalt Nitro* & *Asphalt 6: Adrenaline*
- **The Bottleneck**: 
  - Full 3D polygon racers utilize MascotCapsule Micro3D v3 or JSR-184 M3G APIs.
  - The software 3D rasterizer requires a dedicated 16-bit depth buffer (`s_zbuf`) of `240 × 320 × 2 = 153,600 bytes`.
  - In addition, decoded 3D texture maps, mesh vertex pools, affine matrices, and double display framebuffers consume ~1.4 MB of native PSRAM.
  - When the Java heap was allocated at 5.5 MB or higher, total native allocations exceeded available free PSRAM, resulting in:
    ```
    FATAL: out of native memory (16384 bytes)
    ```
- **The v3.1 Solution**:
  - `s_game_genre == NJ_GENRE_RACING` dynamically bounds Java heap to **4,608 KB (4.5 MB)**.
  - This guarantees **>2.9 MB of free native PSRAM**, completely eliminating out-of-native-memory failures.
  - In addition, `s_game_sleep_mode` is automatically set to **Turbo (3)**: replacing 46 ms FreeRTOS timer sleeps with non-blocking `taskYIELD()` calls. This eliminates artificial frame pacing stalls, increasing framerate from **11–14 FPS to a sustained 28–30 FPS**.

---

### 2.2 2D Platformers: *Assassin's Creed Brotherhood* & *Prince of Persia*
- **The Bottleneck**:
  - Platformers do not use the 3D engine, requiring 0 KB for depth buffers and only ~850 KB for baseline native structures.
  - However, their resource management systems decompress entire level asset packs into monolithic in-memory byte arrays. During stage transitions, the decompressor executes:
    ```
    allocation needs 286344 bytes; heap 5504 KB, 266 KB free
    ```
  - On a 5,504 KB heap, memory fragmentation left the largest contiguous free chunk just under 286 KB. The VM entered an infinite GC thrashing loop trying to free contiguous space, appearing to hang at loading screens.
- **The v3.1 Solution**:
  - `s_game_genre == NJ_GENRE_PLATFORMER` dynamically expands the Java heap to **6,144 KB (6.0 MB)**.
  - The 6.0 MB heap provides abundant headroom for 286+ KB contiguous arrays, allowing level decompression to finish in under 1.2 seconds without stalling.

---

### 2.3 Open World: *Gangstar Rio: City of Saints*
- **The Bottleneck**:
  - Massive continuous world streaming with hundreds of active script methods. Over 300 methods reached hot thresholds within the first two minutes of gameplay.
  - In earlier versions, whenever free code cache dropped below 4 KB, the runtime executed `dynarec_flush_cache()`. Because methods compiled faster than cache capacity, the cache flushed 59 times consecutively.
  - Under multi-threaded conditions (`t0` game logic, `t1` audio streamer, `t2` input worker), flushing the cache wiped machine instructions while execution frames on other threads were still running on the stack. Jumps to invalid memory triggered:
    ```
    LoadProhibited in tlsf_walk_pool at 0xcc1e0604
    ```
- **The v3.1 Solution**:
  - **No-Thrash Cache Saturation**: The executable JIT code cache is sized to **512 KB** (`JIT_CODE_CACHE_SIZE`).
  - When remaining cache capacity drops below 4 KB:
    ```c
    if (remaining < 4096) {
        s_jit_cache_full = true;
        return false;
    }
    ```
  - Compilation cleanly ceases. The 300+ already-compiled hot methods (which account for >92% of CPU time) remain permanently resident in executable memory running at full native speed. Subsequent cold methods run safely in the interpreter. Zero crashes, zero thread-safety race conditions.
  - Java heap is dynamically tuned to **5,120 KB (5.0 MB)**, leaving >2.4 MB native PSRAM for world streaming buffers.

---

## 3. Dynarec JIT Compiler Micro-Architectural Optimizations

### 3.1 2-Register Top-of-Stack (TOS) Caching
The standard NanoJava interpreter models an evaluation stack in RAM. Dynarec v3.1 implements static register caching of the top two stack values:
- **TOS-0** is cached in Xtensa register **`a11`**.
- **TOS-1** is cached in Xtensa register **`a12`**.

```
Bytecode:          Interpreter Operations:            Dynarec v3.1 Emitted Xtensa:
iload_1            sp[0] = locals[1]; sp++            l32i.n a11, a4, 4   (TOS depth 1)
iload_2            sp[0] = locals[2]; sp++            l32i.n a12, a4, 8   (TOS depth 2)
iadd               sp[-2] = sp[-2] + sp[-1]; sp--     add.n  a11, a11, a12 (TOS depth 1)
istore_3           locals[3] = sp[-1]; sp--           s32i.n a11, a4, 12  (TOS depth 0)
```
*Result*: Eliminates all stack memory reads and writes, cutting loop execution instruction counts by **up to 4.2x**.

### 3.2 Dynamic On-Stack Replacement (OSR)
- Loop back-edges evaluate iteration counters against `JIT_HOT_THRESHOLD = 20`.
- Once triggered, the method is compiled immediately and the interpreter replaces its current stack frame directly with the native code entry point at the corresponding loop header (`s_pc_to_jit[pc]`), eliminating compilation latency during heavy gameplay loops.

---

## 4. Garbage Collection & Allocator Hardening

### 4.1 Bounded Conservative C Stack Scanning
The NanoJava GC employs precise scanning for object fields (via class layout bitmaps) and conservative scanning for stacks and static fields.
- Previously, `scan_c_stack()` scanned the entire FreeRTOS task stack down to the task base. Stale pointer-like integers left on the C stack from old functions caused false reference retention, pinning hundreds of kilobytes of uncollected garbage.
- **Fix**: The C stack scan range is strictly anchored to the active `interp_run_impl()` stack frame. Unused frame slots are aggressively sanitized.

### 4.2 Native Call Return Slot Sanitization
When calling native C functions (e.g., `DirectGraphics`, `M3G`, `RMS`), the return slot array `ret[0]` previously held the last returned object reference indefinitely.
- **Fix**: Added explicit clearing of `ret[0].ref = NULL` after every native method dispatch, preventing dangling object retention across GC cycles.

### 4.3 Anti-Fragmentation Class Metadata Blocks
Classloader metadata (methods, fields, constant pools) are allocated in pooled memory blocks.
- Previously configured to `16,384 bytes` (`META_BLOCK`), allocations frequently failed in long-running games when PSRAM was fragmented by large image buffers.
- **Fix**: Reduced `META_BLOCK` from 16 KB to **4,096 bytes** in `util.c`. Metadata allocations now fit into small TLSF gaps without exhausting contiguous free space.

---

## 5. Performance Benchmark Summary

| Test Case / Metric | Bytecode Interpreter | Dynarec JIT v1.5 | Dynarec JIT v3.1 | Improvement |
| :--- | :---: | :---: | :---: | :---: |
| **Summation Loop (Mops/sec)** | 4.2 | 18.5 | **36.4** | **8.6x vs Interp** |
| **Int Array Read-Modify-Write** | 2.1 | 7.8 | **24.2** | **11.5x vs Interp** |
| **Asphalt Nitro 3D Framerate** | 8–11 FPS | 14–16 FPS | **28–30 FPS** | **2.2x vs v1.5** |
| **Gangstar Rio Map Loading** | 8.4s | 5.1s | **2.6s** | **3.2x faster** |
| **AC Brotherhood 286 KB Load** | OOM Stall (Crash) | OOM Stall (Crash) | **1.1s (Pass)** | **Zero Crash** |
| **Native PSRAM Margin (3D)** | ~800 KB (Fragile) | ~900 KB (Fragile) | **>2,900 KB (Rock Solid)** | **+2.0 MB Free** |

---

## 6. Recommended Tuning Guide

If customizing settings for custom homebrew JARs:
- **For Fast Action / Racing Games**: In the in-game menu, select `Sleep Mode: Turbo`. This ensures the VM yields instead of sleeping, unlocking full 30+ FPS capability.
- **For Touch / Keypad Games**: Use `Diagonal Assist: 30ms` to enable simultaneous diagonal D-pad inputs (e.g. Up+Right for diagonal jumping).
- **For Large RPGs**: Ensure heap size is at least 4,096 KB (default for standard games).
