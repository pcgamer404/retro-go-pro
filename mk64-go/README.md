# MK64-Go

A native **Mario Kart 64 port for Retro-Go on ESP32-P4 and ESP32-S3**, based on
the [n64decomp/mk64](https://github.com/n64decomp/mk64) decompilation and the
**MK64 Portable** PSP port. The game code runs directly on the device, with
Retro-Go providing display, controls, storage and system menus.

This is a work-in-progress port. The primary test device is **GB300-P4 with
32 MB PSRAM**. The **ESP32-S3** build (8 MB PSRAM) keeps the extracted assets
in the tail of a **12 MiB app partition** and maps them read-only from flash,
so it does not need a large PSRAM asset buffer. 16 MB P4 devices and the
original ESP32 are not currently supported.

## Setup

1. Install a Retro-Go firmware build that includes `mk64-go`.
2. Copy your **Mario Kart 64 (USA/NTSC)** ROM to `roms/mk64/` on the SD card.
3. Open **Mario Kart 64** in the launcher and select your ROM.
4. Allow the first-run asset extraction to finish.

The ROM filename is up to you. Uncompressed `.z64`, `.n64` and `.v64` files
are supported; `.z64` is recommended. ZIP files and PAL ROMs are not supported.
The accepted USA ROM has this SHA-1 after conversion to `.z64` byte order:

```text
579c48e211ae952530ffc8738709f078d5dd215e
```

**You do not need to compile the game with your ROM or extract assets on a PC.**
Supply your own ROM; game assets are not included with the port.

### SD card layout

For a ROM named `Mario Kart 64.z64`, the default layout is:

```text
SD card/
├── roms/
│   └── mk64/
│       └── Mario Kart 64.z64             # Your ROM
└── retro-go/
    ├── cache/
    │   └── mk64/                         # Generated automatically
    └── saves/
        └── mk64/
            ├── Mario Kart 64.z64.sram    # Native game progress
            ├── Mario Kart 64.z64.sram.bak
            └── Mario Kart 64.z64.png     # Saved screenshot
```

Paths follow Retro-Go's configured storage layout. You only need to copy the
ROM; the port creates its cache and save files.

## How it works

On launch, the port verifies the ROM and prepares the required assets using
extraction recipes inherited from the PSP port. On **ESP32-P4** the extracted
data is cached on the SD card and loaded into PSRAM. On **ESP32-S3** the first
launch extracts about **10.6 MB** into the tail of the 12 MiB app partition and
maps it read-only from flash; later launches only recreate the mapping. In both
cases the decompressed MIO0 blocks are cached on the SD card.

**Keep the ROM on the SD card:** some textures are still read from it while
playing. On P4, to rebuild extracted assets, delete only `retro-go/cache/mk64/`
and launch again. Your progress lives separately in `retro-go/saves/`.

On S3 the flash asset store is independent of the SD cache. To force a rebuild,
erase the app partition tail (or the whole flash) before launching again;
deleting only the SD cache rebuilds the MIO0 cache.

The software renderer draws gameplay at **160×120**, with menus and the
single-player HUD rendered at **320×240** where enabled. Retro-Go scales the
output to the device display. Game updates target **30 per second**; rendering
is skipped when necessary so controls and physics keep advancing. Some loading
pauses remain. The 240 MHz ESP32-S3 runs at a noticeably lower frame rate than
the 400 MHz P4.


## Controls

| Retro-Go button | Mario Kart 64 action |
| --- | --- |
| D-pad | Steer / navigate menus |
| A | Accelerate / confirm |
| B | Brake, reverse / cancel |
| X or L | Use item (Z) |
| R | Hop / drift |
| Y | Look behind (C-up) |
| Select | Cycle race HUD (C-right) |
| Start | Game pause / start |
| Menu | Retro-Go game menu |
| Options | Retro-Go options menu |

Available buttons and Menu/Options combinations depend on your device's
Retro-Go configuration. Steering currently uses digital D-pad input.

## Emulator options and system menu

- **Sound:** enables or disables sound. Off stops audio generation, mixing
  and output rather than merely muting it, reducing CPU work. The setting is
  remembered between launches. Game audio is currently disabled on the
  ESP32-S3 build, where this option has no effect.
- **Save screenshot:** saves the last completed image through Retro-Go.
- **Shared Retro-Go settings:** display, volume and other platform options
  are available through the usual system menus.
- **Reset:** safely relaunches the application and reloads the ROM.
- **Quit:** returns to the launcher and flushes native progress saves.

The game uses its native EEPROM progress saving. **Mid-race save states are
not supported**; selecting save/load state displays an explanatory message.
Controller Pak ghost saving and PSP ad hoc multiplayer are not implemented.

## Building

From the Retro-Go repository root, with its ESP-IDF environment configured:

```sh
python rg_tool.py --target gb300-p4 release mk64-go launcher
```

For an ESP32-S3 handheld, use your S3 target instead, for example:

```sh
python rg_tool.py --target <s3-target> release mk64-go launcher
```

The S3 launcher partition table must give `mk64-go` a **12 MiB app partition**
(**12,582,912 bytes**). The app image itself must stay below **1,961,984 bytes**
(0x1DF000): the 4 KiB header at 0x1DF000 and the asset store from 0x1E0000 to
the end of the partition occupy the rest. The current S3 image is 1,771,232
bytes, leaving about 186 KiB of headroom.

End users running a supplied firmware build only need to provide their ROM.

## Thanks and credits

Thank you to the developers whose work made this port possible:

- **Nintendo's original Mario Kart 64 development team**, for the game.
- **The [n64decomp/mk64 team and contributors](https://github.com/n64decomp/mk64)**,
  for the decompilation and documentation.
- **David Becker and the MK64 Portable PSP contributors**, for the portable
  game foundation, on-device extraction recipes and audio work adapted here.
  See the included [PSP project README](components/mk64-go/README.md).
- **The Retro-Go developers and contributors**, for the handheld platform
  and unified system APIs.
- **The SM64 port developers**, whose Retro-Go rendering and audio work
  provided valuable references, and the upstream
  [sm64-port](https://github.com/sm64-port/sm64-port),
  [SpaghettiKart](https://github.com/HarbourMasters/SpaghettiKart) and
  [Torch](https://github.com/HarbourMasters/torch) projects acknowledged by
  the PSP port.

This is an unofficial project, not affiliated with or endorsed by Nintendo.
Mario Kart 64 belongs to Nintendo. Upstream code has differing licence terms;
see the included [licence and attribution notices](components/mk64-go/LICENSE).
