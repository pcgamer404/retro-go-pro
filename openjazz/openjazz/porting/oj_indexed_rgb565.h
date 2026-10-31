#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t r, g, b, a;
} oj_rgb_color_t;

/* Convert an 8-bit indexed frame to little-endian RGB565 words.
 * src_pitch is measured in bytes; dst_stride is measured in uint16_t pixels.
 * Destination padding, if any, is left untouched. */
bool oj_indexed_to_rgb565(const uint8_t *src, int width, int height, int src_pitch,
                          uint16_t *dst, int dst_stride,
                          const oj_rgb_color_t palette[256]);
