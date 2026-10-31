# mame-go

Arcade and Neo Geo emulation for ESP32-S3 handhelds, running **MAME 0.37b5** (the `mame2000-libretro` core).

Supports save states, hiscore saving, screenshots and automatic frameskip.

## Supported games

Games must be complete **MAME ROM sets in `.zip` format**. Not every MAME game is included, only these driver families:

| Platform | Games |
|---|---|
| **Neo Geo (MVS/AES)** | King of Fighters '94–'99, Metal Slug 1/2/X, Samurai Shodown 1–4, Fatal Fury 1–3 / Special, Garou, Art of Fighting 1–3, Last Blade 1/2, Real Bout Fatal Fury 1/2/Special, World Heroes 1/2/2 Jet/Perfect, Magician Lord, Neo Turf Masters, Puzzle Bobble 1/2, Bust-A-Move, Windjammers, Blazing Star, Pulstar and more (about 150 sets plus clones) |
| **Capcom CPS1** | Street Fighter II (and Champion Edition / Turbo variants), Final Fight, Ghouls'n Ghosts, Strider, Forgotten Worlds, Lost Worlds, Willow, U.N. Squadron, 1941, Mercs, Magic Sword, Carrier Air Wing, Nemo, Three Wonders, King of Dragons, Captain Commando, Knights of the Round, Varth, Cadillacs & Dinosaurs, The Punisher, Warriors of Fate, Slam Masters, Mega Twins, Pang! 3, Mega Man: The Power Battle, Quiz & Dragons |
| **Pac-Man hardware** | Pac-Man and clones (Puck Man, Pac-Man Plus, Hangly Man, Crush Roller…), Ms. Pac-Man, Eyes, Lizard Wizard, Ponpoko, Mr. TNT |
| **Galaxian / Scramble hardware** | Galaxian, Galaga and variants, Moon Cresta, Scramble, Frogger, Amidar, Jump Bug, Mariner, The End, Atlantis |
| **Donkey Kong** | Donkey Kong, Donkey Kong Jr., Donkey Kong 3, Radar Scope, Hunchback |
| **Capcom 1942 / 1943** | 1942, 1943, 1943 Kai |
| **Video System** | Aero Fighters / Sonic Wings, Turbo Force, Spinal Breakers, Karate Blazers |
| **Seibu / Tad** | Blood Bros., West Story |
| **Exidy** | Mouse Trap, Venture, Pepper II, Hard Hat, Fax, Targ, Spectar, Circus, Crash, Robot Bowl, Sidetrack |
| **Bally Astrocade** | Wizard of Wor, Gorf, Robby Roto, Professor Pac-Man, Space Zap, Seawolf II |
| **Other** | Starfire, Fire One, Victory |

The exact list is in `components/mame2000/mamego_driver.c`.

## ROM placement

Put each game's `.zip` (do **not** extract) on the SD card:

```
/sd/roms/arcade/    <- Pac-Man, Galaga, CPS1, etc.
/sd/roms/neogeo/    <- Neo Geo games
```

Example:

```
/sd/roms/arcade/pacman.zip
/sd/roms/arcade/sf2.zip
/sd/roms/neogeo/neogeo.zip     <- Neo Geo BIOS (required)
/sd/roms/neogeo/mslug.zip
/sd/roms/neogeo/kof98.zip
```

### ROM notes

- **Neo Geo needs `neogeo.zip` (the BIOS) in the same folder as the games.** Games with their own BIOS inside the zip also work.
- ROM sets must be **complete**: a missing file shows "This game is not supported, or its ROM set is incomplete". CRC32 is checked, so wrong dumps fail.
- Zip names do not need to match the 0.37b5 name: the driver is chosen from the files inside (e.g. a modern `pacman.zip` loads as the Midway set, `1942a.zip` holding the `1942` set also works). Sets from MAME 0.37b5 through current MAME are expected to work when the files match; split sets need their parent zip.
- Do not rename files inside the zip.
- The first launch of a Neo Geo game is slow while the sprite and sample ROMs are converted. The result is cached in `/sd/retro-go/mame/mame2000/neospr/`. You can pre-generate it on a PC with `tools/neoprep.c` and copy the folder over to skip the wait.
- Large graphics and sound regions go to a `mamerom` flash partition when it exists (4 MB). If it is missing they stay in PSRAM, which is slower to load and leaves less memory for big games.

### Other files

```
/sd/retro-go/mame/mame2000/hiscore.dat        <- high-score definitions (a copy is in components/mame2000/)
/sd/retro-go/mame/mame2000/neospr/            <- cached Neo Geo sprite/sample data
/sd/retro-go/saves/arcade/mame2000/hi/        <- saved high scores (.hi)
```

## Controls

| Handheld button | Arcade |
|---|---|
| D-pad | Joystick |
| A / B / X / Y / L / R | Game buttons |
| START | Player 1 start |
| SELECT | Insert coin |
| MENU | Game menu (save/load state, reset, quit) |

## Credits

- **[pjcau/retro-go](https://github.com/pjcau/retro-go)**: the original `mame-go` app and the whole Retro-Go port of mame2000 (Neo Geo and CPS1 support, tile cache, flash-mapped ROM regions, save states, hiscores, driver picker, performance work), from the [ESP32 Emu Turbo](https://github.com/pjcau/esp32-emu-turbo) project. This folder is that work adapted to another Retro-Go fork; all credit for the port goes to pjcau.
- **[libretro/mame2000-libretro](https://github.com/libretro/mame2000-libretro)**: the MAME 0.37b5 libretro core this is based on.
- **The MAME team** (Nicola Salmoria and contributors): the emulator and every driver in it.
- **[Retro-Go](https://github.com/ducalex/retro-go)** by ducalex and contributors: the frontend it runs on.

## License

MAME's license applies (`components/mame2000/readme.txt`): **free for non-commercial use only**, the full source of the port must be published, and commercial use requires the MAME authors' written permission. Do not sell devices or firmware containing it.

ROMs are not included and are not distributed with this project. Use only dumps of games you own.
