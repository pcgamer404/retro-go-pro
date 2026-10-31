# retro-core

Emulator cores for the ESP32-S3 retro-go handheld. One firmware image (`retro-core`), the core is picked by the
ROM's system. Built on top of `retro-go` (display, input, audio, storage, GUI).

| System | Core | Notes |
|---|---|---|
| NES / Famicom | **FCEUmm** or **Nofrendo** | Choose at game start. FDS always uses FCEUmm |
| Famicom Disk System | FCEUmm | Needs `fds_bios.bin` in the BIOS folder |
| Game Boy / Game Boy Color | gnuboy | RTC config, BIOS option, SRAM autosave, palettes |
| PC Engine / TurboGrafx-16 | pce-go | Overscan option |
| Master System / Game Gear / ColecoVision | smsplus | Game Genie / Action Replay cheats (SMS/GG/Coleco) |
| Sega SG-1000 / SC-3000 | smsplus | START = console PAUSE, SELECT = reset |
| Super Nintendo / Super Famicom | snes9x | 4 key presets, APU on/off, transparency, core-1 audio mixing |
| Game & Watch | gw-emulator (LCD-Game-Emulator) | |

## NES: choosing the core
- When you start a **new game** a small "NES Core" menu appears: pick **FCEUmm** or **Nofrendo**. B / cancel keeps the last choice.
- The choice is remembered **per ROM**. **Resume** skips the menu and uses the core that last ran that ROM.
- Options menu shows the running core (read-only).
- Save states are **core-specific**. Load a state only with the core that saved it.

| | FCEUmm | Nofrendo |
|---|---|---|
| Game Genie cheats | Yes | No |
| Accuracy / mapper coverage | Higher | Good |
| Speed | Good | Fastest |
| FDS | Yes | No |

## SG-1000 (new / fixed)
- Launcher tab id: `sg1` (also accepted: `sg`, `sg1000`). Works with `.sg` files and zips.
- The console is chosen from the launcher tab, so **zipped** SG-1000 ROMs now work (before, only a `.sg` file extension worked).
- START = the console's PAUSE button (most games use it as "start"); SELECT = reset. Before, START opened the ColecoVision keypad prompt.
- 8K / 16K ROMs are mirrored through the 32K cartridge window like on real hardware.

## SNES
- Launcher tab id: `snes` (or `sfc`). Needs PSRAM; heavy games may need frame skip.
- Options: Controls (Native = all 12 buttons 1:1, or the Odroid-style Type A/B/C presets), APU emulation, Transparency.
- Build with `SNES_PROF=1` to get timing counters in the log (the on-screen HUD of the original is not in this retro-go).

## NES: frameskip cap and Game Genie
- **Max frameskip** (Options menu of every retro-core / atari-core emulator, default 2, saved per system). retro-go's auto-frameskip can raise the skip to 5 (1 frame drawn out of 6) when the emulator
  dips below 96% speed. The cap is applied inside the NES cores, so `rg_system.c` is untouched. `0` = never skip except when a frame is already late. The speed
  option (fast-forward) is not capped.
- Game Genie codes (FCEUmm) used to stop working after a **hard reset** (the core reinstalled its memory handlers and dropped the hooks while the codes still showed "on").
  They are re-hooked after hard reset and after loading a state. Cheat files are also parsed safely now (the file buffer was not NUL-terminated).
- A few hot memory handlers (RAM, cart read/write, PPU registers, controller, cheat read) moved to IRAM.

## Controls (all cores)
- **MENU** (tap): game menu (save / load / reset / quit). **OPTION**: options menu.
- FCEUmm: **MENU + A / B** toggles turbo fire. **MENU + Up / Down / Select**: FDS insert / eject / side.
- Nofrendo: **X / Y** = turbo A / B.

## Cheats
- NES (FCEUmm): Options > Game Genie. Codes are saved on the SD card in `cheat/<rom>.cht`.
- SMS / GG / Coleco: Options > Cheat Codes (GG/AR). Same SD card format.

## Frame skip
Frame skip is automatic and only kicks in when the game cannot hold full speed. It is capped at 2 frames
(5 only while fast-forwarding) and returns to 0 after a few stable seconds.

## Audio
Works with the USB DAC dongle (USB-C OTG port) and other retro-go sinks. The dongle's clock paces emulation.

## Credits
FCEUmm, Nofrendo, gnuboy, pce-go, smsplus, snes9x (via pjcau/retro-go-esp32-emu-turbo), LCD-Game-Emulator and their authors; retro-go by ducalex;
fork by DynaMight1124.

Lynx moved to **atari-core**. See `../MIGRATION.md` for the full layout.
