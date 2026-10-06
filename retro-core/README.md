# retro-core

Emulator cores for the ESP32-S3 retro-go handheld. One firmware image (`retro-core`), the core is picked by the
ROM's system. Built on top of `retro-go` (display, input, audio, storage, GUI).

| System | Core | Notes |
|---|---|---|
| NES / Famicom | **FCEUmm** or **Nofrendo** | Choose at game start. FDS always uses FCEUmm |
| Famicom Disk System | FCEUmm | Needs `fds_bios.bin` in the BIOS folder |
| Game Boy / Game Boy Color | gnuboy | RTC config, BIOS option, SRAM autosave, palettes |
| PC Engine / TurboGrafx-16 | pce-go | Overscan option |
| Master System / Game Gear / ColecoVision | smsplus | Game Genie / Action Replay cheats |
| Atari Lynx | handy | Rotation: auto / left / right |
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
FCEUmm, Nofrendo, gnuboy, pce-go, smsplus, handy, LCD-Game-Emulator and their authors; retro-go by ducalex;
fork by DynaMight1124.
