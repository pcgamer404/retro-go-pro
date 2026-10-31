#include "oj_indexed_rgb565.h"

bool oj_indexed_to_rgb565(const uint8_t *src, int width, int height, int src_pitch,
                          uint16_t *dst, int dst_stride,
                          const oj_rgb_color_t palette[256])
{
    if (!src || !dst || !palette || width <= 0 || height <= 0 ||
        src_pitch < width || dst_stride < width)
        return false;

    for (int y = 0; y < height; ++y) {
        const uint8_t *s = src + (size_t)y * (size_t)src_pitch;
        uint16_t *d = dst + (size_t)y * (size_t)dst_stride;
        for (int x = 0; x < width; ++x) {
            const oj_rgb_color_t c = palette[s[x]];
            d[x] = (uint16_t)(((uint16_t)(c.r & 0xF8u) << 8) |
                              ((uint16_t)(c.g & 0xFCu) << 3) |
                              ((uint16_t)c.b >> 3));
        }
    }
    return true;
}
