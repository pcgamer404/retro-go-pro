# gwenesis (Sega Genesis / Mega Drive)

A Sega Genesis / Mega Drive emulator for Retro-Go, built on
[gwenesis](https://github.com/bzhxx/gwenesis) by bzhxx (GPLv3).

The core is a full 68000 + Z80 + VDP + YM2612 + SN76489 emulator. On the
ESP32-S3 handheld it runs most games at a playable speed with sound; the
heaviest titles fall back to the automatic frameskip described below.

```
ROM format : .md / .gen / .bin (SMD images are detected and de-interleaved on
             load if given a .bin name), .zip
Region     : taken from the ROM header (PAL 50 Hz or NTSC 60 Hz)
Display    : 320x240 (H40) or 256x240 (H32), PAL8 palette surfaces
Audio      : 32 kHz, mono source mixed to stereo
Save states: yes (Retro-Go save slots), 6 slots + resume
```

## Controls

| Handheld | Genesis |
|---|---|
| D-pad | D-pad |
| A | A |
| B | B |
| X | C |
| Y | C (second C button, for comfort) |
| Start | Start |
| Select | Retro-Go menu (not passed to the game) |

Three Genesis buttons (A/B/C) are available; there is no 6-button pad mode.
Some fighting games expect X/Y/Z and will only see three buttons.

### Options menu

* **Sound** – one switch for the whole audio path (YM2612 FM, SN76489 PSG and
  the Z80 that drives them). Default On. Turning it off frees the Z80 and FM
  synthesis time, which is a large part of the frame.
* **Swap A/C** – for games that map "jump" to A or that feel better with the
  Western layout. When On, physical A becomes Genesis C and X/Y become
  Genesis A; B is unchanged. The setting is persistent.

## Performance

The ESP32-S3 runs at 240 MHz and has to emulate a 7.67 MHz 68000 plus a
3.58 MHz Z80, so the frame budget is tight: a PAL frame is 20 ms and an NTSC
frame is 16.7 ms.

Emulation is split over both cores:

* **Core 0** runs the 68000, the Z80, and the sound-register shadow. It also
  owns the frame: it decides when a frame is drawn and hands the finished
  picture to the display task.
* **Core 1** runs two worker tasks. The YM2612 synthesiser replays the frame's
  register log into a double buffer, and the VDP scanline renderer draws line
  *N* while core 0 emulates line *N+1*.

Typical measured frame costs (NTSC, `Contra - Hard Corps`, `PERF` output):

| stage | core | time |
|---|---|---|
| 68000 | 0 | 8–11 ms |
| Z80 | 0 | 0.1–5 ms (depends entirely on the sound driver) |
| YM2612 synthesis | 1 | ~7 ms |
| VDP render (per drawn frame) | 1 | 12–15 ms |
| display submit | 1 | ~0.5 ms |

The two cores are close to balanced, which is why the frame rate depends on the
game rather than on one hot spot. A title whose sound driver leaves the Z80
halted spends almost nothing on core 0 and reaches a full 60 FPS; a title whose
driver polls the Z80 hard can add ~5 ms to core 0 and drop it to ~50 FPS.

The app runs its own adaptive frameskip: when a frame overruns the deadline, or
the display task has not finished the previous frame, the next frame is
emulated but not rendered. Game logic and sound always run at full speed, so
gameplay timing and music tempo stay correct; only the picture updates less
often.

### Where the time goes

FM synthesis used to cost ~10 ms of core 1 per frame — as much as the renderer —
and the 24-slot SSG-EG update pass ran on every sample even though almost no
Mega Drive title enables SSG-EG at all. It now tracks a count of slots with
SSG-EG enabled and skips the pass when that count is zero. Measured on the host
harness that removes 28% of all YM2612 time (117 → 84 cycles per sample), and it
is bit-exact: the generated audio is identical sample for sample.

What remains is genuine FM work — six channels of four operators, phase
accumulators, envelope generators and the LFO — and cannot be removed without
changing the sound. Lowering the internal 53,267 Hz sample rate to the 32 kHz
the audio sink uses would save another 40%, but it audibly degrades FM quality,
so it is deliberately not done.

`PERF emu=… core=… vdp=…` is printed to the log every 120 frames:

| field | meaning |
|---|---|
| `emu` | whole emulated frame, core 0 side (`core` + `wait`) |
| `core` | core-0 CPU: 68000 + Z80 + sound shadow |
| `cpu68`, `z80` | split of the above |
| `vdpd` | render cost of one drawn frame, measured on core 1 |
| `wait` | time core 0 spent waiting for the renderer |
| `total` | true frame period; `16667` means a locked 60 Hz |
| `draw` | frames rendered in the last 120-frame window |

`vdp` and `wait` are accumulated only on drawn frames but divided by 120, so
multiply them by `120/draw` to get a per-drawn-frame figure.

## Known limitations

### Games that need extra hardware

* **Virtua Racing** does not work. The cartridge contains a **Sega Virtua
  Processor (SVP)**, a 32-bit RISC co-processor with its own DRAM that draws
  the 3D polygons and writes the resulting bitmap into VRAM. The 68000 only
  uploads code and commands to it. There is no SVP emulation here, so the menus
  (plain 68000/VDP work) look fine but everything the SVP is supposed to draw
  is missing, which shows up as a garbled or mostly blank picture.
* **Virtua Racing** is the only retail cartridge with an SVP. Other
  coprocessor/expansion formats are also unsupported:
  * Sega CD / Mega-CD (needs a second 68000, CD-ROM, PCM chip, ASIC)
  * 32X (needs two SH-2s and extra VDPs)
  * Sega Pico, LaserActive, and the modem/online cartridges

### Emulation gaps

* **Battery-backed saves are not emulated.** Games that save to SRAM/EEPROM
  (RPGs, sports season modes, high-score tables) will not keep that data between
  sessions. Retro-Go save states do work and capture SRAM contents, but only
  while the state is loaded.
* **Interlace mode is not implemented.** The handful of games that switch to
  interlaced 480-line output (mainly 2-player modes in a few titles) will show
  a broken picture while that mode is active.
* **No 6-button controller**, so games that read X/Y/Z see a 3-button pad.
* The YM2612 is emulated at sample level with the FM operators and DAC, but it
  is not a cycle-exact model of the chip: very demanding sound drivers may have
  small pitch/envelope differences. FM synthesis runs on the second core.
* Sprite-per-line limits and the sprite overflow/collision flags are emulated,
  but the VDP is a scanline renderer rather than a dot-exact one, so a few
  raster tricks (mid-line register changes) are approximated.

### Cosmetic

* On some units a faint shimmer is visible in large dithered areas (for
  example the HUD panel in cricket games). This was investigated at length:
  the emulator produces byte-identical frames for those regions, and the
  palette is copied per frame, so it is a panel/SPI artefact rather than an
  emulation bug. It does not affect gameplay.

## Building

This is a Retro-Go app. It expects the shared `testing/components/retro-go`
component and the usual ESP-IDF environment:

```powershell
. C:\Espressif\frameworks\esp-idf-v5.3.5\export.ps1
ninja -C build
```

The app fits the 0x120000 `gwenesis` partition (about 1.09 MB used). It is
built with `-O3` for the emulator core and the app; the scanline renderer is
placed in IRAM because it runs once per line and would otherwise be evicted
from the shared instruction cache by the CPU interpreters.

Cache sizes are set in `sdkconfig.defaults` (and mirrored in `sdkconfig`, which
ESP-IDF does not re-read defaults into once it exists):

| Option | Value | Effect |
| --- | --- | --- |
| `CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE` | 16 KB (default) | no measurable effect; raising it to 32 KB changed `cpu68` by less than run-to-run noise |
| `CONFIG_ESP32S3_DATA_CACHE_SIZE` | 64 KB | worth roughly 10% of total frame time |

The data cache is the one that matters. `m68ki_cycles` is a 64 KB table in
flash DROM indexed by the current opcode, i.e. one random read per instruction.
At 32 KB it could not stay resident and missed almost every time; at 64 KB it
mostly fits, which took the worst-case `total` from about 21.7 ms to 19.6 ms.
The 256 KB `m68ki_instruction_jump_table` is far too large to cache in either
configuration, and the 64 KB `M68K_RAM` never enters the cache at all because
it lives in internal DRAM.

## Layout

```
main/main.c                     app loop: frame pacing, frameskip, input, audio,
                                display buffers, save states
components/gwenesis/src/bus     memory map, cartridge loading, region detect
components/gwenesis/src/cpus    Musashi 68000, Z80 interpreter
components/gwenesis/src/vdp     VDP registers, VRAM/DMA, scanline renderer
components/gwenesis/src/sound   YM2612 (+ worker task), SN76489, Z80 glue
components/gwenesis/src/io      controller ports
components/gwenesis/src/savestate  chunked save-state serialisation
```

## Credits

* gwenesis by bzhxx – <https://github.com/bzhxx/gwenesis>
* Musashi 68000 core (Karl Stenerud)
* Z80 emulator by Marat Fayzullin
* YM2612 emulation derived from the MAME/Genesis Plus lineage
* Retro-Go by ducalex

GPLv3. See `components/gwenesis/LICENSE`.
