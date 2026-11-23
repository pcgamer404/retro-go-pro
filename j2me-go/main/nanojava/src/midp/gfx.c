/*
 * gfx.c - software rasterizer: rectangles, lines, triangles, arcs, image
 * blits with transforms and alpha.
 */
#include "../pal/alloc.h"
#include "gfx.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#endif

#include <stdlib.h>
#include <string.h>
#include <math.h>

static inline void plot(Surface *s, const Clip *c, int x, int y, uint16_t color)
{
    if (x >= c->x1 && x < c->x2 && y >= c->y1 && y < c->y2)
        s->px[y * s->w + x] = color;
}

IRAM_ATTR void gfx_fill_rect(Surface *s, const Clip *c, int x, int y, int w, int h, uint16_t color)
{
    int x1 = x > c->x1 ? x : c->x1;
    int y1 = y > c->y1 ? y : c->y1;
    int x2 = x + w < c->x2 ? x + w : c->x2;
    int y2 = y + h < c->y2 ? y + h : c->y2;
    if (x1 >= x2 || y1 >= y2)
        return;
    int n = x2 - x1;
    uint32_t c32 = ((uint32_t)color << 16) | (uint32_t)color;
    for (int yy = y1; yy < y2; yy++) {
        uint16_t *dst = s->px + yy * s->w + x1;
        int count = n;
        if (((uintptr_t)dst & 3) && count > 0) {
            *dst++ = color;
            count--;
        }
        uint32_t *dst32 = (uint32_t *)(void *)dst;
        while (count >= 8) {
            dst32[0] = c32;
            dst32[1] = c32;
            dst32[2] = c32;
            dst32[3] = c32;
            dst32 += 4;
            count -= 8;
        }
        while (count >= 2) {
            *dst32++ = c32;
            count -= 2;
        }
        if (count > 0) {
            *(uint16_t *)dst32 = color;
        }
    }
}

