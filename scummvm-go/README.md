# ScummVM for Retro-Go (`scummvm-go`)

Classic LucasArts point-and-click adventures on a DIY **ESP32-S3** handheld running
[Retro-Go](https://github.com/ducalex/retro-go). Games can be played from a normal folder **or straight
from a `.zip` / `.scm` / `.svm` archive**, with no stub file needed.

This is a Retro-Go "port" app (same layout as the OpenTyrian port) that wraps a trimmed ScummVM snapshot
(SCUMM v0-v6 + Cine engines) with a custom OSystem backend.

> Status: working on hardware. Tested with the six games listed below.

## Features

- SCUMM engine (v0-v6) and Cine engine (Future Wars / Operation Stealth).
- Launch from the Retro-Go launcher as a **folder game**, a **`.scummvm` stub**, or a **ZIP archive** (`.zip`,
  or a ZIP renamed to `.scm` / `.svm`).
- ZIPs are unpacked once to the SD card and reused afterwards (second launch is almost instant).
- First-launch unpack shows a **progress bar**, and an interrupted unpack **resumes** on the next launch.
- Saves live outside the archive, so they survive cache clears.
- Retro-Go menu, volume, and save/load slots (map to ScummVM quicksave/quickload).

## Tested games

| Game | Version | Format tested |
|---|---|---|
| Indiana Jones and the Fate of Atlantis | CD, DOS, VGA | folder (large ZIP: see notes) |
| Indiana Jones and the Last Crusade | Floppy, DOS, VGA | folder, ZIP |
| Maniac Mansion | Floppy, DOS, v2 Enhanced | folder, ZIP |
| Monkey Island 2: LeChuck's Revenge | Floppy, DOS, VGA | folder, ZIP |
| The Secret of Monkey Island | Floppy, DOS, EGA | folder, ZIP |
| The Secret of Monkey Island | Floppy, DOS, VGA | folder, ZIP |

You must supply your own game data. No game files are included.

## Installation

1. Copy the `scummvm-go` folder into your Retro-Go `retro-go/` source tree so it sits next to the other apps.
2. Register the app in `components/retro-go/...` / `launcher/main/applications.c`:

   ```c
   application("ScummVM", "scummvm", "scummvm svm scm zip", "scummvm-go", 0);
   ```

   Only add `zip` if you want every `.zip` file in that folder to open in ScummVM.
3. Make the app partition **2 MB** in `rg_tool.py` (the build is just under that):

   ```python
   "scummvm-go": 0x200000,   # launcher 0x130000, scummvm-go 0x200000, portslot 0x500000 (my-handheld)
   ```

   Adjust offsets for your own partition table.
4. Build and flash:

   ```bash
   python rg_tool.py build scummvm-go --target my-handheld
   python rg_tool.py build-fw --target my-handheld   # or flash as you usually do
   ```

   Toolchain used: ESP-IDF 5.3.x.

## Putting games on the SD card

Pick whichever is easier:

**A. ZIP / SCM (recommended: one file per game)**

Zip the game files and put the archive in the ScummVM ROM folder, for example:

```
/sd/roms/scummvm/Monkey Island 2.zip
/sd/roms/scummvm/Maniac Mansion.scm        <- same thing, just renamed
```

The game files can be at the top of the ZIP, or inside a single top-level folder. Use normal **Deflate** or
**Store**. ZIP64 and Deflate64 are not supported (see Troubleshooting).

**B. Folder**

```
/sd/roms/scummvm/Monkey Island 2/MI2.LA0 ...
/sd/roms/scummvm/Monkey Island 2.scummvm   <- empty stub file; launch this
```

### Where things are stored

| What | Where |
|---|---|
| Unpacked ZIP games | `/sd/retro-go/cache/scummvm/<zip name>/` |
| Saves + `scummvm.ini` for ZIP games | `/sd/retro-go/saves/scummvm/<zip name>/` |
| Saves for folder games | `<game folder>/saves/` |

The first launch of a ZIP unpacks it to the cache folder. Delete a folder from `/sd/retro-go/cache/scummvm/` to
reclaim space (it will simply be unpacked again next time). If you replace a ZIP with a different one of the
same name, the cache is detected as stale (size + modification time) and rebuilt automatically.

> Upgrading from an earlier build that used `/sd/scummvm/`? Move `/sd/scummvm/saves/*` to
> `/sd/retro-go/saves/scummvm/` to keep your saves, and delete `/sd/scummvm/cache/` (it is rebuilt on demand).

## Controls

| Button | Action |
|---|---|
| D-pad | Move cursor (accelerates; hold **Select** for precision) |
| A | Left click |
| B | Right click |
| Start | Esc |
| X | `.` (skip dialogue line) |
| Y | Space (pause) |
| L | Quicksave (Alt+1) |
| R | Quickload (Ctrl+1) |
| Menu / Option | Retro-Go game / options menu |

Options menu: **Cursor speed** (Slow / Normal / Fast) and **Sound** on/off (restart game to apply).

## Performance notes

- **First launch of a ZIP is slow** because every file is inflated and written to the SD card. Time is
  dominated by SD write speed and game size; CD games with big speech files take the longest. A progress
  bar shows the percentage and MB done so the device doesn't look frozen.
- If you switch the device off during the unpack, launch the game again. It resumes where it stopped
  (finished files are skipped).
- Later launches reuse the cache and start quickly.
- A faster / bigger SD card helps. Pre-extracting to a folder (option B) avoids the one-time wait.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| "Could not read the ZIP file" | File is damaged or not a ZIP. Re-create it. |
| "ZIP unpack failed ..." | SD card full, or the ZIP uses ZIP64 / Deflate64 / unsafe paths. Re-zip with 7-Zip using *Deflate* (not Deflate64) or *Store*, free space, launch again to resume. |
| "No supported game found" | Game files are not at the root of the ZIP/folder (or one top-level folder), or the version isn't supported by SCUMM/Cine. |
| Large CD game ZIP (e.g. Fate of Atlantis CD, ~60 MB zipped) doesn't start | Unpacked size is several hundred MB, so unpacking takes a long time and needs the space on the SD card. Earlier builds gave no feedback and no resume; now there is a progress bar and resume. If it still fails, copy the serial log (`idf.py monitor`) from the `scummvm:` lines and open an issue. Using the folder form always works as a fallback. |
| Game stops responding for a while on first launch | Normal during unpack; with the current build the progress screen should be visible. |

## What was done (development log)

Starting point: [johannesbehr/scumm](https://github.com/johannesbehr/scumm), a ScummVM snapshot for the
ESP32 / Odroid-Go, plus an OpenTyrian port for Retro-Go used as the app template.

1. **Retro-Go app layout.** `main/main.c` (rg_system init, options, menu handlers), ScummVM merged into one
   component `components/scummvm`, 2 MB partition entry, `application(...)` registration.
2. **Custom OSystem backend** (`rg/osystem_rg.cpp/.h`) replacing the Odroid backend: 8-bit paletted 320x200
   surface, software cursor drawn from the game's cursor image, mixer on core 1 via an audio task, real FreeRTOS
   mutexes, timers pumped from the engine thread, input mapping, quicksave/quickload hooks.
3. **C bridge** (`rg/rg_glue.c/.h`, `rgx_*` functions). ScummVM and Retro-Go headers clash (`MIN`, `MAX`, `byte`,
   `uint`), so only this pure-C glue sees Retro-Go headers.
4. **Launcher** (`rg/scummvm_run.cpp`): game detection (SCUMM, then Cine), config domain setup, engine run.
5. **Build fixes** (ESP-IDF builds with `-Werror=all`): missing FreeRTOS includes, `engines/obsolete.h` include
   order, and per-warning `-Wno-error=...` flags (a plain `-Wno-error` does not undo `-Werror=all`).
6. **ZIP support**, which needed three separate fixes:
   - *Crash:* the original code opened the archive through `Common::FSNode`, which asserts `g_system` before the
     backend exists. Replaced with a plain stdio stream.
   - *Link error:* `unz_file_info` is an anonymous struct, so the header could not name it. Added a matching
     forwarding overload.
   - *"failed opening ZIP member":* `USE_ZLIB` is not defined on the device, so unzip had no inflate. Added
     `rg/rg_inflate.c`, a streaming wrapper around the ESP32-S3 **ROM miniz `tinfl_decompress`** (no extra
     flash), hooked into `common/unzip.cpp`. CRC32 of compressed members is deliberately not verified.
   - Extraction is hardened: rejects `..`, absolute and drive paths, converts backslashes, ignores `__MACOSX`
     and dot files, handles a single wrapping folder, and writes the cache marker last.
7. **Performance / usability pass** (latest):
   - Progress bar screen during unpack (drawn on the game's own 8-bit surface; no extra Retro-Go APIs).
   - Resumable unpack: `.rgzip.part` marker holds the archive signature; finished files are skipped.
   - Bigger copy buffers (32 KB with fallback), stdio read buffering, no-op seek skipping.
   - Error alerts on failure instead of silently exiting.
8. **Host-side test harness** (not in the repo): the archive helpers and the real `unzip.cpp` + miniz were built
   on a PC and tested with flat, wrapped, backslash, malicious, non-ZIP, long-name, empty and 380 MB archives
   (including MD5 checks, random read sizes, stale cache replacement and resume).

## Known limitations

- **No music.** AdLib/OPL and MIDI music are not implemented in this build; digital sound effects and speech
  go through the mixer. Wiring an OPL emulator (for example the one in OpenTyrian) is the logical next step.
- No overlay, so ScummVM's own dialogs (Global Main Menu, error boxes) aren't shown; use the Retro-Go menus.
- Only 8-bit paletted games; engines other than SCUMM and Cine are not enabled.
- ZIP64 / Deflate64 archives are not supported.
- ZIP member CRCs are not checked.
- `-std=gnu++11` is used because this ScummVM snapshot predates C++17.

## Credits and license

- [ScummVM](https://www.scummvm.org/) team (GPL-2.0-or-later).
- [johannesbehr/scumm](https://github.com/johannesbehr/scumm) ESP32 snapshot.
- [Retro-Go](https://github.com/ducalex/retro-go) and the OpenTyrian Retro-Go port (app template).
- [miniz](https://github.com/richgel999/miniz) (via the ESP32-S3 ROM) for inflate.
- Port work with help from Claude and ChatGPT.

Licensed under **GPL-2.0-or-later**, same as ScummVM. LucasArts / Disney game data is not included and not
covered by this license.
