Oswan Retro-Go v7

Baseline: newly uploaded original oswan(1).zip.

Changes:
- Restored the original input architecture exactly: the main loop calls ws_input_poll(0), and WsRun() also obtains input through WsInputGetState().
- Preserved the original WonderSwan button mapping and Select/X/Y behavior.
- Changed only MENU/OPTION handling to rising-edge one-shot behavior, avoiding the first MENU press/release problem while retaining the original polling architecture.
- Preserved the Rockman EXE WS sprite-priority compatibility change from v3.

This build intentionally does not carry forward the later experimental input rewrites.
Build/runtime: ESP32-S3 hardware verification not performed here.
