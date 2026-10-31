# Retro-Go Store (ESP32-S3)

An on-device ROM store for a [Retro-Go](https://github.com/ducalex/retro-go) fork. Browse your own
store over Wi-Fi, pick a game and it is saved to the SD card, with optional cover art.
The server side lives in [`store-backend`](../store-backend/README.md).

The launcher's **Store** tab starts this app (it is a separate OTA app, partition `store`).

## Contents

```
store/
├── CMakeLists.txt        app definition (needs RG_ENABLE_NETWORKING)
├── sdkconfig             ready-made ESP-IDF config for the store app
├── examples/store.json   template for the file you put on the SD card
└── main/
    ├── main.c            starts Wi-Fi, then opens the store
    ├── rom_store.c       UI, catalog and ROM downloads
    ├── rom_store.h
    ├── romart.c / .h     cover-art download and validation
    └── store_images.h    console artwork (embedded images)
```

## Installing into Retro-Go

Copy the `store` folder next to `launcher` in the Retro-Go root (it includes `../base.cmake`):

```
retro-go/
├── base.cmake
├── launcher/
├── store/          <- this folder
└── components/
```

Build and flash like any other Retro-Go app (see Retro-Go's `BUILDING.md`):

```
python rg_tool.py build store
python rg_tool.py flash store
```

Make sure `store` is in your app list (`DEFAULT_APPS` / `RG_TOOL_APPS` in `rg_tool.py`) and in
`partitions.csv` (the store slot is 0x120000 bytes in this project).

## Setting your store URL

The URL is **not compiled in**, so your personal address never ends up in the repository.
Create this file on the SD card (a template is written the first time you open the store):

`/sd/retro-go/config/store.json`

```json
{
    "url": "https://your-store.vercel.app/roms",
    "hide_unavailable": false,
    "sort": 0
}
```

`hide_unavailable` and `sort` are optional; the Store Settings screen adds them when you change
those options.

- The URL must include the `/roms` folder. The app requests `<url>/<console>/<console>.json`.
- A trailing `/` is ignored.
- Retro-Go also uses `store.json` for the store app's own settings, so it may add keys of its own.
  Your `url` is kept. Resetting settings from the launcher deletes the config folder, and with it this file.
- Keep this file out of git. The target `config.h` no longer has a store URL.

If the file is missing, empty or still has the placeholder, the store shows an alert and then opens
the on-device URL editor so it can be fixed without taking the SD card out. Cancelling that editor
returns to the launcher.

You can also edit the URL later: **Store Menu > Store Settings > Store URL**. The same settings
screen contains **Test connection**, **Hide unavailable systems**, and **Default sort**. Retro-Go
writes those keys back into `store.json`; the `url` key is preserved. Test connection requests the
selected system's catalog (for example `<url>/a52/a52.json`), not the bare `/roms` folder, because
the backend has no index there.

## Using it

| Where | Keys |
|-------|------|
| Console screen | Left / Right = change system, **A** = open, **B** = exit, **Menu** = store menu |
| Game list | Up / Down = move, Left / Right = page, **A** = install/reinstall, **B** = back, **Select** = search, **Menu** = game menu |
| Download | **B** = cancel and remove the incomplete `.part` file |

The game menu contains:

- **Install / Reinstall** - download the ROM (asks before overwriting an installed game).
- **Play** - shown for installed games when the system's app is available.
- **Fetch cover art** - shown for installed games that are not PICO-8 carts and do not have art yet.
- **Info** - local size, cover-art status and the exact SD path.
- **Delete from SD** - deletes the ROM and its cover art after confirmation.
- **Search / Filter**, **Clear filter**, and **Sort** (Newest, A-Z, Downloaded first).
- **Store Settings**, **System Options**, **Exit to Launcher**.

Games are saved to `/sd/roms/<console>/<file>`.

Downloads show a progress bar with the downloaded size, percentage, speed and ETA. The store checks
free space before writing, keeps the old file until the new one is complete, and offers a retry when a
download fails. After a successful download it asks whether to play the game now. PICO-8 carts are
stored as `.png` and never receive a separate cover-art file.

Wi-Fi is checked before every request and reconnected if it dropped. The store also starts the radio
itself if Wi-Fi is switched off in settings.

### Search, sorting and unavailable systems

The game list can be filtered by a substring of the file or display name. Sorting and the filter are
kept while the list is open; the sort choice is saved in `store.json`.

**Store Settings > Hide unavailable systems** hides systems that do not have a launcher tab in this
build (and systems whose app partition is missing). It defaults to **off** so new/store-only systems
remain visible; turn it on for a shorter carousel. Systems without embedded artwork are shown with a
stylized text card until an image is added to `store_images.h`.

### Cover art

After a game has downloaded, the store asks **"Download cover art for this game?"**.

- If art is already on the SD card it asks **"Cover art is already present. Download a new one?"** (default: no).
- Art comes from your store: `<url>/<console>/<game name>.png` (160x168 PNG, made with the
  *ROM Art Manager* of the backend). Nothing is converted on the device.
- It is saved next to the ROM as `/sd/roms/<console>/<game name>.png`, where Retro-Go looks for covers.
  `<game name>` is the ROM file name without its extension.
- The download goes to a temporary `.tmp` file and is checked to be a real PNG before it replaces anything,
  so a failed download never destroys the cover you already have.
- If the store has no art for that game you get "The store has no cover art for this game yet".
  Add it in the ROM Art Manager.
- `FF00FF` (magenta) pixels are transparent in Retro-Go, which is how the manager pads covers.
- PICO-8 carts are `.png` files themselves, so cover-art download is skipped for that system. This
  prevents the cover from replacing the cart.

## What the server must provide

```
<url>/<console>/<console>.json      [{"n": "Display Name", "f": "file name.ext"}, ...]
<url>/<console>/<file>              the ROM
<url>/<console>/<name>.png          optional cover art
```

The backend generates all of this for you. The catalog is read into a growable buffer (64 KB initial,
8 MB hard limit), so it is no longer silently truncated at 64 KB. Incomplete transfers are rejected
instead of being parsed. Entries without a usable file name are skipped.

## Supported systems

`nes`, `gb`, `gbc`, `gba`, `gw` (Game & Watch), `snes`, `gg`, `sms`, `md`, `sg1`, `ngp`, `pico8`, `lnx`,
`a26`, `ws`, `col`, `msx`, `pce`, `c64`, `a52` (Atari 5200), `a78` (Atari 7800), `jar` (J2ME),
`arcade` (MAME games) and `scummvm`. The server folder name must match the id.

MAME downloads to `/sd/roms/arcade`, which is the arcade ROM folder used by `mame-go`. The catalog
and cover art for it use the `arcade` server folder.

Systems without an embedded image (`a52`, `a78`, `jar`, `arcade`, `scummvm`) get a stylized text
card in the carousel. Add images to `store_images.h` and the relevant `store_consoles[]` entry when
they are available.

To add a system: add its image to `store_images.h` (or leave it NULL), add a line to
`store_consoles[]` in `rom_store.c`, create the folder on the server, and (for online cover search)
add it to `SYSTEMS` in the backend's `api/_lib/romart.js` and to the page's dropdown.

## Troubleshooting

Watch the serial log (`python rg_tool.py monitor`). Useful lines:

| Log line | Meaning |
|----------|---------|
| `Store URL loaded from store.json` | the URL was read correctly |
| `store.json has no "url" key, template written` | first run wrote a template; enter the URL on the device or edit the file |
| `HTTP 404 (/nes/nes.json)` | wrong URL: check the `/roms` suffix and the folder name |
| `HTTP 404 (/nes/Game.png)` | no cover art on the store for that game |
| `Wi-Fi not connected ... reconnecting` | the store is bringing Wi-Fi back up |

Only the request path is logged, never your host name, so logs are safe to share.

## Notes

- All Store code lives in this folder. The launcher only keeps the Store tab entry and its artwork.
  `RG_STORE_BASE_URL` no longer exists.
- Cover art is no longer fetched from third-party sites on the device. That work (search, resize, convert)
  moved to the backend, which is much faster than doing it on an ESP32.


## Stack overflow fix (ESP32-S3)

The captured crash explicitly reports `A stack overflow in task main` during `esp_http_client_open()` for the HTTPS catalogue request. This package raises both `CONFIG_ESP_MAIN_TASK_STACK_SIZE` and `CONFIG_MAIN_TASK_STACK_SIZE` from 8192 to 16384 bytes in the included `sdkconfig`. This is a targeted fix for the observed failure; it must be rebuilt and flashed to test.

After extracting, ensure the build uses this folder's `sdkconfig`. If your workflow uses ESP-IDF, run `idf.py reconfigure` and `idf.py build` from this project folder, then flash the Store app as you normally do. Check the resulting `sdkconfig` still has `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384`. Do not erase all flash or change the partition table.
