#ifndef NJ_PAL_ALLOC_H
#define NJ_PAL_ALLOC_H
#include <stddef.h>

/* Runtime allocations use the platform allocator so large metadata/arrays can
 * live in PSRAM on ESP32 instead of exhausting internal SRAM. */
void *nj_pal_malloc(size_t size);
void *nj_pal_calloc(size_t count, size_t size);
void *nj_pal_realloc(void *ptr, size_t size);
void nj_pal_free(void *ptr);
/* Zeroed allocation preferring fast internal RAM (falls back to nj_pal_calloc). */
void *nj_pal_calloc_fast(size_t size);

#endif
