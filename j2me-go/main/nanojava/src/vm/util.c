/*
 * util.c - allocation and logging helpers.
 */
#include "../pal/alloc.h"
#include "vm.h"
#include "../pal/pal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void *nj_alloc(size_t n)
{
    void *p = nj_pal_calloc(1, n ? n : 1);
    if (!p)
        nj_fatal("out of native memory (%u bytes)", (unsigned)n);
    return p;
}

/* Metadata (classes, methods, symbols) lives as long as the VM, so it is
 * carved out of large blocks to avoid per-allocation malloc overhead. All
 * blocks are chained so nj_meta_free_all() can release them at shutdown. */
#define META_BLOCK 4096

typedef union MetaBlock {
    union MetaBlock *next;
    double           align;
} MetaBlock;

static MetaBlock *meta_blocks;
static char      *meta_cur;
static size_t     meta_left;

static void *meta_block(size_t n, bool fatal)
{
    MetaBlock *b = nj_pal_calloc(1, sizeof(MetaBlock) + n);
    if (!b) {
        if (fatal)
            nj_fatal("out of native memory (%u bytes)", (unsigned)(sizeof(MetaBlock) + n));
        return NULL;
    }
    b->next = meta_blocks;
    meta_blocks = b;
    return b + 1;
}

void nj_meta_free_all(void)
{
    while (meta_blocks) {
        MetaBlock *next = meta_blocks->next;
        nj_pal_free(meta_blocks);
        meta_blocks = next;
    }
    meta_cur = NULL;
    meta_left = 0;
}

void *nj_meta(size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (n > META_BLOCK / 4)
        return meta_block(n, true);
    if (n > meta_left) {
        size_t try_sizes[] = { META_BLOCK, META_BLOCK / 2, META_BLOCK / 4, n };
        void *p = NULL;
        size_t got = 0;
        for (int i = 0; i < 4; i++) {
            if (try_sizes[i] < n) continue;
            p = meta_block(try_sizes[i], i == 3);
            if (p) {
                got = try_sizes[i];
                break;
            }
        }
        meta_cur = p;
        meta_left = got;
    }
    void *p = meta_cur;
    meta_cur += n;
    meta_left -= n;
    return p;
}

void nj_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof buf - 2)
        n = sizeof buf - 2;
    buf[n++] = '\n';
    pal_console_write(buf, n);
}

void nj_fatal(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    nj_log("FATAL: %s", buf);
    pal_fatal(buf);
}
