/*
 * bmp.c - uncompressed BMP decoder (1/4/8/16/24/32 bpp).
 *
 * Only the rows the caller asks for are converted, into one reusable ARGB
 * row buffer; no full-image copy is made.
 */
#include "../pal/alloc.h"
#include "bmp.h"

#include <string.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct {
    uint32_t off;
    int32_t  w, h;
    uint16_t bpp;
    uint32_t comp;
    uint32_t dib;
    uint32_t pal_off;
    uint32_t pal_n;
    uint32_t masks[4];
    uint32_t stride;
    bool     top_down;
    bool     has_alpha;
} Bmp;

static int mask_shift(uint32_t mask)
{
    int s = 0;
    if (!mask)
        return 0;
    while (!(mask & 1)) {
        mask >>= 1;
        s++;
    }
    return s;
}

static int mask_bits(uint32_t mask)
{
    int n = 0;
    while (mask) {
        n += mask & 1;
        mask >>= 1;
    }
    return n;
}

static unsigned expand(unsigned v, int bits)
{
    if (bits <= 0)
        return 0;
    if (bits >= 8)
        return v & 0xff;
    unsigned max = (1u << bits) - 1;
    return (v * 255u + max / 2) / max;
}

static unsigned extract(uint32_t v, uint32_t mask)
{
    if (!mask)
        return 0;
    return expand((v & mask) >> mask_shift(mask), mask_bits(mask));
}

/* Returns true when the alpha bytes carry real information rather than
 * being the zero padding that many 32-bit BMP writers leave behind. */
static bool alpha_used(const uint8_t *p, uint32_t count, uint32_t step)
{
    bool zero = false, full = false;
    for (uint32_t i = 0; i < count; i++, p += step) {
        uint8_t a = *p;
        if (a == 0)
            zero = true;
        else if (a == 255)
            full = true;
        else
            return true;
    }
    return zero && full;
}

static bool parse(const uint8_t *d, uint32_t len, Bmp *b)
{
    if (len < 14 + 40 || d[0] != 'B' || d[1] != 'M')
        return false;
    memset(b, 0, sizeof *b);
    b->off = le32(d + 10);
    b->dib = le32(d + 14);
    if (b->dib < 40 || b->dib > len - 14)
        return false;
    b->w = (int32_t)le32(d + 18);
    b->h = (int32_t)le32(d + 22);
    b->bpp = le16(d + 28);
    b->comp = le32(d + 30);
    if (b->w <= 0 || b->w > 4096 || b->h == 0 || b->h < -4096 || b->h > 4096)
        return false;
    if (le16(d + 26) != 1)
        return false;
    if (b->bpp != 1 && b->bpp != 4 && b->bpp != 8 && b->bpp != 16 &&
        b->bpp != 24 && b->bpp != 32)
        return false;
    if (b->comp != 0 && !(b->comp == 3 && (b->bpp == 16 || b->bpp == 32)))
        return false;
    b->top_down = b->h < 0;
    uint32_t abs_h = (uint32_t)(b->top_down ? -b->h : b->h);

    int64_t stride = ((int64_t)b->bpp * b->w + 31) / 32 * 4;
    if (stride > 0x7fffffff)
        return false;
    b->stride = (uint32_t)stride;
    if ((int64_t)b->off + (int64_t)b->stride * abs_h > len)
        return false;

    if (b->bpp <= 8) {
        uint32_t n = le32(d + 14 + 32);
        if (n == 0 || n > (1u << b->bpp))
            n = 1u << b->bpp;
        b->pal_n = n;
        b->pal_off = 14 + b->dib;
        if (b->pal_off + n * 4 > len || b->pal_off + n * 4 > b->off)
            return false;
        b->has_alpha = alpha_used(d + b->pal_off + 3, n, 4);
    }

    if (b->comp == 3) {
        uint32_t mo = 14 + (b->dib >= 52 ? 40 : b->dib);
        if (mo + 12 > len)
            return false;
        b->masks[0] = le32(d + mo);
        b->masks[1] = le32(d + mo + 4);
        b->masks[2] = le32(d + mo + 8);
        if (b->dib >= 56)
            b->masks[3] = le32(d + mo + 12);
        if ((int64_t)mo + 12 > b->off)
            return false;
    }

    if (b->bpp == 32) {
        bool zero = false, full = false;
        for (uint32_t y = 0; y < abs_h && !b->has_alpha; y++) {
            const uint8_t *p = d + b->off + (size_t)y * b->stride + 3;
            for (int x = 0; x < b->w; x++, p += 4) {
                uint8_t a = *p;
                if (a == 0)
                    zero = true;
                else if (a == 255)
                    full = true;
                else {
                    b->has_alpha = true;
                    break;
                }
            }
            if (zero && full)
                b->has_alpha = true;
        }
    } else if (b->bpp == 16) {
        b->has_alpha = false;
    }
    return true;
}