void gfx_line(Surface *s, const Clip *c, int x1, int y1, int x2, int y2, uint16_t color, bool dotted)
{
    if (y1 == y2 && !dotted) {
        if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
        gfx_fill_rect(s, c, x1, y1, x2 - x1 + 1, 1, color);
        return;
    }
    if (x1 == x2 && !dotted) {
        if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
        gfx_fill_rect(s, c, x1, y1, 1, y2 - y1 + 1, color);
        return;
    }
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    for (int step = 0;; step++) {
        if (!dotted || (step & 2) == 0)
            plot(s, c, x1, y1, color);
        if (x1 == x2 && y1 == y2)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

void gfx_rect(Surface *s, const Clip *c, int x, int y, int w, int h, uint16_t color, bool dotted)
{
    if (w < 0 || h < 0)
        return;
    if (w == 0 || h == 0) {
        gfx_line(s, c, x, y, x + w, y + h, color, dotted);
        return;
    }
    gfx_line(s, c, x, y, x + w, y, color, dotted);
    gfx_line(s, c, x, y + h, x + w, y + h, color, dotted);
    gfx_line(s, c, x, y + 1, x, y + h - 1, color, dotted);
    gfx_line(s, c, x + w, y + 1, x + w, y + h - 1, color, dotted);
}

/* Fast DDA scanline triangle fill with 16.16 fixed-point edges and 32-bit dual-pixel
 * span writing. Slopes are calculated once per edge, replacing per-scanline 64-bit software
 * division with pure single-cycle additions. */
IRAM_ATTR void gfx_fill_triangle(Surface *s, const Clip *c, int x1, int y1, int x2, int y2,
                                int x3, int y3, uint16_t color)
{
    int t;
    if (y1 > y2) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y2 > y3) { t = y2; y2 = y3; y3 = t; t = x2; x2 = x3; x3 = t; }
    if (y1 > y2) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }

    /* Completely outside vertical clip bounds */
    if (y3 < c->y1 || y1 >= c->y2)
        return;

    /* Degenerate horizontal line */
    if (y1 == y3) {
        if (y1 >= c->y1 && y1 < c->y2) {
            int lo = x1 < x2 ? x1 : x2;
            if (x3 < lo) lo = x3;
            int hi = x1 > x2 ? x1 : x2;
            if (x3 > hi) hi = x3;
            gfx_fill_rect(s, c, lo, y1, hi - lo + 1, 1, color);
        }
        return;
    }

    uint32_t c32 = ((uint32_t)color << 16) | (uint32_t)color;

    /* Long edge 1-3 */
    int dy13 = y3 - y1;
    int32_t dx13 = (int32_t)(((int64_t)(x3 - x1) << 16) / dy13);

    /* Top half: y1 to y2 */
    if (y2 > y1) {
        int dy12 = y2 - y1;
        int32_t dx12 = (int32_t)(((int64_t)(x2 - x1) << 16) / dy12);

        int32_t xa = ((int32_t)x1 << 16) + 0x8000;
        int32_t xb = xa;

        int ys = y1;
        if (ys < c->y1) {
            int skip = c->y1 - ys;
            xa += dx13 * skip;
            xb += dx12 * skip;
            ys = c->y1;
        }

        int ye = (y2 == y3) ? y2 : y2 - 1;
        if (ye > c->y2 - 1)
            ye = c->y2 - 1;

        for (int y = ys; y <= ye; y++) {
            int l = (int)(xa >> 16);
            int r = (int)(xb >> 16);
            if (l > r) { int tmp = l; l = r; r = tmp; }
            int xs = l > c->x1 ? l : c->x1;
            int xe = r < c->x2 - 1 ? r : c->x2 - 1;
            if (xs <= xe) {
                int count = xe - xs + 1;
                uint16_t *dst = s->px + y * s->w + xs;
                if (((uintptr_t)dst & 2) && count > 0) {
                    *dst++ = color;
                    count--;
                }
                uint32_t *dst32 = (uint32_t *)dst;
                while (count >= 8) {
                    dst32[0] = c32; dst32[1] = c32; dst32[2] = c32; dst32[3] = c32;
                    dst32 += 4;
                    count -= 8;
                }
                while (count >= 2) {
                    *dst32++ = c32;
                    count -= 2;
                }
                if (count > 0) {
                    *(uint16_t *)dst32 = color;
                }
            }
            xa += dx13;
            xb += dx12;
        }
    }

    /* Bottom half: y2 to y3 (only if y3 > y2) */
    if (y3 > y2) {
        int dy23 = y3 - y2;
        int32_t dx23 = (int32_t)(((int64_t)(x3 - x2) << 16) / dy23);

        int32_t xa = ((int32_t)x1 << 16) + 0x8000 + (int32_t)(((int64_t)dx13 * (y2 - y1)));
        int32_t xb = ((int32_t)x2 << 16) + 0x8000;

        int ys = y2;
        if (ys < c->y1) {
            int skip = c->y1 - ys;
            xa += dx13 * skip;
            xb += dx23 * skip;
            ys = c->y1;
        }

        int ye = y3 < c->y2 - 1 ? y3 : c->y2 - 1;

        for (int y = ys; y <= ye; y++) {
            int l = (int)(xa >> 16);
            int r = (int)(xb >> 16);
            if (l > r) { int tmp = l; l = r; r = tmp; }
            int xs = l > c->x1 ? l : c->x1;
            int xe = r < c->x2 - 1 ? r : c->x2 - 1;
            if (xs <= xe) {
                int count = xe - xs + 1;
                uint16_t *dst = s->px + y * s->w + xs;
                if (((uintptr_t)dst & 2) && count > 0) {
                    *dst++ = color;
                    count--;
                }
                uint32_t *dst32 = (uint32_t *)dst;
                while (count >= 8) {
                    dst32[0] = c32; dst32[1] = c32; dst32[2] = c32; dst32[3] = c32;
                    dst32 += 4;
                    count -= 8;
                }
                while (count >= 2) {
                    *dst32++ = c32;
                    count -= 2;
                }
                if (count > 0) {
                    *(uint16_t *)dst32 = color;
                }
            }
            xa += dx13;
            xb += dx23;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Arcs                                                                     */

/* Angle in degrees [0, 360) of (dx, -dy), measured counter-clockwise from
 * the positive x axis like MIDP. Uses a cheap octant approximation that is
 * monotonic, which is all the arc clipping needs. */
static inline int angle_of(float dx, float dy)
{
    float ax = dx < 0.0f ? -dx : dx, ay = dy < 0.0f ? -dy : dy;
    if (ax == 0.0f && ay == 0.0f)
        return 0;
    /* atan approximation, max error ~0.3 degrees */
    float a;
    if (ax >= ay) {
        float z = ay / ax;
        a = (45.0f * z) - z * (z - 1.0f) * (14.0f + 3.83f * z);
    } else {
        float z = ax / ay;
        a = 90.0f - ((45.0f * z) - z * (z - 1.0f) * (14.0f + 3.83f * z));
    }
    if (dx < 0.0f) a = 180.0f - a;
    if (dy < 0.0f) a = 360.0f - a;
    int r = (int)(a + 0.5f);
    return r >= 360 ? r - 360 : (r < 0 ? 0 : r);
}

static inline bool in_arc_norm(int ang, int norm_start, int norm_arc)
{
    int d = ang - norm_start;
    if (d < 0)
        d += 360;
    return d <= norm_arc;
}

/* Alois Zingl's integer midpoint ellipse rasterizer for full circle/ellipse outline */
static void gfx_ellipse_outline(Surface *s, const Clip *c, int x0, int y0, int x1, int y1, uint16_t color)
{
    int a = abs(x1 - x0), b = abs(y1 - y0), b1 = b & 1;
    long dx = 4 * (1 - a) * b * b, dy = 4 * (b1 + 1) * a * a;
    long err = dx + dy + b1 * a * a, e2;

    if (x0 > x1) { x0 = x1; x1 += a; }
    if (y0 > y1) { y0 = y1; }
    y0 += (b + 1) / 2;
    y1 = y0 - b1;
    a *= 8 * a;
    b1 = 8 * b * b;

    do {
        plot(s, c, x1, y0, color);
        plot(s, c, x0, y0, color);
        plot(s, c, x0, y1, color);
        plot(s, c, x1, y1, color);
        e2 = 2 * err;
        if (e2 <= dy) {
            y0++;
            y1--;
            dy += a;
            err += dy;
        }
        if (e2 >= dx || 2 * err > dy) {
            x0++;
            x1--;
            dx += b1;
            err += dx;
        }
    } while (x0 <= x1);

    while (y0 - y1 < b) {
        plot(s, c, x0 - 1, y0, color);
        plot(s, c, x1 + 1, y0++, color);
        plot(s, c, x0 - 1, y1, color);
        plot(s, c, x1 + 1, y1--, color);
    }
}

void gfx_arc(Surface *s, const Clip *c, int x, int y, int w, int h, int start, int arc,
             uint16_t color, bool fill)
{
    if (w < 0 || h < 0 || arc == 0)
        return;
    if (x + w < c->x1 || x >= c->x2 || y + h < c->y1 || y >= c->y2)
        return;

    if (!fill && (arc >= 360 || arc <= -360)) {
        gfx_ellipse_outline(s, c, x, y, x + w, y + h, color);
        return;
    }

    float rx = (float)w * 0.5f, ry = (float)h * 0.5f;
    float cx = (float)x + rx, cy = (float)y + ry;
    if (rx <= 0.0f || ry <= 0.0f) {
        gfx_line(s, c, x, y, x + w, y + h, color, false);
        return;
    }

    if (!fill) {
        /* Outline: step along the arc with 2D rotation (avoiding per-step sinf/cosf) */
        float span = (float)arc;
        if (span > 360.0f) span = 360.0f;
        else if (span < -360.0f) span = -360.0f;

        float max_r = (rx > ry) ? rx : ry;
        float arc_rad = fabsf(span) * (3.141592653589793f / 180.0f);
        int num_steps = (int)(arc_rad * max_r) + 2;
        if (num_steps < 2) num_steps = 2;
        if (num_steps > 800) num_steps = 800;

        float step_rad = (span * (3.141592653589793f / 180.0f)) / (float)(num_steps - 1);
        float cur_rad = (float)start * (3.141592653589793f / 180.0f);

        float d_cos = cosf(step_rad);
        float d_sin = sinf(step_rad);
        float cur_c = cosf(cur_rad);
        float cur_s = sinf(cur_rad);

        int last_x = -999999, last_y = -999999;
        for (int i = 0; i < num_steps; i++) {
            int px = (int)roundf(cx + rx * cur_c);
            int py = (int)roundf(cy - ry * cur_s);

            if (i > 0 && (abs(px - last_x) > 1 || abs(py - last_y) > 1)) {
                gfx_line(s, c, last_x, last_y, px, py, color, false);
            } else {
                plot(s, c, px, py, color);
            }
            last_x = px;
            last_y = py;

            float next_c = cur_c * d_cos - cur_s * d_sin;
            float next_s = cur_s * d_cos + cur_c * d_sin;
            cur_c = next_c;
            cur_s = next_s;
        }
        return;
    }

    /* Fill: scanline rasterization within the ellipse bounds */
    int y1 = (int)floorf(cy - ry);
    int y2 = (int)ceilf(cy + ry);
    if (y1 < c->y1) y1 = c->y1;
    if (y2 >= c->y2) y2 = c->y2 - 1;

    float inv_ry2 = 1.0f / (ry * ry);

    if (arc >= 360 || arc <= -360) {
        /* Full ellipse fill: fast span fill without angle checks */
        for (int py = y1; py <= y2; py++) {
            float fdy = (float)py + 0.5f - cy;
            float rem = 1.0f - (fdy * fdy) * inv_ry2;
            if (rem < 0.0f) continue;
            float xspan = sqrtf(rem) * rx;
            int sx1 = (int)ceilf(cx - 0.5f - xspan - 1e-4f);
            int sx2 = (int)floorf(cx - 0.5f + xspan + 1e-4f);
            if (sx1 < c->x1) sx1 = c->x1;
            if (sx2 >= c->x2) sx2 = c->x2 - 1;
            if (sx2 >= sx1)
                gfx_fill_rect(s, c, sx1, py, sx2 - sx1 + 1, 1, color);
        }
        return;
    }

    /* Partial arc fill (pie slice) */
    int norm_arc = arc;
    int norm_start = start;
    if (norm_arc < 0) {
        norm_start += norm_arc;
        norm_arc = -norm_arc;
    }
    norm_start %= 360;
    if (norm_start < 0) norm_start += 360;

    float inv_rx = 1.0f / rx;
    float inv_ry = 1.0f / ry;

    for (int py = y1; py <= y2; py++) {
        float fdy = (float)py + 0.5f - cy;
        float rem = 1.0f - (fdy * fdy) * inv_ry2;
        if (rem < 0.0f) continue;
        float xspan = sqrtf(rem) * rx;
        int sx1 = (int)ceilf(cx - 0.5f - xspan - 1e-4f);
        int sx2 = (int)floorf(cx - 0.5f + xspan + 1e-4f);
        if (sx1 < c->x1) sx1 = c->x1;
        if (sx2 >= c->x2) sx2 = c->x2 - 1;
        if (sx1 > sx2) continue;

        float ny = -fdy * inv_ry;
        int span_start = -1;
        for (int px = sx1; px <= sx2; px++) {
            float nx = ((float)px + 0.5f - cx) * inv_rx;
            if (in_arc_norm(angle_of(nx, ny), norm_start, norm_arc)) {
                if (span_start < 0) span_start = px;
            } else {
                if (span_start >= 0) {
                    gfx_fill_rect(s, c, span_start, py, px - span_start, 1, color);
                    span_start = -1;
                }
            }
        }
        if (span_start >= 0) {
            gfx_fill_rect(s, c, span_start, py, sx2 - span_start + 1, 1, color);
        }
    }
}

void gfx_round_rect(Surface *s, const Clip *c, int x, int y, int w, int h, int aw, int ah,
                    uint16_t color, bool fill)
{
    if (w < 0 || h < 0)
        return;
    if (aw < 0) aw = -aw;
    if (ah < 0) ah = -ah;
    if (aw > w) aw = w;
    if (ah > h) ah = h;
    int rw = aw / 2, rh = ah / 2;
    if (fill) {
        gfx_fill_rect(s, c, x + rw, y, w - 2 * rw, h, color);
        gfx_fill_rect(s, c, x, y + rh, rw, h - 2 * rh, color);
        gfx_fill_rect(s, c, x + w - rw, y + rh, rw, h - 2 * rh, color);
        gfx_arc(s, c, x, y, aw, ah, 90, 90, color, true);
        gfx_arc(s, c, x + w - aw, y, aw, ah, 0, 90, color, true);
        gfx_arc(s, c, x, y + h - ah, aw, ah, 180, 90, color, true);
        gfx_arc(s, c, x + w - aw, y + h - ah, aw, ah, 270, 90, color, true);
    } else {
        gfx_line(s, c, x + rw, y, x + w - rw, y, color, false);
        gfx_line(s, c, x + rw, y + h, x + w - rw, y + h, color, false);
        gfx_line(s, c, x, y + rh, x, y + h - rh, color, false);
        gfx_line(s, c, x + w, y + rh, x + w, y + h - rh, color, false);
        gfx_arc(s, c, x, y, aw, ah, 90, 90, color, false);
        gfx_arc(s, c, x + w - aw, y, aw, ah, 0, 90, color, false);
        gfx_arc(s, c, x, y + h - ah, aw, ah, 180, 90, color, false);
        gfx_arc(s, c, x + w - aw, y + h - ah, aw, ah, 270, 90, color, false);
    }
}

/* ------------------------------------------------------------------------ */
/* Blits                                                                    */

IRAM_ATTR void gfx_blit(Surface *dst, const Clip *c, const Surface *src, int sx, int sy, int w, int h,
                        int transform, int dx, int dy)
{
    bool swap = transform >= 4;   /* the ROT90/ROT270 family swaps axes */
    int  dw = swap ? h : w, dh = swap ? w : h;

    /* Where destination (0,0) reads from, and the source step per
     * destination x and y, in source pixels. */
    int u0, v0, ux, vx, uy, vy;
    switch (transform) {
    default:
    case TRANS_NONE:          u0 = 0;     v0 = 0;     ux = 1;  vx = 0;  uy = 0;  vy = 1;  break;
    case TRANS_MIRROR:        u0 = w - 1; v0 = 0;     ux = -1; vx = 0;  uy = 0;  vy = 1;  break;
    case TRANS_MIRROR_ROT180: u0 = 0;     v0 = h - 1; ux = 1;  vx = 0;  uy = 0;  vy = -1; break;
    case TRANS_ROT180:        u0 = w - 1; v0 = h - 1; ux = -1; vx = 0;  uy = 0;  vy = -1; break;
    case TRANS_ROT90:         u0 = 0;     v0 = h - 1; ux = 0;  vx = -1; uy = 1;  vy = 0;  break;
    case TRANS_ROT270:        u0 = w - 1; v0 = 0;     ux = 0;  vx = 1;  uy = -1; vy = 0;  break;
    case TRANS_MIRROR_ROT90:  u0 = w - 1; v0 = h - 1; ux = 0;  vx = -1; uy = -1; vy = 0;  break;
    case TRANS_MIRROR_ROT270: u0 = 0;     v0 = 0;     ux = 0;  vx = 1;  uy = 1;  vy = 0;  break;
    }

    int x1 = dx > c->x1 ? dx : c->x1;
    int y1 = dy > c->y1 ? dy : c->y1;
    int x2 = dx + dw < c->x2 ? dx + dw : c->x2;
    int y2 = dy + dh < c->y2 ? dy + dh : c->y2;
    if (x1 >= x2 || y1 >= y2)
        return;

    /* Fast path: untransformed opaque copy, row by row (memmove handles
     * copyArea overlap). */
    if (transform == TRANS_NONE && !src->alpha) {
        int n = x2 - x1;
        if (src != dst) {
            for (int y = y1; y < y2; y++)
                memcpy(dst->px + y * dst->w + x1,
                       src->px + (sy + y - dy) * src->w + sx + (x1 - dx), (size_t)n * 2);
            return;
        }
        if (dy < sy) {
            for (int y = y1; y < y2; y++)
                memmove(dst->px + y * dst->w + x1,
                        src->px + (sy + y - dy) * src->w + sx + (x1 - dx), (size_t)n * 2);
        } else {
            for (int y = y2 - 1; y >= y1; y--)
                memmove(dst->px + y * dst->w + x1,
                        src->px + (sy + y - dy) * src->w + sx + (x1 - dx), (size_t)n * 2);
        }
        return;
    }

    /* Transformed copies from a surface onto itself go through a copy. */
    uint16_t *tmp = NULL;
    const uint16_t *spx = src->px;
    int sstride = src->w;
    if (src == dst) {
        tmp = nj_pal_malloc((size_t)w * h * 2);
        if (!tmp)
            return;
        for (int y = 0; y < h; y++)
            memcpy(tmp + y * w, src->px + (sy + y) * src->w + sx, (size_t)w * 2);
        spx = tmp;
        sstride = w;
        sx = sy = 0;
    }

    const int dsi = vx * sstride + ux;      /* source index step per destination x */
    const uint8_t *alpha = (src->alpha && !tmp) ? src->alpha : NULL;
    for (int y = y1; y < y2; y++) {
        int ddx = x1 - dx, ddy = y - dy;
        int u = u0 + ux * ddx + uy * ddy;
        int v = v0 + vx * ddx + vy * ddy;
        int si = (sy + v) * sstride + sx + u;
        uint16_t *out = dst->px + y * dst->w + x1;
        uint16_t *end = out + (x2 - x1);
        if (!alpha) {
            if (dsi == 1) {
                memcpy(out, spx + si, (size_t)(x2 - x1) * sizeof(uint16_t));
            } else {
                for (; out < end; out++, si += dsi)
                    *out = spx[si];
            }
        } else {
            if (dsi == 1) {
                const uint16_t *src_p = spx + si;
                const uint8_t  *al_p  = alpha + si;
                while (out + 4 <= end) {
                    uint32_t a4;
                    memcpy(&a4, al_p, 4);
                    if (a4 == 0) {
                        out += 4;
                        src_p += 4;
                        al_p += 4;
                        continue;
                    }
                    if (a4 == 0xffffffff) {
                        memcpy(out, src_p, 4 * sizeof(uint16_t));
                        out += 4;
                        src_p += 4;
                        al_p += 4;
                        continue;
                    }
                    for (int k = 0; k < 4; k++) {
                        uint8_t a = al_p[k];
                        if (a == 255)
                            out[k] = src_p[k];
                        else if (a)
                            out[k] = gfx_blend(out[k], src_p[k], a);
                    }
                    out += 4;
                    src_p += 4;
                    al_p += 4;
                }
                for (; out < end; out++, src_p++, al_p++) {
                    uint8_t a = *al_p;
                    if (a == 255)
                        *out = *src_p;
                    else if (a)
                        *out = gfx_blend(*out, *src_p, a);
                }
            } else {
                for (; out < end; out++, si += dsi) {
                    uint8_t a = alpha[si];
                    if (a == 255)
                        *out = spx[si];
                    else if (a)
                        *out = gfx_blend(*out, spx[si], a);
                }
            }
        }
    }
    nj_pal_free(tmp);
}

void gfx_blit_alpha(Surface *dst, const Clip *c, const Surface *src, int sx, int sy,
                    int w, int h, int transform, int dx, int dy)
{
    if (!dst->alpha || !src->alpha)
        return;
    bool swap = transform >= 4;
    int  dw = swap ? h : w, dh = swap ? w : h;

    int u0, v0, ux, vx, uy, vy;
    switch (transform) {
    default:
    case TRANS_NONE:          u0 = 0;     v0 = 0;     ux = 1;  vx = 0;  uy = 0;  vy = 1;  break;
    case TRANS_MIRROR:        u0 = w - 1; v0 = 0;     ux = -1; vx = 0;  uy = 0;  vy = 1;  break;
    case TRANS_MIRROR_ROT180: u0 = 0;     v0 = h - 1; ux = 1;  vx = 0;  uy = 0;  vy = -1; break;
    case TRANS_ROT180:        u0 = w - 1; v0 = h - 1; ux = -1; vx = 0;  uy = 0;  vy = -1; break;
    case TRANS_ROT90:         u0 = 0;     v0 = h - 1; ux = 0;  vx = -1; uy = 1;  vy = 0;  break;
    case TRANS_ROT270:        u0 = w - 1; v0 = 0;     ux = 0;  vx = 1;  uy = -1; vy = 0;  break;
    case TRANS_MIRROR_ROT90:  u0 = w - 1; v0 = h - 1; ux = 0;  vx = -1; uy = -1; vy = 0;  break;
    case TRANS_MIRROR_ROT270: u0 = 0;     v0 = 0;     ux = 0;  vx = 1;  uy = 1;  vy = 0;  break;
    }

    int x1 = dx > c->x1 ? dx : c->x1;
    int y1 = dy > c->y1 ? dy : c->y1;
    int x2 = dx + dw < c->x2 ? dx + dw : c->x2;
    int y2 = dy + dh < c->y2 ? dy + dh : c->y2;
    if (x1 >= x2 || y1 >= y2)
        return;

    int ddx = x1 - dx, ddy = y1 - dy;
    int si = (sy + v0 + vx * ddx + vy * ddy) * src->w
             + sx + u0 + ux * ddx + uy * ddy;
    const int stepx = vx * src->w + ux;
    const int stepy = vy * src->w + uy;
    for (int y = y1; y < y2; y++) {
        const uint8_t *in = src->alpha + si;
        uint8_t       *out = (uint8_t *)dst->alpha + (size_t)y * dst->w + x1;
        if (stepx == 1 && in + (x2 - x1) <= src->alpha + (size_t)src->w * src->h) {
            memcpy(out, in, (size_t)(x2 - x1));
        } else {
            for (int x = x1; x < x2; x++, in += stepx)
                *out++ = *in;
        }
        si += stepy;
    }
}

IRAM_ATTR void gfx_draw_rgb(Surface *dst, const Clip *c, const int32_t *rgb, int offset, int scan,
                            int x, int y, int w, int h, bool alpha)
{    int x1 = x > c->x1 ? x : c->x1;
    int y1 = y > c->y1 ? y : c->y1;
    int x2 = x + w < c->x2 ? x + w : c->x2;
    int y2 = y + h < c->y2 ? y + h : c->y2;
    if (x1 >= x2 || y1 >= y2)
        return;
    if (!alpha) {
        for (int yy = y1; yy < y2; yy++) {
            const int32_t *row = rgb + offset + (yy - y) * scan + (x1 - x);
            uint16_t      *out = dst->px + yy * dst->w + x1;
            int count = x2 - x1;
            if (((uintptr_t)out & 2) && count > 0) {
                *out++ = gfx_pack((uint32_t)*row++);
                count--;
            }
            uint32_t *out32 = (uint32_t *)out;
            while (count >= 2) {
                uint32_t p0 = gfx_pack((uint32_t)row[0]);
                uint32_t p1 = gfx_pack((uint32_t)row[1]);
                *out32++ = p0 | (p1 << 16);
                row += 2;
                count -= 2;
            }
            out = (uint16_t *)out32;
            if (count > 0) {
                *out = gfx_pack((uint32_t)*row);
            }
        }
        return;
    }
    for (int yy = y1; yy < y2; yy++) {
        const int32_t *row = rgb + offset + (yy - y) * scan + (x1 - x);
        uint16_t      *out = dst->px + yy * dst->w + x1;
        for (int xx = x1; xx < x2; xx++, row++, out++) {
            uint32_t argb = (uint32_t)*row;
            uint32_t a = argb >> 24;
            if (!a)
                continue;
            if (a == 255)
                *out = gfx_pack(argb);
            else
                *out = gfx_blend_argb(*out, argb, a);
        }
    }
}
