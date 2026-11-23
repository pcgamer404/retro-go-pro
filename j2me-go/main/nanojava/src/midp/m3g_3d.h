/*
 * m3g_3d.h - Software 3D Rasterizer & Native Engine for JSR-184 (M3G)
 *            and MascotCapsule Micro3D v3 on ESP32-S3.
 */
#ifndef NJ_M3G_3D_H
#define NJ_M3G_3D_H

#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"

typedef struct {
    float m[16]; /* Column-major 4x4 matrix */
} M3GMatrix;

typedef struct {
    int32_t  x, y, z;   /* 16.16 fixed-point screen coords */
    int32_t  u, v;      /* 16.16 fixed-point texel coords */
    uint32_t color;     /* 0xAARRGGBB */
} M3GVertex;

void midp_m3g_init(void);
void midp_m3g_shutdown(void);

#endif /* NJ_M3G_3D_H */
