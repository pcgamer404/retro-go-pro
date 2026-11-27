/*
 * bmp.h - uncompressed BMP decoder (1/4/8/16/24/32 bpp).
 *
 * Some J2ME titles ship BMP artwork and hand it to Image.createImage or
 * DirectUtils.createImage. Those calls used to fail because only PNG was
 * understood; this decoder covers the common Windows BMP variants.
 */
#ifndef NJ_BMP_H
#define NJ_BMP_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct BmpInfo {
    int  width, height;
    bool has_alpha;
} BmpInfo;

/* Same contract as PngRowFn: n ARGB pixels for row y at x0, x0 + step, ... */
typedef void (*BmpRowFn)(void *ctx, int y, int x0, int step,
                         const uint32_t *argb, int n);

bool bmp_is_bmp(const uint8_t *data, uint32_t len);
bool bmp_info(const uint8_t *data, uint32_t len, BmpInfo *info);
bool bmp_decode(const uint8_t *data, uint32_t len, BmpRowFn fn, void *ctx);

#endif
