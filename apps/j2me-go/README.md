# J2ME-Go v3.1

**High-Performance Java ME (MIDP 2.0 / CLDC 1.1) Runtime with Xtensa Dynarec JIT & Native 3D Acceleration for ESP32-S3 / Retro-Go**

J2ME-Go is an advanced, bare-metal Java Virtual Machine tailored for the ESP32-S3 microcontroller running the [Retro-Go](https://github.com/ducalex/retro-go) handheld operating system. It brings mobile phone gaming classics (MIDP 2.0, CLDC 1.1, Nokia DirectGraphics, MascotCapsule Micro3D v3, and JSR-184 M3G) to modern ESP32-S3 handheld consoles with fluid framerates, native audio synthesis, full filesystem RMS persistence, and responsive controls.

---

## What's New in v3.1

### 1. Dynamic Genre Profiler & Adaptive Memory Budgeting
On 8 MB Octal PSRAM ESP32-S3 hardware, games have drastically diverging memory demands:
- **3D Racing** (*Asphalt Nitro*, *Asphalt 6*, *NFS*): Demands over 2.5 MB of native PSRAM for the 150 KB 16-bit depth buffer (`s_zbuf`), texture cache, mesh vertex pools, and display buffers. Java heap is auto-tuned to **4,608 KB (4.5 MB)**, leaving >2.9 MB native PSRAM free to eliminate `out of native memory` panics and sustain solid 28–30 FPS.
- **2D Platformers** (*Assassin's Creed Brotherhood*, *Prince of Persia*): Allocates massive monolithic asset chunks on the Java heap during level decompression (e.g. 286 KB single `byte[]`). Java heap is auto-tuned to **6,144 KB (6.0 MB)** with minimal native overhead (~850 KB), completely preventing asset loading stalls.
- **Open World** (*Gangstar Rio: City of Saints*): Massive streaming maps with over 300 active compiled methods. Java heap is auto-tuned to **5,120 KB (5.0 MB)**, balancing heap allocation with native headroom (>2.4 MB) to prevent TLSF fragmentation.
- **Standard / RPGs / Others**: Auto-allocated 4,096 KB (or 4,480 KB for large JARs > 500 KB).

### 2. Unified Dynarec JIT Compiler
- **2-Register Top-of-Stack (TOS) Caching**: Tracks the top 2 operand stack values in Xtensa registers (`a11`, `a12`), eliminating memory traffic for standard arithmetic, bitwise, and comparison operations.
- **Direct Xtensa Trampolines**: Inter-method and native bridge calls preserve caller register state (`a10`) across windowed ABI boundaries.
- **Full On-Stack Replacement (OSR)**: Dynamic compilation and entry at loop back-edges, enabling instant acceleration without waiting for whole-method invocations.
- **Anti-Thrashing JIT Cache Saturation**: Allocates 512 KB of executable PSRAM for machine code. Once the cache reaches capacity (`< 4 KB` remaining), compilation gracefully halts (`s_jit_cache_full = true`). All resident hot methods remain permanently active at native speed without unstable mid-execution cache wipes, while cold methods seamlessly fall back to the interpreter.

### 3. Dual Native 3D Engines (MascotCapsule Micro3D v3 & JSR-184 M3G)
- Sub-pixel accurate 16-bit Z-buffered software rasterizer optimized for Xtensa LX7 instructions.
- Full fixed-point 16.16 matrix transformations, Gouraud lighting, flat shading, affine/perspective texture mapping, and clip-space culling.
- Native binary `.m3g` scene graph loader and skeletal keyframe animator.

### 4. Memory Allocator & GC Resilience
- **Bounded Conservative C Stack Scanning**: The garbage collector strictly bounds root scanning to the active interpreter stack frame (`interp_run_impl()`), preventing false references from pinning dead objects.
- **Native Method Return Zeroing**: Stale reference slots in `ret[0]` are systematically cleared immediately after native call dispatch.
- **Anti-Fragmentation Class Metadata**: `META_BLOCK` allocation chunks reduced from 16 KB to 4 KB, allowing dense packing in fragmented native memory pools.

---

## System Architecture

```
D:/Games/E/retro-go-pro/j2me/j2me-go/
├── art/                              # Core icons, banners, and launcher art
├── build/                            # Compiled binaries (j2me-go.bin, ELF, maps)
├── components/                       # Retro-Go platform component bindings
├── main/
│   ├── CMakeLists.txt
│   ├── nanojava/
│   │   ├── classlib/                 # Java standard library sources & JAR (CLDC 1.1 + MIDP 2.0)
│   │   ├── platform/retrogo/         # Retro-Go platform port (display, audio, input, settings)
│   │   │   └── nanojava_main.c       # Main entry, genre detection, memory allocator, GUI menu
│   │   └── src/
│   │       ├── midp/                 # MIDP 2.0 implementation
│   │       │   ├── events.c          # Event loop and input queue
│   │       │   ├── font.c / font_data.c # Proportional bitmap font rendering
│   │       │   ├── gfx.c / gfx.h     # 2D graphics, LCDUI primitives, scaling, rotation
│   │       │   ├── m3g_3d.c / m3g_3d.h # Dual 3D rasterizer (Micro3D v3 & JSR-184)
│   │       │   ├── natives_audio.c   # Audio playback (WAV, tone, MIDI synthesis)
│   │       │   ├── natives_lcdui.c   # LCDUI Display, Form, Canvas native hooks
│   │       │   └── natives_storage.c # Record Management System (RMS) on SD card
│   │       ├── pal/                  # Platform Abstraction Layer
│   │       ├── util/                 # Image decoders (PNG, BMP) and Deflate/Inflate/Zip
│   │       └── vm/                   # Core NanoJava Virtual Machine
│   │           ├── class.c / classfile.c # Class loader & constant pool resolution
│   │           ├── dynarec.c / dynarec.h # Unified Dynarec JIT compiler (Xtensa LX7)
│   │           ├── heap.c            # Garbage collector & memory management
│   │           ├── interp.c          # Bytecode interpreter with JIT OSR bridges
│   │           ├── native.c          # JNI-style native method registration
│   │           ├── thread.c          # Cooperative green threading & time-slicing
│   │           ├── util.c            # Metadata memory allocator (4 KB blocks)
│   │           ├── xjit_emit.h       # Xtensa instruction emitter
│   │           └── xjit_exec.c       # Executable memory protection and cache sync
│   └── tools/
│       └── dynarec-test/             # Host Xtensa emulator and automated test harness
├── core.json                         # Retro-Go core metadata definition
├── sdkconfig.defaults                # 64 KB D-Cache, 32 KB I-Cache, 1000 Hz FreeRTOS tick
└── README.md                         # This file
```

---

## Memory Budgeting & Genre Profiles

On an ESP32-S3 with 8 MB Octal SPI PSRAM, total usable external memory at startup is approximately **8,064 KB**. Memory is partitioned dynamically upon game launch:

| Genre | Example Games | Java Heap | Native PSRAM Reserve | Features Enabled |
| :--- | :--- | :---: | :---: | :--- |
| **Platformer** | *Assassin's Creed*, *Prince of Persia*, *Castlevania* | **6,144 KB** | ~1,920 KB | 286 KB decompression chunk support, 8-way diagonal assist |
| **Racing (3D)** | *Asphalt Nitro*, *Asphalt 6*, *NFS*, *Ferrari GT* | **4,608 KB** | **>2,900 KB** | 150 KB 16-bit Z-buffer, M3G mesh/texture cache, Turbo sleep mode (0ms yield) |
| **Open World** | *Gangstar Rio*, *Saints Row*, *GTA*, *Spider-Man* | **5,120 KB** | **>2,400 KB** | 300+ JIT methods, streaming map heap stability, 8-way diagonal assist |
| **RPG / Strategy** | *Final Fantasy*, *Doom RPG*, *Wolfenstein RPG* | **4,096 KB** | ~3,400 KB | Keypad 5 action default, optimized font glyph rendering |
| **Standard / Other** | *Sonic*, *The Sims 3*, *Cricket*, *Tetris* | **4,096–4,480 KB** | ~3,000 KB | Standard MIDP 2.0 LCDUI Canvas pipeline |

---

## Dynarec JIT Technical Specifications

The Dynarec JIT compiler transforms Java bytecode sequences directly into native Xtensa LX7 machine code at runtime.

### Xtensa Register Allocation Matrix
- **`a2`**: VM `Thread*` context pointer / return status code.
- **`a3`**: Active `Frame*` pointer.
- **`a4`**: Pointer to Java local variables array (`f->locals`).
- **`a5`**: Biased operand stack pointer (`f->sp`).
- **`a6 – a9`**: General-purpose scratch and temporary registers.
- **`a10`**: Callee-saved return address preservation register across trampolines.
- **`a11`**: Top-of-Stack Register 0 (TOS-0).
- **`a12`**: Top-of-Stack Register 1 (TOS-1).
- **`a13 – a15`**: Dynamic scratch and branch evaluation registers.

### Execution Guarantees
1. **Zero-Overhead Stack Folding**: Sequential instructions such as `iload_1; iload_2; iadd; istore_3` compile directly into Xtensa `l32i`, `add`, and `s32i` instructions without modifying the memory stack pointer `a5`.
2. **Deterministic Time Slices**: Loop backward branches emit `addi a11, -1; bltz deopt` to guarantee cooperative green thread multitasking and audio mixer execution cannot be starved.
3. **Safe Exception Bailouts**: Null pointer checks, array bounds verifications, and arithmetic zero divisions branch to cold deoptimization stubs, reconstructing exact interpreter frame state before resuming in the bytecode interpreter.

---

## Controls & In-Game Navigation

### Default Key Mappings
| Handheld Button | Java MIDP Keycode | Typical Function |
| :--- | :--- | :--- |
| **D-Pad Up / Down / Left / Right** | `UP`, `DOWN`, `LEFT`, `RIGHT` / `KEY_NUM2, 8, 4, 6` | Movement & navigation |
| **A Button** | `FIRE` / `KEY_NUM5` | Select / Jump / Action / Accelerate |
| **B Button** | `KEY_NUM7` / `KEY_SOFT_RIGHT` | Back / Brake / Cancel / Secondary Action |
| **X Button** | `KEY_NUM1` / `KEY_STAR` | Nitro / Special weapon / Menu shortcut |
| **Y Button** | `KEY_NUM3` / `KEY_POUND` | Camera switch / Map / Inventory |
| **Start Button** | `KEY_SOFT_LEFT` | Game Menu / Pause / Start |
| **Select Button** | `KEY_SOFT_RIGHT` | Secondary Menu / Options |
| **L / R Shoulders** | `KEY_NUM9`, `KEY_NUM0` | Weapon cycling / Shifting / Extra actions |

### In-Game Menu (Hold Menu Button)
Pressing the hardware **Menu** button brings up the J2ME-Go runtime settings overlay:
- **Canvas Scaling**: 1:1 Pixel-Perfect, Aspect-Fit (Scaled with bilinear filtering), Full-Screen Stretch.
- **Screen Rotation**: 0° (Normal), 90° (TATE vertical games), 180°, 270°.
- **Sleep Mode**: Default (46ms), Responsive (10ms), Turbo (0ms pure yield for maximum 3D/Action FPS).
- **Diagonal Assist**: Off, 30ms (Platformers/Action), 60ms (Strict grid-based RPGs).
- **Action Key Switch**: Map Button A to `FIRE` or `KEY_NUM5`.

Per-game preferences are automatically saved to SD card at `/sd/retro-go/config/j2me/games/<slug>.cfg`.

---

## Building and Flashing

### Prerequisites
- **ESP-IDF**: Version v5.3.x (with Python 3.11 environment).
- **Build Tools**: CMake >= 3.20, Ninja >= 1.12.
- **Target Hardware**: ESP32-S3 handheld console with 16 MB Quad SPI Flash and 8 MB Octal SPI PSRAM.

### Compiling from Source
To compile the core firmware binary:
```bash
# Navigate to the retro-go project root
cd D:/Games/E/retro-go-pro/j2me

# Execute the automated build driver
python build.py
```
The output binary is produced at:
`j2me-go/build/j2me-go.bin` (~953 KB).

### Flashing to Hardware
You can flash the compiled core using Retro-Go's toolchain or standard `esptool.py`:
```bash
# Flash j2me-go to the designated app partition (Offset 0x130000)
python -m esptool --chip esp32s3 -p COM4 -b 921600 write_flash 0x130000 j2me-go/build/j2me-go.bin
```

---

## Game Placement
Place your `.jar` and `.mjar` files on your MicroSD card under:
```
/sd/roms/jar/
├── Asphalt/
│   ├── Asphalt 6 - Adrenaline.jar
│   └── Asphalt Nitro.jar
├── Assassin's Creed/
│   └── Assassin's Creed - Brotherhood.jar
├── Gangstar/
│   └── Gangstar Rio - City of Saints.jar
└── Prince of Persia/
    └── Prince of Persia - The Forgotten Sands.jar
```
Cover art images (PNG format) can be placed in `/sd/romart/jar/` matching the exact name of the JAR file.

---

## License
J2ME-Go is licensed under the GNU General Public License v3.0 (GPL-3.0) with parts based on NanoJava and Retro-Go. See `LICENSE` for details.
