#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "rg_surface.h"
#include "oj_indexed_rgb565.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert OpenJazz's 8-bit indexed framebuffer to Retro-Go RGB565 and submit it.
 * Palette entries are RGB triples (SDL-style); alpha is ignored.
 * Intended for the final software-rendered framebuffer, not every intermediate
 * SDL_Surface. The OpenJazz video loop calls this for final-frame presentation.
 */
typedef oj_rgb_color_t oj_rg_palette_color_t;

rg_surface_t *oj_rg_get_frame(void);

bool oj_rg_present_indexed(const uint8_t *pixels, int width, int height, int pitch,
                           const oj_rg_palette_color_t palette[256]);

#ifdef __cplusplus
} /* extern "C" */
#endif
