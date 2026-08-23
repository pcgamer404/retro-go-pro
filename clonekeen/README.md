# CloneKeen -> retro-go-pro integration guide

## What this package contains

Only the files that had to change to run CloneKeen on top of retro-go
instead of its original standalone SPI/I2S/GPIO drivers:

    main/main.c                    (new)
    main/CMakeLists.txt             (new)
    components/SDL/spi_lcd.c        (rewritten: rg_display backend)
    components/SDL/spi_lcd.h        (trimmed: dropped unused ledc.h)
    components/SDL/SDL_audio.c      (rewritten: rg_audio backend)
    components/SDL/SDL_event.c      (rewritten: rg_input backend)
    components/SDL/CMakeLists.txt   (updated: now depends on retro-go)

Everything else -- all of `components/keen`, all of `components/zlib`,
and the rest of `components/SDL` (SDL_video.c, SDL_system.c,
SDL_input.c, SDL_error.c, headers, etc.) -- is untouched. Copy those
folders verbatim from the original CloneKeen-master zip into your new
app folder.

## Why only these 5 files

CloneKeen's game code and SDL_video.c only ever touch hardware through
three narrow choke points:
- `spi_lcd_*()` (display) -- called from SDL_video.c only
- I2S/DAC calls inside SDL_audio.c's updateTask (audio)
- raw `gpio_get_level()`/ISR inside SDL_event.c (input)

Everything else in the SDL shim and all of `components/keen` calls
*generic* SDL2 API (SDL_BlitSurface, SDL_LockDisplay, SDL_OpenAudio,
SDL_PollEvent, etc.), which doesn't know or care what's underneath.
Rewriting just the hardware-facing internals of these 3 files means
none of keen's ~30 source files needed to change at all.

## Steps to build

1. Create `retro-go-pro/clonekeen/` next to your other apps (rott,
   ngp-go, etc.).
2. Copy in this package's `main/` and `components/SDL/` on top of a
   fresh copy of the original CloneKeen-master's `components/keen` and
   `components/zlib` (unchanged).
3. Check `main/main.c`'s `rg_system_init()` call against your CURRENT
   `components/retro-go/rg_system.h` -- this project has gone through
   at least 3 different API generations in this conversation. Two
   variants are commented in the file; keep whichever matches. If your
   header uses `rg_config_t`, use Option A; if it's the
   `(sampleRate, handlers, NULL)` 3-arg form, use Option B.
4. Add `clonekeen` to your top-level build tool / launcher app list the
   same way rott/ngp-go/oswan were added (partition table entry +
   launcher app-list entry).
5. Verify `FB_PIXEL_FORMAT` in `spi_lcd.c` matches an actual palette
   pixel format name in your `rg_surface.h` (grep it for `PAL` --
   different retro-go snapshots have named this slightly differently,
   e.g. `RG_PIXEL_PAL565_BE` vs `RG_PIXEL_PAL565_LE`; NES's
   `main_nes.c` picks between the two based on `RG_SCREEN_PIXEL_FORMAT`
   -- copy that pattern if needed).
6. Build. Expect a handful of the same class of errors we've hit all
   through this project (`rg_system_exit_called`, `rg_display_is_busy`,
   `rg_task_create` arg count, etc.) if your header has drifted further
   since this was written -- same fixes as before: match the call to
   whatever your current `rg_system.h`/`rg_display.h` actually declare.

## Known gaps / things to verify on hardware

- **Palette timing**: `spi_lcd.c` re-copies all 256 palette entries
  into the surface every frame rather than only on change. Cheap, but
  if you want to optimize later, track a dirty flag in `SDL_video.c`
  where it writes `lcdpal[i]`.
- **Volume cycling**: originally cycled via the VOL/select button
  through `SDL_CAPSLOCK`; wired to `RG_KEY_SELECT` here -- remap in
  `SDL_event.c`'s `extra_keymap[]` if you'd rather use a different
  physical button.
- **Audio format**: the original DAC path used an ESP32-specific
  differential 8-bit DAC encoding. This version assumes keen's audio
  callback fills an 8-bit *unsigned* mono PCM buffer (matches the
  original `buf[i]-127`-style math) and converts that to 16-bit signed
  stereo for `rg_audio_submit()`. If audio sounds wrong (too quiet,
  too loud, or noisy), the most likely culprit is this assumption --
  check what `as.callback` in `components/keen` actually writes into
  its buffer and adjust `convertToStereoPCM()` in `SDL_audio.c`
  accordingly.
- **Screen border**: `spi_lcd_send_boarder()`'s partial-height copy is
  preserved from the original logic but not tested against actual
  border rendering -- verify the top/bottom border area doesn't show
  garbage on first boot.
