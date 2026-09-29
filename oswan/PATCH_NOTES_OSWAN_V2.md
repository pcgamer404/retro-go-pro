# Oswan Retro-Go v2

## Fixes
- Restored per-scanline clearing of the persistent Z/W renderer buffers. v1 moved these buffers out of the stack but only cleared them at allocation; stale Z-buffer values could suppress sprites.
- Moved Retro-Go MENU/OPTION handling into the main loop before WonderSwan input, matching the standard Retro-Go core pattern. This prevents the first MENU press from being consumed as a held emulator/system input.
- Preserved Select X/Y-pad toggle behavior.

## Performance/behavior
- Restored automatic Retro-Go-style frameskip when a frame falls behind instead of the v1 custom counter, while respecting a configured fixed frameskip if one is explicitly set.
- No changes to WonderSwan CPU timing, slot scheduling, DMA, sprite priority rules, or graphics algorithms.

## Verification
- Source-level consistency checks performed.
- ESP32-S3/ESP-IDF hardware runtime not available in this environment.
