/* xjit_exec.h - executable memory for translated code on the ESP32-S3 */
#ifndef XJIT_EXEC_H
#define XJIT_EXEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint8_t *data;      /* where to write (data alias in RAM/PSRAM) */
    uint32_t exec;      /* where the code runs (instruction alias in MMU) */
    size_t   size;
    bool     psram;
} xj_exec_t;

#ifdef __cplusplus
extern "C" {
#endif

bool xj_exec_alloc_iram(xj_exec_t *x, size_t size);
bool xj_exec_alloc_psram(xj_exec_t *x, size_t size);   /* rounded up to 64 KB MMU pages */
void xj_exec_free(xj_exec_t *x);
int  xj_exec_sync(xj_exec_t *x, size_t off, size_t len);

static inline void *xj_exec_ptr(const xj_exec_t *x, size_t off)
{
    return (void *)(uintptr_t)(x->exec + off);
}

#ifdef __cplusplus
}
#endif

#endif /* XJIT_EXEC_H */
