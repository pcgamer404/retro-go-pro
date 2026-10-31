# Core manager

Installs emulator/port `.bin` cores into a shared flash pool and boots them,
without giving each core a fixed partition in the partition table.

Status: **prototype**. Install/list/delete/boot work. Installed cores appear as
tabs after restart. New core bundles describe their ROM behavior and artwork;
older images without metadata get a direct-launch tab and matching built-in art
when available. Core names are matched without punctuation and an optional
`-go` suffix, so names such as `mk-64` and `mk64-go` resolve together.

## Layout

```
0x000000  bootloader + partition table
0x010000  launcher app            0x140000 (1.25 MB)
0x150000  "cores" APP partition   0xEA0000 (~14.6 MB)
            0x0000  manifest copy 0 (4 KB)
            0x1000  manifest copy 1 (4 KB)
            0x10000 first core, 64 KB aligned, packed upwards
```

The pool is registered as type **app** (custom subtype `0x30`) although it is
never booted through the table. Cores such as `mk64-go` call
`esp_ota_get_running_partition()` to find the partition that holds them, and
that function only searches app partitions. The stock bootloader counts only
OTA subtypes, so `app_count` stays 1 and `coreboot` keeps working.

`rg_tool.py` sizes the pool to fill the rest of flash (minus one 64 KB block,
because `mkfw.py` appends a 256 byte footer to the image). Nothing in the pool
is a partition, so cores are added and removed at runtime.

## Cores with a flash asset store

`mk64-go` (S3) writes its own data into the partition that contains it:
`[0x1DF000, 0xC00000)` is erased and filled with ~10 MB of extracted assets on
first run, and its image must stay below `0x1DF000`.

The range is not looked up by name at runtime: when such a core is installed,
`core_pool.c` copies the range into the manifest entry (`reserve_offset` /
`reserve_size`). From then on:

* the entry's footprint is image + asset area, and that is what the Core manager
  shows (e.g. `11.71 MB (1.70 + 10.01 assets)`),
* the range is skipped by the allocator and counted as used space,
* installing a core that would land in the range is refused, and
* replacing it rewrites the same offset (there is no room for a second copy).

Only the install-time table `core_stores[]` knows which cores need an area; add
one line there for a new core. Manifest v1 pools are upgraded on first boot, so
cores installed before this change keep their reservation.

So `mk64-go` must be installed on an empty pool: format, install it, then
install the other cores, which are placed above `0xC00000` (~2.6 MB free).

Why 64 KB alignment: the ESP image format stores only virtual addresses, and
the loader maps flash pages using the physical offset. A 64 KB aligned base
keeps `paddr` and `vaddr` congruent for every image, which is what makes an
arbitrary placement work.

## Manifest

`launcher/main/core_manifest.h` is shared by the app and the bootloader. It
holds up to 16 entries:

| field | meaning |
|---|---|
| `name` | from the `.bin` filename, lowercased, used to replace a core |
| `title` | from the app descriptor's `project_name` |
| `version` | app descriptor version, e.g. `1.4x-MEGAPACK-32-g2f1f0-dirty` |
| `date`, `time` | app descriptor build stamp |
| `offset`, `size` | image location inside the pool |
| `crc32` | CRC of the image |
| `reserve_offset`, `reserve_size` | asset area the core owns (0 if none) |

Manifest version 2 adds the reserve fields; a version 1 pool is converted on the
first boot, keeping the installed cores.

Two copies are kept in the first two sectors. Every save goes to the copy that
is not in use, so a power loss can never destroy the list of installed cores.
`seq` decides which copy is newer.

## Boot path

`launcher/bootloader_components/coreboot` hooks `bootloader_after_init()`.
After the stock bootloader has read the partition table it:

1. reads the manifest from `ota[0].offset + ota[0].size`;
2. if `selected >= 0`, **clears `selected` and saves** (single-use selection);
3. verifies the image with `esp_image_verify()`;
4. points a copy of `bs.ota[0]` at the core and calls
   `bootloader_utility_load_boot_image()`, reusing the stock loader.

Every failure path just returns, so the stock loader boots the launcher.

Because the selection is consumed before the core starts, a broken core cannot
cause a boot loop: it either starts, or the next reset lands in the launcher.
No boot counter is needed.

### Recovery

Hold **SELECT** (GPIO 5) at power-on for 2 seconds. The bootloader erases both
manifest copies and boots the launcher. Installed images are left alone but are
no longer listed.

## Usage

1. Copy core images to `/sd/cores/` on the SD card.
2. Retro-Go settings → **Core manager**.
3. **Install from SD** → pick a `.bin`. The image is written, verified, and
   listed with its build version.
4. Select a core → **Boot now**. The device restarts and boots that core.
5. **Delete** removes it from the list; its space is reused by the next install.
6. Installed cores appear as launcher tabs once the launcher restarts. ROM cores
   list files matching their declared extensions; direct-launch ports show a
   **Start game** entry.

After installing, deleting, or formatting cores, the launcher asks whether to
restart now. Choose **No** to stay in the launcher; restart later to refresh the
available tabs.

## Building a core with launcher metadata

Core projects can describe their launcher behavior in `core.json` at the
project root. The build tool generates `main/core_assets_manifest.generated.json`
and `main/rg_core_assets_generated.c`; the generated C resource is linked into
the core `.bin` so the launcher can read it after installation.

```json
{
  "id": "example-core",
  "title": "Example Core",
  "launch_mode": "rom",
  "rom_extensions": ["rom"],
  "rom_folder": "example-core"
}
```

`id` must be 1–15 lowercase letters, digits, `_` or `-`. ROM extensions are
listed without dots. `rom_folder` is relative to `/sd/roms/`. Put optional
`background.png`, `logo.png`, and `banner.png` artwork in the core project's
`art/` folder. The build tool bundles these PNG files and the generated
manifest into the core image.

For a port that starts itself rather than opening a selected ROM, add an empty
`start_game` marker file at the project root. The generated manifest then uses
direct launch mode, and the launcher shows a **Start game** entry. Its core may
still use data stored under `/sd/roms/<rom_folder>/`.

Build one core from the project root with:

```bash
python rg_tool.py build-core classicube
```

This creates `classicube/build/classicube.bin`. Install it from `/sd/cores/` as
usual. The launcher extracts its metadata and art to
`/sd/cores/installed/<id>.rga`. Older core images without a resource bundle
still appear as direct-launch tabs and use matching built-in artwork from
`launcher/main/images.c` when available.

Exiting a core reboots into the launcher as usual, because the selection was
already consumed.

## Ports

Ports are ordinary cores: copy the `.bin` to `/sd/cores/`, install it from
**Core manager** and it gets its own tab. There is no separate ports tab and no
`portslot` partition.

## App identity

`rg_handlers_t` has two extra hooks so a pool core looks exactly like a
partition app to the rest of the system:

- `haveApp(partition)` — `applications.c` asks this before creating a tab.
- `switchApp(...)` — called from `update_boot_config()`; returning true means
  "the boot source is not a partition, I handled it". The launcher implements
  both from the manifest, so `esp_ota_set_boot_partition()` is never touched
  for cores.

## Current limits

- **Packaging.** Core image generation is supported for project folders with
  an ESP-IDF `main/` component. Full multi-core release packaging is still
  separate from the launcher image build.
- The pool is not exposed over the web file server.
- Cores must be installed before the tab list is built, so the tab list is
  refreshed after restarting.
