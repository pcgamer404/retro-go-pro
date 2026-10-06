# RG Ports and the Port Slot

Run as many native Retro-Go ports as you like from the SD card, using **one shared flash partition**.

- **RG Ports** is a tab in the launcher. It lists port `.bin` files from the SD card and launches them like ROMs.
- **Port Slot** (`portslot`) is the single reserved partition those ports are flashed into when you launch them.

A port does not need its own partition any more. Adding or removing a port means copying or deleting a file on the SD card: no partition-table change, no firmware rebuild.

```
SD:/roms/rgports/openlara.bin  --Launch-->  flashed into portslot  -->  reboot into the port
SD:/roms/openlara/data/...                  (game files stay on the SD card)
```

---

## Contents
1. [Quick start](#quick-start)
2. [Getting a port's .bin file](#getting-a-ports-bin-file)
3. [Where files go](#where-files-go)
4. [Port slot size](#port-slot-size)
5. [Tested ports](#tested-ports)
6. [Features and game menu](#features-and-game-menu)
7. [Ports that need a special data path](#ports-that-need-a-special-data-path)
8. [Hiding or removing RG Ports](#hiding-or-removing-rg-ports)
9. [How it works](#how-it-works)
10. [Limits and known issues](#limits-and-known-issues)
11. [Troubleshooting](#troubleshooting)

---

## Quick start

**One-time setup**

1. In `rg_tool.py` (`PROJECT_APPS`) keep one slot entry and remove the old `rgports` app:
   ```python
   'portslot': [0, 16, 2097152],   # shared slot, currently 2 MB (see "Port slot size" to change it)
   ```
   Also remove any port you now run from the slot (`oregontrail`, etc.) to get its partition back.
2. Build and flash the **launcher** and **portslot** (`portslot` is a tiny placeholder app that reserves the partition). Flash the partition table too.
3. Create the folder `SD:/roms/rgports/`.

**For each port**

1. Get the port's `.bin` (see below) and copy it to `SD:/roms/rgports/`.
2. Put the port's game files in its usual folder under `SD:/roms/` (see [Where files go](#where-files-go)).
3. Open the **RG Ports** tab, select the game, press A, choose **Launch**.

---

## Getting a port's .bin file

Every Retro-Go port is an ordinary app. Build it the way you normally build an app (`rg_tool.py` or Retro-Go Manager's **Build**). The build produces the app image in that app's build folder:

```
<project>\<app>\build\<app>.bin          e.g.  openlara\build\openlara.bin
```

Copy that file to `SD:/roms/rgports/`.

- Use the **app** `.bin`. Do **not** use the combined flash image (`.img` from *Build Image*): it contains the bootloader and partition table and is rejected ("Not an ESP32-S3 app image").
- Build the port from the same Retro-Go tree and for the same target as your launcher.
- The `.bin` contains only the port's **code**. It does **not** include game files: those stay on the SD card (below).
- To update a port, rebuild it and copy the new `.bin` over the old one. The launcher notices the change and re-flashes automatically.

---

## Where files go

| What | Location |
|---|---|
| Port `.bin` files | `SD:/roms/rgports/` (subfolders are allowed) |
| Game data (WADs, PAK files, levels, assets, ...) | The port's usual folder, e.g. `SD:/roms/doom/`, `SD:/roms/quake/id1/`, `SD:/roms/openlara/data/` |
| Save states and saves | `SD:/retro-go/saves/` (managed by Retro-Go) |
| Covers (optional) | `SD:/roms/rgports/<name>.png` or `SD:/romart/rgports/<name>.png` |

The key rule: **the `.bin` carries code only. Put game files in the `SD:/roms/<port folder>/` the port normally uses**, exactly as if it were a normal tab. Nothing needs to be duplicated into `roms/rgports`.

---

## Port slot size

`portslot` is currently set to **2 MB** in `rg_tool.py`:

```python
'portslot': [0, 16, 2097152],   # size in bytes = 2 MB
```

Every port's `.bin` must fit inside it. The launcher refuses larger images with **"Too big for the port slot"**. If a port's `.bin` is bigger than 2 MB, raise the number in `rg_tool.py`:

| Slot size | Value (bytes) |
|---|---|
| 2 MB (current) | `2097152` |
| 3 MB | `3145728` |
| 4 MB | `4194304` |
| 6 MB | `6291456` |

Things to know when changing it:

- **Size it for your largest port.** Space the slot doesn't use is wasted: flash is a fixed 16 MB, so every extra MB comes out of what is left for other apps.
- **Keep it a multiple of 64 KB (65536)**, like the other entries in `PROJECT_APPS`.
- **Flash the new partition table, then `portslot`.** In the current list `portslot` is the last entry, so enlarging it does not move any other app. If you put apps after it, their offsets shift and you must reflash them too.
- Check the size of a port's image first: look at its `build/<app>.bin` file size.

---

## Tested ports

Tested and working through RG Ports:

| Port | Notes |
|---|---|
| Cannonball (OutRun) | |
| ClassiCube | |
| DOOM | |
| Quake | uses the data-path table (below) |
| OpenLara (Tomb Raider) | uses the data-path table (below) |
| Wolfenstein 3D | |
| Oregon Trail | |
| Rise of the Triad | |

**Not tested: Super Mario 64.** Its image is around 11 MB, which is more than the current 2 MB slot, so it keeps its own partition for now. A slimmed-down SM64 that reads most files from the SD card is planned.

---

## Features and game menu

- **One extra partition, any number of ports.** Only `portslot` is needed.
- **Its own tab** with Favorites and Recent support like every other tab.
- **Fast relaunch.** If the port is already in the slot, flashing is skipped. The launcher compares the file with what is really in flash (header + the image's trailing SHA-256), so it reads about 50 bytes instead of the whole file. It stays correct even if the slot was re-flashed by something else.
- **Automatic updates.** Copy a new `.bin` over the old one and the next launch re-flashes it.
- **Safe flashing.** Only ESP32-S3 app images that fit the slot are accepted. A failed or interrupted flash is never trusted, so the next launch flashes again.
- **Save states and SRAM** work for ports that support them, keyed per game.
- **Optional.** Hide the tab, leave out `portslot`, or compile it out entirely.

**Game menu (RG Ports tab only; every other tab keeps its normal menu):**

| Option | What it does |
|---|---|
| Resume game | Boots the port and loads a save state (greyed out if the port has none). |
| Launch | Starts the port. Flashes it into `portslot` first if the slot holds a different image. |
| Reinstall | Re-flashes the port even if the slot already holds it. Use after a bad flash. |
| Add / Del favorite | Same as other tabs. |
| Delete save | Deletes save states and SRAM. |
| Properties | Name, folder, size, CRC32 and **Delete file** (removes the `.bin`). |

Switching to a *different* port re-flashes it, which takes a few seconds (flash erase and write). Relaunching the same port is fast.

---

## Ports that need a special data path

Most ports find their data in their own folder (`SD:/roms/<port>/`). A few work out their data folder from the path they were launched with. Launched from RG Ports that path would be `roms/rgports/...`, so those ports would look in the wrong place.

For these, `launcher/main/rg_ports.c` has a small table, `DATA_PATHS`. It launches the port with a path inside its real folder:

| `.bin` file name | Launched with | Result |
|---|---|---|
| `openlara.bin` | `SD:/roms/openlara/openlara.bin` | OpenLara reads `SD:/roms/openlara/data/` |
| `quake.bin` / `quake-go.bin` | `SD:/roms/quake/id1/pak0.pak` | Quake reads its PAK files from `SD:/roms/quake/id1/` |

Names are matched without the extension, ignoring case. **If a port can't find its files, add one line to this table** and rebuild the launcher. Save states for those ports use the same path.

---

## Hiding or removing RG Ports

RG Ports is built into the launcher but is optional at three levels:

| Level | How | Effect |
|---|---|---|
| Hide the tab | Launcher menu, tab visibility list, untick **RG Ports** | Tab disappears. Fully reversible. |
| No slot partition | Don't add `portslot` to `rg_tool.py` | The tab never appears. |
| Remove the code | Build the launcher with `-DRG_ENABLE_PORTS=0`, or put `set(RG_ENABLE_PORTS 0)` above `rg_setup_compile_options` in `launcher/main/CMakeLists.txt` | No RG Ports code in the launcher at all. |

---

## How it works

1. You choose **Launch** on `SD:/roms/rgports/<name>.bin`.
2. The launcher validates the image (ESP32-S3 app image, fits the slot) and checks whether the same image is already in `portslot`.
3. If not, it erases and writes the image into `portslot` (`esp_ota_begin` / `esp_ota_write` / `esp_ota_end`).
4. It records the launch in Recent, then switches to `portslot` and reboots into the port.
5. The port runs as a normal Retro-Go app. On exit it returns to the launcher.

**Source files** (all in `launcher/main/`):
- `rg_ports.c`, `rg_ports.h`: everything RG Ports does (menu, slot flashing, data-path table).
- `applications.c`: three small hooks (include, a branch at the top of `application_show_file_menu()` for RG Ports files, tab registration).
- `CMakeLists.txt`: the `RG_ENABLE_PORTS` build switch.

**Shutdown workaround.** Before switching apps the launcher ticks the system monitor and waits 1.2 s. After a slow flash the monitor can think the launcher is unresponsive and draw on a display that is already shutting down, which crashes the launcher. If you fix `system_monitor_task` in `rg_system.c` so it checks `exitCalled` before drawing (`if (!exitCalled && rg_input_wait_for_key(RG_KEY_MENU, true, 1000))`), those two lines in `launch()` can be removed.

---

## Limits and known issues

- **Slot size.** A port's image must fit `portslot`, currently 2 MB. If a `.bin` is bigger, enlarge the slot in `rg_tool.py` ([Port slot size](#port-slot-size)). A very large port such as Super Mario 64 (about 11 MB) is better kept in its own partition until it is slimmed down.
- **Switching ports takes a few seconds** because the new image is flashed. Relaunching the current port is fast.
- **Ports must return to the launcher on exit** (the standard Retro-Go behavior).
- **Celeste:** one crash was seen once when loading a save state. It has not been reproduced. If it happens again, capture the monitor log from the Resume choice through to the reboot.
- The earlier `.rgport` package format was dropped. Only plain `.bin` ports are supported.

---

## Troubleshooting

| Message / symptom | Meaning and fix |
|---|---|
| Partition 'portslot' not found | `portslot` is not in the partition table. Add it and reflash the partition table. |
| Not an ESP32-S3 app image | The file is not an app `.bin` (wrong chip, or a combined `.img`). Use `build/<app>.bin`. |
| Too big for the port slot | The image is larger than `portslot` (currently 2 MB). Enlarge the slot in `rg_tool.py` ([Port slot size](#port-slot-size)) or use a dedicated partition. |
| Flash failed | Write error. Try **Reinstall**. |
| Port starts but can't find its files | The port derives its data folder from its launch path. Add it to `DATA_PATHS`, or check the files are in its `SD:/roms/<port>/` folder. |
| Port launches then returns to the launcher at once | Look at the monitor log. Often missing game files (the port exits) or a build for a different Retro-Go version. |
| Tab missing | `portslot` not flashed, the tab is hidden, or the launcher was built with `-DRG_ENABLE_PORTS=0`. |
