# retro-home

Home-computer emulators in one app. The core is chosen by the launcher tab (`configNs`).

| System | Tab id | Core |
|---|---|---|
| MSX / MSX2 | `msx` | fMSX |
| Commodore 64 | `c64` | Frodo Lite (formats: prg crt t64 d64 zip) |

- **MSX**: needs the fMSX BIOS files in the BIOS folder (the app tells you which one is missing and where to get it). Keyboard emulation and crop options in the Options menu.
- **C64**: see `components/frodo/README` / the old c64-go README: formats table, controls (hold START = virtual keyboard), joystick port option (default 2).
  Hardware key chords: START+SELECT = RUN/STOP, SELECT+A = Return, START+A = Y, START+B = N.

## Adding a new computer (ZX Spectrum, Amstrad, ...)
1. Put the core in `components/<name>/` with its own `CMakeLists.txt` (`COMPONENT_REQUIRES "retro-go"`).
2. Add `main/main_<name>.c(pp)` with `void <name>_main(void)`: call `rg_system_reinit(<rate>, &handlers, NULL)` first, then your loop.
3. Declare it in `main/shared.h`, add a `strcmp(app->configNs, "<id>")` branch in `main/main.c`, add `<name>` to `COMPONENTS` in `CMakeLists.txt`
   and to `COMPONENT_REQUIRES` in `main/CMakeLists.txt`.
4. Register the system in the launcher with part = `retro-home` (see MIGRATION.md).

## Notes
- The pjcau C64 (Teensy64/MCUME) core was **not** used: your Frodo port is more complete (save states, D64 write cache, virtual keyboard).
- Saves: MSX keeps the `msx` namespace; C64 saves move from `c64-go` to `c64` if your launcher tab id is `c64`.
- Needs a bigger partition than fMSX alone: start with 1.5 MB and trim.
