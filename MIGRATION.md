# Retro-Go Pro: merged cores (retro-core / atari-core / retro-home / retro-extra / arcade3d)

**Status: not compiled.** I had no ESP-IDF toolchain, so none of this has been built or run. Everything was ported by
reading both repos against your `Components/retro-go` API. Expect to fix a few compile errors on the first build; the
spots most likely to need attention are listed at the end.

## 1. Apply (this is the FULL tree)

This zip is your complete uploaded tree with everything merged: unzip it over `D:\Games\E\retro-go-pro`, allowing overwrite.
Old folders `c64-go/`, `stella/`, `fmsx/`, `retro-core/components/handy/` and `retro-core/main/main_lynx.cpp` are already removed here, so delete them from your disk too
(unzipping does not delete). Not included because they were not in your upload: `base.cmake`, `rg_tool.py`, the launcher, partitions/sdkconfig. Do sections 3-5 by hand.

Delete (they moved):
```
c64-go/                      -> retro-home/components/frodo + retro-home/main/main_c64.cpp
stella/                      -> atari-core/components/StellaDS + atari-core/main/main_a26.cpp
fmsx/                        -> retro-home/components/fmsx + retro-home/main/main_msx.c
retro-core/components/handy/ -> atari-core/components/handy
retro-core/main/main_lynx.cpp-> atari-core/main/main_lynx.cpp
```
Keep your `retro-core/components/launcher` stub, `base.cmake` and `rg_tool.py` (not in the upload, so I could not edit them: sections 3-5).

## 2. New layout

| App (partition) | Systems (launcher tab id) | Cores |
|---|---|---|
| `retro-core` | NES/FDS `nes`, GB/GBC `gb`/`gbc`, PCE `pce`, SMS `sms`, GG `gg`, Coleco `col`, **SG-1000 `sg1`**, G&W `gw`, **SNES `snes`** | nofrendo, fceumm, gnuboy, pce-go, smsplus, gw-emulator, **snes9x** |
| `atari-core` | Lynx `lnx`, 2600 `a26`, 5200 `a52`, 7800 `a78` | handy, StellaDS, atari5200, prosystem |
| `retro-home` | MSX `msx`, C64 `c64` (ZX Spectrum etc. later) | fmsx, frodo |
| `retro-extra` | NGP/NGPC `ngp` | race |
| `arcade3d` | standalone game port | arcade3d |

Each app has a README with controls, BIOS needs and notes.

## 3. rg_tool.py
Add `retro-home`, `atari-core`, `arcade3d`, `retro-extra` to `PROJECT_APPS` by copying your existing `retro-core` entry (same
format), and remove `c64-go`, `stella`, `fmsx`. Your `retro-core` entry may need a larger size (snes9x was added). Build each
once and look at the size of `build/<app>.bin` to set the partition sizes; I could not measure them.

## 4. Partitions
Partition/app names must match the app names above. If you use RG Ports for `arcade3d`, build `arcade3d.bin` and use it as a raw
port `.bin` instead of giving it its own partition.

## 5. Launcher registration
Not visible to me, so adapt to your launcher source. Pattern: system name, tab id (folder under `roms/` and `configNs`), extensions, **app/partition**:
```
Atari Lynx        lnx   -> atari-core      Atari 2600  a26 -> atari-core
Atari 5200        a52   -> atari-core      Atari 7800  a78 -> atari-core
MSX               msx   -> retro-home      Commodore 64 c64 -> retro-home
Sega SG-1000      sg1   -> retro-core      SNES        snes -> retro-core
Neo Geo Pocket    ngp   -> retro-extra     Arcade 3D   (port entry) -> arcade3d
```
The dispatch in each `main.c` also accepts the old names (`stella`, `fmsx`, `c64-go`, `sg`, `sfc`, `ngpc`), so a launcher that still passes those keeps working.

## 6. `rg_system.c` is NOT modified
`rg_system_reinit()` applies the *previous* sample rate to the audio sink before storing the new one. Rather than touching it, the cores that run at
a rate different from the 32000 Hz used in `main.c` (StellaDS 22050, Frodo 22050, ProSystem ~31440; fMSX for safety) call
`rg_audio_set_sample_rate(rate)` themselves right after `rg_system_reinit()`. Your existing cores are unaffected.

