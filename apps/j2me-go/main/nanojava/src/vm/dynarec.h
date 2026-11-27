/*
 * dynarec.h - Dynamic Recompiler (JIT v3.1 Unified) for NanoJava on ESP32-S3.
 *
 * Features:
 *  - 2-Register Top-of-Stack (TOS) caching (a11, a12) eliminating operand stack RAM traffic
 *  - Direct C-trampoline calls (callx8) with caller return register preservation (a10)
 *  - Native 3D Engine integration: JSR-184 (M3G) and MascotCapsule Micro3D v3
 *  - Unlimited dynamic cache flushing with ACC_JIT_UNSUPPORTED reset
 *  - Full OSR (On-Stack Replacement) at every loop back-edge
 *  - Hardware FPU (Coprocessor 0) single-precision execution
 */
#ifndef NJ_DYNAREC_H
#define NJ_DYNAREC_H

#include "vm.h"
#include <stdint.h>
#include <stdbool.h>

#define NJ_ENABLE_DYNAREC       1
#define JIT_HOT_THRESHOLD       20            /* Compile loops in 4 iters, methods in 20 calls */
#define JIT_CODE_CACHE_SIZE     (512 * 1024)  /* 512 KB executable JIT code cache in PSRAM */

enum {
    JIT_OK          = 0,  /* Method executed and returned normally */
    JIT_DEOPT       = 1,  /* Bailout to interpreter at current f->pc */
    JIT_YIELD_CALL  = 2,  /* Yield to interpreter to execute an invoke/instruction at f->pc */
    JIT_ERROR       = -1  /* Runtime error or exception */
};

typedef int (*JitFunc)(Thread *t, Frame *f);

#ifdef __cplusplus
extern "C" {
#endif

void dynarec_init(void);
bool dynarec_compile_method(Method *m, Thread *t);
int  dynarec_run_method(Method *m, Thread *t, Frame *f);
void dynarec_flush_cache(void);
void dynarec_check_pending_flush(void);
void dynarec_reset(void);
void dynarec_shutdown(void);
void dynarec_dump_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* NJ_DYNAREC_H */
