# Oswan Retro-Go v5

## Input correction
- Restored the original Oswan input timing architecture: `WsInputGetState(mode)` calls `ws_input_poll(mode)` directly.
- This is important because `WsRun()` requests input at a specific WonderSwan timing point; polling only once from the outer frame loop can leave the emulator without the expected state.
- Kept MENU/OPTION edge handling inside the input callback to avoid the old first MENU press/hold behavior.
- Preserved Select X/Y pad toggle and A/B/Start/D-pad mapping.

## Graphics
- Preserves v3 sprite-priority diagnostic fix.

## Performance
- Preserves v1/v2 performance changes.

ESP32-S3 build/runtime still needs verification on the user's hardware.