## 7. SG-1000: what was wrong
1. **Dispatch**: neither `main.c` handled an `sg1` tab, so it fell through to the launcher stub and exited. Fixed (`sg1`, `sg`, `sg1000`).
2. **Console selection** used only the `.sg` extension, so zipped SG-1000 ROMs were run as Master System. Now chosen by tab id as well.
3. **Input**: SG-1000 fell into the ColecoVision branch: START opened the Coleco keypad prompt and muted audio. Now START = PAUSE (NMI, the "start" of most games), SELECT = reset.
4. **ROM mapping**: 8K/16K ROMs were padded to 16K but the SG-1000 maps 32K at `$0000`; the upper half read past the buffer. They are now mirrored over 32K (`loadrom.c`).
Not verified with a ROM; if a specific game still fails tell me which and I will trace it. Files: `retro-core/main/main.c`, `main_sms.c`, `components/smsplus/loadrom.c`.

## 8. pjcau -> yours
Incorporated: Lynx BS93 homebrew header fix (`handy/ram.h`), snes9x + SNES front end, RACE (NGP), ProSystem (7800), Atari 5200, Arcade 3D.
Port changes: `rg_system_init(rate, handlers)` -> your `rg_config_t` / `rg_system_reinit`; `rg_display_sync(false)` -> `!rg_display_is_busy()` (inverted meaning handled);
palette surfaces use your `RG_SCREEN_PIXEL_FORMAT` switch (BE/LE) like your other cores; `rg_storage_fread` (loading %) -> `fread`; debug-HUD call stubbed out.
Not taken: pjcau's `retro-go` library changes (SD-app install, debug HUD, band display submit, 16-bit indexed surfaces, load-progress), its PCE restructure (yours is newer),
its Stella, Teensy64 C64 (your Frodo is better) and fMSX (identical). Your own extras (map165, SMS cheats, FCEUmm, G&W) are untouched.

## 9. Watch on first build
- `retro-core` size with snes9x (partition); snes9x needs PSRAM and starts two helper tasks on core 1.
- `atari-core` and `retro-home` link several large cores together for the first time; a rough scan found no duplicate global names, but the linker has the last word.
- `main_a26.cpp` / `main_c64.cpp`: first `rg_system_init` now happens in `main.c` with `frameRate 60`; each core then sets its own tick rate (Frodo 50, fMSX 55, Stella from the ROM).
- Settings namespaces follow the tab id: saves from the old standalone `stella` / `c64-go` folders must be renamed to `a26` / `c64` to be found.

## 10. NES / FCEUmm changes (added later)
Files: `retro-core/main/main_fceumm.c`, `main_nes.c`, `components/fceumm/src/{cheat.c,cheat.h,fceu.c,fceu-cart.c,ppu.c,input.c}`.
See the "NES: frameskip cap and Game Genie" section of `retro-core/README.md`. If the link fails with an `iram0_0_seg` overflow, remove the
`IRAM_ATTR` I added in `fceu.c` / `fceu-cart.c` / `input.c` / `ppu.c` (about 1-2 KB in total).

## 11. Other cores: abnormalities found and fixed (all inside the cores, `rg_system.c` untouched)
- **Frameskip cap everywhere**: new `main/core_frameskip.h` (copied into retro-core, atari-core, retro-home, retro-extra). Every core now reads the auto-frameskip through
  `core_frameskip()`, capped by the **Max frameskip** option (default 2, stored per system). Cores that had no options menu (5200, 7800, NGP) get one with just that row.
  SNES keeps its tuned baseline of 3 as the lowest ceiling; C64 uses the cap but has no menu row.
- **GB/GBC and Lynx drew only every other frame**: retro-go's default `frameskip` is 1 and auto-frameskip never goes below 1, so unless a core sets 0 (NES, SMS, C64 do) it renders at half rate. Both now start at 0.
- **PC Engine**: forced `frameskip = 1` (always every other frame) and its own late-frame code did `skipFrames++` without limit. Baseline 0, and the late-frame skip is capped.
- **Lynx**: `AUDIO_SAMPLE_RATE / (gAudioBufferPointer / 2)` divided by zero when a frame produced no audio samples (hard crash). Guarded.
- Known remaining limitation, in `rg_system.c` (not changed): changing the speed option or the overclock level sets `frameskip` back to 1, which loses a core's 0 baseline until
  auto-frameskip recomputes. The cap still applies.
