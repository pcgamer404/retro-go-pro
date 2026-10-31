# mame2000 for mame-go

Subset of [libretro/mame2000-libretro](https://github.com/libretro/mame2000-libretro)
(MAME 0.37b5), commit in `UPSTREAM_COMMIT`: only the files the selected
drivers need, found with `gcc -MM`.

**License:** MAME's own license (`readme.txt`): free for non-commercial
use, the full source of the port must be published, no commercial use
without the authors' written permission. This app is part of a
non-commercial project for that reason.

Local changes:
- `src/libretro/libretro.c`: with `-DMAMEGO` the game is chosen by
  `mamego_pick_driver()` (`mamego_pick.c`), which also accepts modern ROM
  sets (e.g. `pacman.zip` holding the Midway set, driver `pacmanm`).
- `mamego_driver.c` replaces `src/driver.c`; regenerate it with
  `python3 gen_drivers.py mamego_driver.c src/drivers/<file>.c ...` when a
  driver file is added.
- `src/mame.c`: with `-DMAMEGO` the "run at the single fixed-rate source's
  rate" shortcut is off. Pac-Man's Namco WSG is 48 kHz, the retro-go sink
  runs at 22050 Hz and paces the frame loop, so the shortcut dropped the
  game to 60 × 22050 / 48000 = 28 fps. The mixer resamples to 22050 instead.
- Save states: `mamego_state_*()` at the end of `src/cpuintrf.c` (generic
  8-bit snapshot: CPU contexts, IRQ state, CPU memory regions; refused when
  written by another firmware build), wired to `retro_serialize()` in
  `src/libretro/libretro.c`.
- `src/libretro/fileio.c`: `hiscore.dat` is read from the system directory
  (`/sd/retro-go/mame/mame2000/`); a copy of the upstream file is kept here
  (`hiscore.dat`). The `.hi` records go to `/sd/retro-go/saves/arcade/mame2000/hi/`.
- `src/vidhrdw/astrocde.c`: fast path in `wow_update_line()` for lines
  without sparkle (Robby Roto 28 -> 60 fps on the board, same frames).
- `src/drivers/circus.c`: 254 + 256 overlay colours instead of 254 + 32768
  (Circus 40 -> 57 fps, native; same frames).
  The timer scheduler is saved too (`mamego_timer_state()` in `src/timer.c`).
  Measured on the host: after a load the machine state stays byte-identical
  to the original run for 60 frames, then drifts at the scheduler's 1-second
  renormalisation (some absolute time outside the timer list is not saved
  yet) -- the game goes on correctly, only its random numbers differ.
- **Tile cache** (`src/drawgfx.c`, `GFX_TILE()` / `GFX_TILES()` in
  `src/drawgfx.h`): a gfx set larger than 256 KB decoded keeps its raw ROM
  and decodes tiles on demand into 192 KB of slots; tiles used in the
  current frame are never evicted. Tilemaps keep set + code per tile and
  look the pixels up again (`tile_pen_data()` in `src/tilemap.c`);
  multi-tile sprites get a contiguous run from a per-frame arena. Same
  frames as the fully decoded build (checked hash by hash on Blood Bros.,
  Aero Fighters, Pac-Man, 1943).
- **ROM regions in flash** (`mamego_regions_to_flash()` in `src/common.c`,
  called after driver init in `src/mame.c`): gfx and sound-sample regions
  of 256 KB and more move to the `mamerom` flash partition, memory-mapped
  (`mamego_flash_store()` in `main/main.c`, rewritten only when the content
  differs). Regions in flash are never written or freed
  (`MAMEGO_REGION_FREE`). The PC test build maps them read-only to catch
  a driver writing to them.
- **Idle-loop speed-ups** (`#ifdef MAMEGO` read handlers, MAME style): the
  68000 main loops that poll a vblank flag -- Aero Fighters
  `0B84: cmpi.b #1,$FF8055 / bcs` (`aerofgt_speedup_r`), Blood Bros.
  `0988: btst #7,$8004C / beq` (`bloodbro_speedup_r`) -- stop the CPU until
  the interrupt instead of emulating thousands of passes per frame. Found
  by counting executed PCs on the PC build; the 68000's work roughly
  halves, frames identical.
