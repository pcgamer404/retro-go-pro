#include "SDL_video.h"
#include "rg_system.h"
#include "rg_display.h"
#include "rg_surface.h"
#include "rg_utils.h"
#include <string.h>
#include <stdlib.h>

// OpenTyrian always renders an 8bpp, 320x200 indexed framebuffer and flips
// it as a whole (see vga256d.c / SDL_UpdateRect -> SDL_Flip). There is no
// dirty-rect tracking in the original engine, so we simply double buffer
// two full 320x200 indexed surfaces and hand them to rg_display_submit().

#define TYRIAN_W 320
#define TYRIAN_H 200
#define NUM_BUFFERS 2

#if RG_SCREEN_PIXEL_FORMAT == 0
#define TYRIAN_PIXEL_FORMAT RG_PIXEL_PAL565_BE
#else
#define TYRIAN_PIXEL_FORMAT RG_PIXEL_PAL565_LE
#endif

typedef struct {
    rg_surface_t surface;
    uint8_t *pixels;
} fb_t;

static fb_t fb_pool[NUM_BUFFERS];
static int current_fb = 0;
static uint16_t palette565[256];

SDL_Surface *primary_surface;

int SDL_LockSurface(SDL_Surface *surface)
{
    return 0;
}

void SDL_UnlockSurface(SDL_Surface *surface)
{
}

void SDL_UpdateRect(SDL_Surface *screen, Sint32 x, Sint32 y, Sint32 w, Sint32 h)
{
    SDL_Flip(screen);
}

SDL_VideoInfo *SDL_GetVideoInfo(void)
{
    static SDL_VideoInfo info;
    static SDL_PixelFormat vfmt;
    info.vfmt = &vfmt;
    info.vfmt->BitsPerPixel = 8;
    return &info;
}

char *SDL_VideoDriverName(char *namebuf, int maxlen)
{
    return "retro-go";
}

SDL_Rect **SDL_ListModes(SDL_PixelFormat *format, Uint32 flags)
{
    static SDL_Rect mode_data = {0, 0, TYRIAN_W, TYRIAN_H};
    static SDL_Rect *modes[] = {&mode_data, NULL};
    return modes;
}

void SDL_WM_SetCaption(const char *title, const char *icon)
{
}

char *SDL_GetKeyName(SDLKey key)
{
    return (char *)"";
}

SDL_Keymod SDL_GetModState(void)
{
    return (SDL_Keymod)0;
}

IRAM_ATTR Uint32 SDL_GetTicks(void)
{
    return (Uint32)(rg_system_timer() / 1000);
}

Uint32 SDL_WasInit(Uint32 flags)
{
    return 0;
}

int SDL_InitSubSystem(Uint32 flags)
{
    if (flags & SDL_INIT_VIDEO)
    {
        for (int i = 0; i < NUM_BUFFERS; i++)
        {
            fb_pool[i].pixels = rg_alloc(TYRIAN_W * TYRIAN_H, MEM_SLOW);
            if (!fb_pool[i].pixels)
            {
                RG_LOGE("Failed to allocate Tyrian framebuffer %d", i);
                return -1;
            }
            fb_pool[i].surface.width = TYRIAN_W;
            fb_pool[i].surface.height = TYRIAN_H;
            fb_pool[i].surface.stride = TYRIAN_W;
            fb_pool[i].surface.format = TYRIAN_PIXEL_FORMAT;
            fb_pool[i].surface.palette = palette565;
            fb_pool[i].surface.data = fb_pool[i].pixels;
        }
        SDL_CreateRGBSurface(0, TYRIAN_W, TYRIAN_H, 8, 0, 0, 0, 0);
    }
    return 0;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    SDL_Surface *surface = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    SDL_Rect rect = {.x = 0, .y = 0, .w = width, .h = height};
    SDL_Color col = {.r = 0, .g = 0, .b = 0, .unused = 0};
    SDL_Palette pal = {.ncolors = 1, .colors = &col};
    SDL_PixelFormat *pf = (SDL_PixelFormat *)calloc(1, sizeof(SDL_PixelFormat));
    pf->palette = &pal;
    pf->BitsPerPixel = 8;
    pf->BytesPerPixel = 1;
    pf->Rloss = 0; pf->Gloss = 0; pf->Bloss = 0; pf->Aloss = 0;
    pf->Rshift = 0; pf->Gshift = 0; pf->Bshift = 0; pf->Ashift = 0;
    pf->Rmask = 0; pf->Gmask = 0; pf->Bmask = 0; pf->Amask = 0;
    pf->colorkey = 0;
    pf->alpha = 0;

    surface->flags = flags;
    surface->format = pf;
    surface->w = width;
    surface->h = height;
    surface->pitch = width;
    surface->clip_rect = rect;
    surface->refcount = 1;

    // For the primary 320x200 surface, render directly into the buffer
    // that will be submitted to retro-go, avoiding a per-frame copy.
    if (width == TYRIAN_W && height == TYRIAN_H && fb_pool[0].pixels)
    {
        current_fb = 0;
        surface->pixels = fb_pool[current_fb].pixels;
    }
    else
    {
        surface->pixels = heap_caps_malloc(width * height, MALLOC_CAP_SPIRAM);
        memset(surface->pixels, 0, width * height);
    }

    if (primary_surface == NULL)
        primary_surface = surface;
    return surface;
}

int SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color)
{
    if (dst != NULL)
    {
        if (dstrect != NULL)
        {
            for (int y = dstrect->y; y < dstrect->y + dstrect->h; y++)
                memset((unsigned char *)dst->pixels + y * dst->w + dstrect->x, (unsigned char)color, dstrect->w);
        }
        else
        {
            memset(dst->pixels, (unsigned char)color, dst->pitch * dst->h);
        }
    }
    return 0;
}

SDL_Surface *SDL_GetVideoSurface(void)
{
    return primary_surface;
}

Uint32 SDL_MapRGB(SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b)
{
    if (fmt->BitsPerPixel == 16)
    {
        uint16_t bb = (b >> 3) & 0x1f;
        uint16_t gg = ((g >> 2) & 0x3f) << 5;
        uint16_t rr = ((r >> 3) & 0x1f) << 11;
        return (Uint32)(rr | gg | bb);
    }
    return (Uint32)0;
}

int SDL_SetColors(SDL_Surface *surface, SDL_Color *colors, int firstcolor, int ncolors)
{
    for (int i = firstcolor; i < firstcolor + ncolors && i < 256; i++)
    {
        uint16_t v = ((colors[i].r >> 3) << 11) | ((colors[i].g >> 2) << 5) | (colors[i].b >> 3);
#if RG_SCREEN_PIXEL_FORMAT == 0 /* 565_BE */
        v = (v >> 8) | (v << 8);
#endif
        palette565[i] = v;
    }
    return 1;
}

SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags)
{
    return SDL_GetVideoSurface();
}

void SDL_FreeSurface(SDL_Surface *surface)
{
    if (!surface)
        return;
    // Never free a pooled framebuffer's pixel storage.
    bool pooled = false;
    for (int i = 0; i < NUM_BUFFERS; i++)
        if (surface->pixels == fb_pool[i].pixels)
            pooled = true;
    if (!pooled)
        free(surface->pixels);
    free(surface->format);
    surface->refcount = 0;
}

void SDL_QuitSubSystem(Uint32 flags)
{
}

int SDL_Flip(SDL_Surface *screen)
{
    if (!screen || !screen->pixels)
        return -1;

    fb_t *fb = &fb_pool[current_fb];

    // The engine renders straight into fb->pixels already (see
    // SDL_CreateRGBSurface above), so no per-frame copy is required for the
    // common case. Just submit it.
    rg_display_submit(&fb->surface, 0);

    // Point the engine at the other buffer for the next frame, and seed it
    // with the frame we just submitted so partial redraws (menus, HUD)
    // remain correct.
    int next = (current_fb + 1) % NUM_BUFFERS;
    memcpy(fb_pool[next].pixels, fb->pixels, TYRIAN_W * TYRIAN_H);
    current_fb = next;
    screen->pixels = fb_pool[current_fb].pixels;

    if (primary_surface == screen)
        primary_surface->pixels = screen->pixels;

    return 0;
}

int SDL_VideoModeOK(int width, int height, int bpp, Uint32 flags)
{
    return bpp == 8;
}

void SDL_LockDisplay()
{
}

void SDL_UnlockDisplay()
{
}
