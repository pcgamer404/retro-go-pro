Oswan Retro-Go v9 (profiling only)

Baseline: v8 Test A (original + sprite-priority change; controls confirmed working).
Change: diagnostic timing counters only. No behaviour, input, memory, or render changes.
  - main/main.c: timers around WsRun, ws_graphics_paint (display submit), audio drain;
    prints one line to the serial console every 150 frames.
  - components/oswan/WS.c: timer around the RefreshLine() call.

Serial output example:
[OSWAN PROF] frames=150 fps=24.0 | per-frame ms: wall=41.7 run=.. (cpu+other=.. refresh=.. submit=..) audio_wait=.. frameskip=..

Build/runtime not verified in this environment.
