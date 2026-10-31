# atari-core

One app (`atari-core`) for every Atari system. The core is chosen by the launcher tab (`configNs`).

| System | Tab id | Core | BIOS (in the BIOS folder) |
|---|---|---|---|
| Atari 2600 | `a26` | StellaDS | none |
| Atari 5200 | `a52` | Atari800 core (MCUME) | `5200.rom` (2 KB) **required** |
| Atari 7800 | `a78` | ProSystem | `7800 BIOS (U).rom` / `7800 BIOS (E).rom` optional |
| Atari Lynx | `lnx` | handy | none (rotation: auto / left / right) |

File extensions are defined in the launcher registration, not in the app.

## Controls
- **2600**: D-pad stick, A/B fire, SELECT console select, START console reset. Options menu: P1/P2 difficulty.
- **5200**: D-pad stick, A = fire, B = second button (user 2), START = user 1. Hold SELECT as a shift: with it the face buttons / D-pad / L type the 5200 keypad digits (X=1, Y=2, ... L=RESET).
- **7800**: D-pad, A/B = buttons 1/2 (X also fires 1), START = Reset, SELECT = Select, Y = Pause.
- **Lynx**: D-pad, A/B, START = Pause (Option 2), SELECT = Option 1.
- MENU = game menu (save/load/reset/quit), OPTION = options (scaling, filter, ...).

## Layout
```
atari-core/
  main/main.c          dispatch on configNs -> lynx_main / a26_main / a52_main / a78_main
  main/main_lynx.cpp   (your retro-core Lynx front end, unchanged)
  main/main_a26.cpp    (your stella/main/main.cpp, app_main -> a26_main)
  main/main_a52.c      (pjcau retro-home, ported)
  main/main_a78.c      (pjcau retro-home, ported)
  components/handy  StellaDS  atari5200  prosystem
```

## Notes
- Each core calls `rg_system_reinit(<its own rate>, <its handlers>)` after the single `rg_system_init()` in `main.c`.
  Each also calls `rg_audio_set_sample_rate()` itself; `rg_system.c` is not modified.
- Saves/settings use the tab id as namespace (`a26`, `lnx`, ...), so existing Lynx saves keep working.
  Old standalone Stella saves were stored under the `stella` namespace; set the tab id to `a26` and rename `saves/stella` to `saves/a26` if you want to keep them.
- Not yet verified on hardware: the four cores have never been linked into one image together. If the image is too big for the partition, increase the `atari-core` partition (suggested start: 1.5 MB) or drop one core from `COMPONENTS`.
