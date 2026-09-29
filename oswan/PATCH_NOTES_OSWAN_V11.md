Oswan Retro-Go v11

Baseline: v9 (v8 Test A + profiling counters). The v10 early-IRAM-alloc change is NOT
included: measured no gain (cpu+other 14.28 ms vs 14.1-14.6 ms, refresh unchanged).
Single logical change: audio disabled (OSWAN_NO_AUDIO=1 in components/oswan/oswan_config.h).
  - main.c: rg_audio_submit() skipped (Dummy sink wait was ~16 ms dead time per frame).
  - WSApu.c: WsWaveSet() mixer skipped (pure audio output; voice DMA/sweep still run).
Set OSWAN_NO_AUDIO to 0 to restore original audio behaviour.
No frame limiter yet: the game currently runs below real time, so none is needed.
Build/runtime not verified in this environment.
