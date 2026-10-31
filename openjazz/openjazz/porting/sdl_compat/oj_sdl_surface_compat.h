#ifndef OJ_SDL_SURFACE_COMPAT_H
#define OJ_SDL_SURFACE_COMPAT_H
/*
 * Small SDL2-shaped, 8-bit indexed-surface subset for the OpenJazz port.
 * This is not a complete SDL replacement. It is deliberately standalone so
 * the pixel semantics can be host-tested before wiring it into OpenJazz.
 */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef int SDL_bool;
#define SDL_TRUE 1
#define SDL_FALSE 0
#define SDL_PIXELFORMAT_INDEX8 0x13000801u
#define SDL_SRCCOLORKEY 0x00001000u
#define SDL_LOGPAL 0x01
#define SDL_PHYSPAL 0x02
#define SDL_MUSTLOCK(surface) 0

typedef struct SDL_Color { Uint8 r, g, b, a; } SDL_Color;
typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
typedef struct SDL_Palette { int ncolors; SDL_Color *colors; Uint32 hash; /* 0 = never set */ Uint8 is_ramp; /* r==g==b==index */ } SDL_Palette;
typedef struct SDL_PixelFormat { Uint32 format; SDL_Palette *palette; Uint8 BitsPerPixel, BytesPerPixel; Uint32 Rmask, Gmask, Bmask, Amask, colorkey; } SDL_PixelFormat;
typedef struct SDL_Surface {
    Uint32 flags;
    SDL_PixelFormat *format;
    int w, h, pitch;
    void *pixels;
    SDL_Rect clip_rect;
    Uint32 color_key;
    SDL_bool has_color_key;
    SDL_bool owns_pixels;
    SDL_Color *phys_colors;     /* SDL_PHYSPAL: what the display shows (screen only) */
    Uint8 *blit_map;            /* cached src->dst index map for blits between different palettes */
    Uint32 blit_map_src_hash, blit_map_dst_hash;
} SDL_Surface;

SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width, int height, int depth, Uint32 format);
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(void *pixels, int width, int height, int depth, int pitch, Uint32 format);
void SDL_FreeSurface(SDL_Surface *surface);
int SDL_LockSurface(SDL_Surface *surface);
void SDL_UnlockSurface(SDL_Surface *surface);
int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors, int firstcolor, int ncolors);
int SDL_SetPalette(SDL_Surface *surface, int flags, SDL_Color *colors, int firstcolor, int ncolors);
int SDL_SetColorKey(SDL_Surface *surface, int flag, Uint32 key);
int SDL_GetColorKey(SDL_Surface *surface, Uint32 *key);
SDL_bool SDL_SetClipRect(SDL_Surface *surface, const SDL_Rect *rect);
Uint32 SDL_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b);
int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color);
int SDL_FillRects(SDL_Surface *dst, const SDL_Rect *rects, int count, Uint32 color);
int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
const char *oj_sdl_surface_last_error(void);
#ifdef __cplusplus
}
#endif
#endif
