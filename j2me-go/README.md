# j2me-go

**Java ME (J2ME) games on Retro-Go.**

`j2me-go` is a Retro-Go application that runs Java ME / MIDP 2.0 game JARs
directly on ESP32-S3 handhelds. Pick a `.jar` from the ROM list, it starts the
first MIDlet, and the console buttons become Java key events. No conversion, no
patching: the game's own JAR is loaded as-is.

It is built on **[NanoJava](https://github.com/mostazaniikkkk/Nano-Java)**, a
small, portable CLDC/MIDP runtime written in C, with an ESP32/Retro-Go platform
layer, a display pipeline and a handful of VM fixes on top (see
[PERFORMANCE_NOTES.md](PERFORMANCE_NOTES.md)).

Most 2D titles run well, including 2D racing and sports games. 3D titles that
need the M3G API (`javax.microedition.m3g`) cannot run - that API is not part of
this build, and the log says so instead of leaving the game hanging.

Developed and tested on an ESP32-S3 handheld with a 320x240 display.

## Features

Every setting is **per game** and saved the first time you change it.

| Feature | What it does |
| --- | --- |
| **Canvas profile** | The virtual screen size the game draws into: `Auto (JAR name)`, `128x128`, `128x160`, `176x208`, `176x220`, `208x208`, `320x240`. Auto reads a size hint from the JAR filename, e.g. `Asphalt 3 [176x208].jar`. |
| **Rotation** | `Auto`, `Off`, `90 CW`, `180`, `270 CW`. Auto turns portrait canvases into the landscape panel. |
| **Screen placement** | `Centered 1:1` (crisp, no scaling) or `Stretch to screen` (fills the panel). |
| **Frame pacing** | `Accurate` honours `Thread.sleep()` exactly; `Balanced` only shortens frame-limiter sleeps; `Fast` (default) shortens most sleeps; `Turbo` turns every sleep of 2 ms or more into 1 ms. Faster pacing can rescue games that throttle themselves too hard. |
| **Keypad diagonals** | Off by default. When enabled, holding a diagonal for 0.5/0.75/1 s also presses `1`/`3`/`7`/`9`, for ports that only accept a number key to continue. |
| **Performance logging** | A once-per-second telemetry line: FPS, present/convert cost, VM vs native time, GC, idle, sleep and yield counts, the most expensive native and Java methods, and measured input latency. |

Runtime support includes `GameCanvas`, the 2D game API (`LayerManager`,
`TiledLayer`, `Sprite`), `RecordStore` saves, JAR resources and image decoding.
Audio is currently stubbed.

## Controls

| Handheld | Java ME key |
| --- | --- |
| D-pad Up / Down / Left / Right | `KEY_UP` / `KEY_DOWN` / `KEY_LEFT` / `KEY_RIGHT` |
| A | Fire / centre key (`KEY_NUM5`) |
| Select | Left soft key (OK / Select) |
| Start | Right soft key (Back) |
| Y | `0` |
| B | `*` |
| X | `#` |

The menu button opens the normal Retro-Go game menu; its **Emulator Options**
row opens the settings above (including performance logging).

## Building

You need an ESP-IDF 5.3.x checkout and a Retro-Go tree. The app folder sits next
to the other apps (for example `retro-go/testing/j2me-go`), is listed in the
build driver's `PROJECT_APPS` with its partition/memory sizing, and needs a
launcher entry such as:

```c
application("Java / J2ME", "j2me", "jar", "j2me-go", 0);
```

Then:

```sh
# from the Retro-Go root that contains rg_tool.py
python rg_tool.py build j2me-go --target <your-target>
python rg_tool.py flash j2me-go --target <your-target>
```

The class library is committed prebuilt (`main/nanojava_classlib.jar`, built
from `main/nanojava/classlib/src`), so a normal build needs no JDK. If you edit
the Java sources, rebuild the JAR with `javac --release 8` and keep the class
files at version 52; the JAR is embedded in the app image.

## Storage layout

```
/sd/roms/jar/                              game JARs (subfolders are scanned)
/sd/retro-go/nanojava/*.rms                RecordStore saves (hex-encoded names)
/sd/retro-go/config/j2me/games/<game>.cfg  per-game settings
```

## Repository layout

```
main/nanojava_classlib.jar    embedded Java class library (class file v52)
main/nanojava/classlib/src/   its sources
main/nanojava/src/            VM, class library natives, MIDP, utils
main/nanojava/platform/retrogo/  Retro-Go platform layer (input, display, app_main)
components/retro-go/rg_gui.c  app-local copy of the system GUI file that adds
                              the "Emulator Options" row to the game menu
PERFORMANCE_NOTES.md          why the port looks the way it does, and what was
                              measured/fixed for speed and correctness
```

## Limitations

- 3D games using M3G (`javax.microedition.m3g`) do not run; the app reports it.
- Audio is stubbed, so sound-related threads may throw harmlessly.
- No save states (MIDP games save through `RecordStore`, which does work).
- MIDP/CLDC coverage is good but not complete; the odd title may still miss an
  API. A log with the `Uncaught exception` line identifies it.

## Credits

- **[NanoJava](https://github.com/mostazaniikkkk/Nano-Java)** by
  mostazaniikkkk - the CLDC/MIDP runtime this port is built on (CC0 1.0).
- **[Retro-Go](https://github.com/ducalex/retro-go)** by ducalex - the firmware
  framework, launcher, display/input/storage APIs and build system.
- DejaVu fonts, see `main/nanojava/FONT_LICENSE`.

NanoJava's licence text is in `main/nanojava/LICENSE`. Platform-layer changes in
this repository are documented in `PERFORMANCE_NOTES.md`.
