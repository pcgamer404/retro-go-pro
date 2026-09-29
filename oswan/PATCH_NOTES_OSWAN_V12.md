Oswan Retro-Go v12

Baseline: v11 (audio disabled, profiling counters). Test v11 first.
Single change: NEC V30MZ instruction handlers moved from flash to IRAM.
  - cpu/nec.c: OP() macro now expands to `static void IRAM_ATTR name(void)`;
    nec_interrupt() and i_invalid() also IRAM_ATTR.
  - cpu/necinstr.h: 246 forward declarations marked IRAM_ATTR.
  - cpu/necea.h: 24 effective-address helpers marked IRAM_ATTR.
Rationale: only nec_execute() itself was in IRAM; every opcode handler ran from flash
through a 16KB instruction cache. cpu+other stayed ~14 ms/frame regardless of RAM placement.
Cost: internal RAM (code size of the handlers comes out of free internal heap).
Build/runtime not verified in this environment.