bool bmp_is_bmp(const uint8_t *data, uint32_t len)
{
    return len >= 2 && data[0] == 'B' && data[1] == 'M';
}

bool bmp_info(const uint8_t *data, uint32_t len, BmpInfo *info)
{
    Bmp b;
    if (!parse(data, len, &b))
        return false;
    info->width = b.w;
    info->height = b.top_down ? -b.h : b.h;
    info->has_alpha = b.has_alpha;
    return true;
}

bool bmp_decode(const uint8_t *data, uint32_t len, BmpRowFn fn, void *ctx)
{
    Bmp b;
    if (!parse(data, len, &b))
        return false;
    uint32_t abs_h = (uint32_t)(b.top_down ? -b.h : b.h);
    uint32_t *row = nj_pal_malloc((size_t)b.w * sizeof(uint32_t));
    if (!row)
        return false;

    for (uint32_t y = 0; y < abs_h; y++) {
        uint32_t src_row = b.top_down ? y : abs_h - 1 - y;
        const uint8_t *p = data + b.off + (size_t)src_row * b.stride;
        for (int x = 0; x < b.w; x++) {
            uint32_t argb;
            if (b.bpp == 32) {
                if (b.comp == 3 && b.masks[0]) {
                    uint32_t v = le32(p + x * 4);
                    unsigned a = b.has_alpha && b.masks[3]
                                     ? extract(v, b.masks[3]) : 255;
                    argb = (a << 24) | (extract(v, b.masks[0]) << 16) |
                           (extract(v, b.masks[1]) << 8) |
                           extract(v, b.masks[2]);
                } else {
                    const uint8_t *q = p + x * 4;
                    unsigned a = b.has_alpha ? q[3] : 255;
                    argb = (a << 24) | ((uint32_t)q[2] << 16) |
                           ((uint32_t)q[1] << 8) | q[0];
                }
            } else if (b.bpp == 24) {
                const uint8_t *q = p + x * 3;
                argb = 0xff000000u | ((uint32_t)q[2] << 16) |
                       ((uint32_t)q[1] << 8) | q[0];
            } else if (b.bpp == 16) {
                uint16_t v = le16(p + x * 2);
                unsigned r, g, bl;
                if (b.comp == 3 && b.masks[0]) {
                    r = extract(v, b.masks[0]);
                    g = extract(v, b.masks[1]);
                    bl = extract(v, b.masks[2]);
                } else {
                    r = expand((v >> 10) & 31, 5);
                    g = expand((v >> 5) & 31, 5);
                    bl = expand(v & 31, 5);
                }
                argb = 0xff000000u | (r << 16) | (g << 8) | bl;
            } else {
                unsigned idx;
                if (b.bpp == 8) {
                    idx = p[x];
                } else if (b.bpp == 4) {
                    idx = (p[x >> 1] >> ((x & 1) ? 0 : 4)) & 15;
                } else {
                    idx = (p[x >> 3] >> (7 - (x & 7))) & 1;
                }
                if (idx >= b.pal_n)
                    idx = 0;
                const uint8_t *q = data + b.pal_off + idx * 4;
                unsigned a = b.has_alpha ? q[3] : 255;
                argb = (a << 24) | ((uint32_t)q[2] << 16) |
                       ((uint32_t)q[1] << 8) | q[0];
            }
            row[x] = argb;
        }
        fn(ctx, (int)y, 0, 1, row, b.w);
    }
    nj_pal_free(row);
    return true;
}
