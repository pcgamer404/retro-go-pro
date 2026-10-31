# Quake II-Go

Quake II app port for Retro-Go on ESP32-S3, based on the supplied
Quake2-ESP32S3 source archive.

## Port status

The Quake II engine source and its upstream GPL license are staged in this app.
The first Retro-Go adapter pass connects launch, indexed display, basic
gamepad input, audio submission, save paths, and the Retro-Go game menu. The
app builds for ESP32-S3. The first hardware runs exposed SD file-descriptor
exhaustion and startup PSRAM pressure. PAK reads now share one serialized SD
handle, and the display adapter no longer keeps duplicate frame buffers. The
updated build needs a hardware retest.

The upstream port stores decoded models and skins in a dedicated flash
partition to keep large levels within the ESP32-S3's 8 MiB PSRAM. This app links
the same image into its own flash rodata instead (no extra partition, no
launcher changes): `tools/mkassets.py` packs every MD2 model into
`main/assets.bin`, `main/assets.S.in` pins it into the app image, and
`Q_Assets_Init()` hands it to the engine, which then uses the models in place.

Rebuild it whenever the PAK files change:

```sh
python tools/mkassets.py sd/baseq2 main/assets.bin
```

That adds 8.4 MB to the app (9.4 MB image). Without `main/assets.bin` the build
stops with an explicit error, because the game does not fit in PSRAM without it
(`Hunk_Begin: out of memory` while loading a map). Skins are still read from the
PAKs; `--skins` caches them too but needs a much larger image.

## Performance

The video menu (`Options` -> `Video`) only exposes settings that do something on
this port: 3D view size (fewer pixels = faster), brightness, texture detail
(software mip level) and the water warp effect. The PC driver/mode selectors are
gone, and `vid_ref` is forced to the only renderer that is linked in (`soft`).

Two build settings matter more than any cvar, and they live in
`sdkconfig.defaults` (merged on top of the target's sdkconfig at configure
time): the instruction cache (32 KB, was 16) and the data cache (64 KB /
64-byte lines, was 32 KB / 32-byte) plus `-O2` instead of `-Os`. The renderer
runs from flash, so cache size is the main cost per frame.

Sound is fed to the I2S driver by its own task (`audio_feeder` in `main.c`).
Doing it from the frame loop made `i2s_write()` block for up to 33 ms per frame
(the hardware DMA ring) because one frame of game audio at 15 fps is 66 ms long;
the engine now mixes into a 0.5 s ring in PSRAM and never waits for the DAC.

The engine has a built-in profiler for the next round of tuning: `perf start
200`, play, `perf stop`, `perf dump` in the console prints per-frame time for
server/game/client/render/parallel/present/file/sound. The console (`~` /
`toggleconsole`) is not bound to a gamepad button in this port yet.

Keep the retail game data on the SD card; it is not included in this source
port. The upstream port expects the patched retail PAK files under
`/sd/roms/quake2/baseq2/`:

- `pak0.pak`
- `pak1.pak`
- `pak2.pak`

## Upstream

The engine is from the supplied `Quake2-ESP32S3-main.zip` archive and is
licensed under GPL-2.0. See [LICENSE](LICENSE) and
[UPSTREAM_README.md](UPSTREAM_README.md) for the original license and upstream
port notes.
