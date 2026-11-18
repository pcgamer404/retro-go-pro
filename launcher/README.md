# Launcher

The Retro-Go launcher. It shows one tab per system or port, lists the games found on the SD card,
keeps **Favorites** and **Recent** lists, and boots the matching app when you pick a game.

- A tab only appears if the app it launches is present in flash, so removing an app from the firmware removes its tab.
- Any tab can be hidden from the launcher menu.
- Games live in `SD:/roms/<tab name>/` (for example `SD:/roms/nes/`). Optional cover images go next to the game
  (`<game>.png`) or in `SD:/romart/<tab name>/`.
- A built-in web interface is started when Wi-Fi is configured.

---

## RG Ports (port slot)

RG Ports is a built-in tab for **native ports** (DOOM, Quake, OpenLara, Wolfenstein 3D, Cannonball, ClassiCube,
Rise of the Triad, Oregon Trail, ...) that would otherwise each need their own flash partition.

Instead, all of them share **one partition, `portslot`**. Ports are plain app `.bin` files kept on the SD card in
`SD:/roms/rgports/`. When you launch one, the launcher flashes it into `portslot` (skipped if it is already there)
and reboots into it. Adding or removing a port is just copying or deleting a file: no partition-table change and no
firmware rebuild. Game data stays in each port's normal folder under `SD:/roms/`.

```
SD:/roms/rgports/doom.bin  --Launch-->  portslot  -->  reboot into DOOM
SD:/roms/doom/...                       (game files stay on the SD card)
```

**Quick setup**
1. In `rg_tool.py` keep one entry: `'portslot': [0, 16, 2097152]` (2 MB; increase it if a `.bin` is bigger).
2. Flash the launcher and the `portslot` placeholder app, and flash the partition table.
3. Copy each port's `build/<app>.bin` to `SD:/roms/rgports/`, open the **RG Ports** tab and press A on a game.

**RG Ports game menu** (only on the RG Ports tab, every other tab keeps its normal menu):
Resume game, Launch, Reinstall, Add/Del favorite, Delete save, Properties (with Delete file).

**Optional:** hide the tab from the launcher menu, leave out `portslot` and the tab never appears, or build the
launcher with `-DRG_ENABLE_PORTS=0` to remove the code completely.

**Full guide:** how to get a port's `.bin`, where files go, tested ports, changing the slot size, ports that need a
special data path, how it works and troubleshooting are in **[RG-PORTS.md](RG-PORTS.md)**.

---

## Building

Build and flash the launcher like any other Retro-Go app (`rg_tool.py` or Retro-Go Manager). Build options:

| Option | Default | Effect |
|---|---|---|
| `RG_ENABLE_PORTS` | `1` | `0` removes RG Ports from the launcher. Pass `-DRG_ENABLE_PORTS=0` to the build, or `set(RG_ENABLE_PORTS 0)` in `main/CMakeLists.txt`. |

---

## Source layout (`main/`)

| File | Purpose |
|---|---|
| `main.c` | Launcher entry point |
| `applications.c`, `applications.h` | System/app registry, game lists and the game menu |
| `rg_ports.c`, `rg_ports.h` | **RG Ports**: its game menu, slot flashing and data-path table. Self-contained |
| `bookmarks.c`, `bookmarks.h` | Favorites and Recent |
| `gui.c`, `gui.h` | Tabs, lists, backgrounds and headers |
| `images.c` | Built-in logos, banners and backgrounds |
| `webui.c`, `webui.h`, `webui.html.h` | Web interface |

RG Ports touches `applications.c` in three places only: the include, a branch at the top of
`application_show_file_menu()` for RG Ports files, and the tab registration. All other tabs behave as before.
