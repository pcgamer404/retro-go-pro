# Launcher

The Retro-Go launcher. It shows one tab per system or port, lists the games found on the SD card,
keeps **Favorites** and **Recent** lists, and boots the matching app when you pick a game.

- A tab only appears if the app it launches is present in flash, so removing an app from the firmware removes its tab.
- Any tab can be hidden from the launcher menu.
- Games live in `SD:/roms/<tab name>/` (for example `SD:/roms/nes/`). Optional cover images go next to the game
  (`<game>.png`) or in `SD:/romart/<tab name>/`.
- A built-in web interface is started when Wi-Fi is configured.
- Emulator and port cores are installed at runtime with **Core manager** (About Retro-Go), not from the
  partition table. See `../CORE_MANAGER.md`.

---

## Building

Build and flash the launcher like any other Retro-Go app (`rg_tool.py` or Retro-Go Manager).

---

## Source layout (`main/`)

| File | Purpose |
|---|---|
| `main.c` | Launcher entry point |
| `applications.c`, `applications.h` | System/app registry, game lists and the game menu |
| `core_pool.c`, `core_pool.h` | Core pool install/remove/select and the Core manager UI |
| `core_assets.h` | Core bundle metadata and artwork access |
| `bookmarks.c`, `bookmarks.h` | Favorites and Recent |
| `gui.c`, `gui.h` | Tabs, lists, backgrounds and headers |
| `images.c` | Built-in logos, banners and backgrounds |
| `webui.c`, `webui.h`, `webui.html.h` | Web interface |
