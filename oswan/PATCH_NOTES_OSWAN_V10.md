Oswan Retro-Go v10

Baseline: v9 (v8 Test A + profiling counters). Controls confirmed working in v8.
Single functional change: allocate the 64KB WonderSwan work RAM (IRAM) before the
video surfaces so it lands in internal RAM instead of falling back to PSRAM.
  - WS.c: WsAllocateBuffers() returns early if already allocated.
  - main.c: calls WsAllocateBuffers() before creating the surfaces; prints one
    [OSWAN MEM] line showing where IRAM landed and internal free/largest block.
Build/runtime not verified in this environment.

DIAGNOSTIC VARIANT (v10-diag-noaudio): additionally defines OSWAN_DIAG_NO_AUDIO_SUBMIT,
which drains the APU ring but skips rg_audio_submit(). This removes audio pacing so the
game runs unthrottled; use only to read the uncapped emulation speed from [OSWAN PROF].
