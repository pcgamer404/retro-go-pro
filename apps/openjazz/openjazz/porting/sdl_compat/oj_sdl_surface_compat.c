#include "oj_sdl_surface_compat.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static char last_error[96];
static void seterr(const char *s) { size_t n = strlen(s); if (n >= sizeof(last_error)) n = sizeof(last_error)-1; memcpy(last_error,s,n); last_error[n]=0; }
const char *oj_sdl_surface_last_error(void) { return last_error; }
static int valid_surface(const SDL_Surface *s) { return s && s->pixels && s->format && s->format->format == SDL_PIXELFORMAT_INDEX8; }
static SDL_Surface *make_surface(void *pixels, int w, int h, int pitch, int owns) {
    if (w <= 0 || h <= 0 || pitch < w || (size_t)pitch > SIZE_MAX/(size_t)h) { seterr("invalid surface dimensions"); return NULL; }
    SDL_Surface *s=(SDL_Surface*)calloc(1,sizeof(*s)); SDL_PixelFormat *f=(SDL_PixelFormat*)calloc(1,sizeof(*f)); SDL_Palette *p=(SDL_Palette*)calloc(1,sizeof(*p)); SDL_Color *c=(SDL_Color*)calloc(256,sizeof(*c));
    if (!s || !f || !p || !c) { free(s); free(f); free(p); free(c); seterr("out of memory creating surface"); return NULL; }
    if (!pixels && !(pixels=calloc((size_t)pitch,(size_t)h))) { free(s); free(f); free(p); free(c); seterr("out of memory creating pixels"); return NULL; }
    s->format=f; f->format=SDL_PIXELFORMAT_INDEX8; f->BitsPerPixel=8; f->BytesPerPixel=1; f->palette=p; p->ncolors=256; p->colors=c;
    s->w=w; s->h=h; s->pitch=pitch; s->pixels=pixels; s->owns_pixels=owns || pixels != NULL; s->clip_rect=(SDL_Rect){0,0,w,h};
    /* For externally supplied pixel memory, the caller retains ownership. */
    if (!owns) s->owns_pixels=0;
    return s;
}
SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags,int w,int h,int depth,Uint32 fmt) {
    (void)flags; if (depth != 0 && depth != 8) { seterr("only indexed 8-bit surfaces supported"); return NULL; }
    if (fmt != SDL_PIXELFORMAT_INDEX8) { seterr("only INDEX8 format supported"); return NULL; }
    return make_surface(NULL,w,h,w,1);
}
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags,int w,int h,int depth,Uint32 r,Uint32 g,Uint32 b,Uint32 a) {
    (void)r;(void)g;(void)b;(void)a; if (depth != 8) { seterr("only 8-bit surfaces supported"); return NULL; }
    return SDL_CreateRGBSurfaceWithFormat(flags,w,h,8,SDL_PIXELFORMAT_INDEX8);
}
SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(void *pixels,int w,int h,int depth,int pitch,Uint32 fmt) {
    if (!pixels || depth != 8 || fmt != SDL_PIXELFORMAT_INDEX8) { seterr("invalid external INDEX8 surface"); return NULL; }
    return make_surface(pixels,w,h,pitch,0);
}
void SDL_FreeSurface(SDL_Surface *s) { if(!s)return; if(s->owns_pixels)free(s->pixels); free(s->phys_colors); free(s->blit_map); if(s->format){if(s->format->palette){free(s->format->palette->colors);free(s->format->palette);}free(s->format);} free(s); }
int SDL_LockSurface(SDL_Surface *s) { if(!valid_surface(s)){seterr("invalid surface lock");return -1;} return 0; }
void SDL_UnlockSurface(SDL_Surface *s) { (void)s; }
static void palette_rehash(SDL_Palette *p) {
    Uint32 h = 2166136261u; Uint8 ramp = 1;
    for (int i = 0; i < p->ncolors; i++) {
        const SDL_Color *c = &p->colors[i];
        h = (h ^ c->r) * 16777619u; h = (h ^ c->g) * 16777619u; h = (h ^ c->b) * 16777619u;
        if (c->r != i || c->g != i || c->b != i) ramp = 0;
    }
    p->hash = h | 1u; p->is_ramp = ramp;
}
int SDL_SetPaletteColors(SDL_Palette *p,const SDL_Color *colors,int first,int count) {
    if(!p||!colors||first<0||count<0||first>p->ncolors||count>p->ncolors-first){seterr("palette range out of bounds");return -1;}
    memcpy(p->colors+first,colors,(size_t)count*sizeof(SDL_Color)); palette_rehash(p); return 0;
}
/* SDL 1.2 semantics: SDL_LOGPAL = palette used when blitting (colour matching),
 * SDL_PHYSPAL = palette the display actually shows. OpenJazz keeps a grey-ramp
 * logical palette on every surface (and recolours text by rewriting it) and
 * fades/animates only the screen's physical palette. */
int SDL_SetPalette(SDL_Surface *s,int flags,SDL_Color *colors,int first,int count) {
    if(!valid_surface(s)||!colors||first<0||count<0||first+count>256){seterr("bad palette args");return -1;}
    if (flags == 0 || (flags & SDL_LOGPAL)) { if (SDL_SetPaletteColors(s->format->palette,colors,first,count)) return -1; }
    if (flags & SDL_PHYSPAL) {
        if (!s->phys_colors) { s->phys_colors=(SDL_Color*)calloc(256,sizeof(SDL_Color)); if(!s->phys_colors){seterr("out of memory");return -1;} memcpy(s->phys_colors,s->format->palette->colors,256*sizeof(SDL_Color)); }
        memcpy(s->phys_colors+first,colors,(size_t)count*sizeof(SDL_Color));
    }
    return 0;
}
int SDL_SetColorKey(SDL_Surface *s,int flag,Uint32 key) { if(!valid_surface(s)){seterr("invalid color key surface");return -1;} s->has_color_key=(flag!=0);s->color_key=key&255u;s->format->colorkey=s->color_key;return 0; }
int SDL_GetColorKey(SDL_Surface *s,Uint32 *key) { if(!valid_surface(s)||!key||!s->has_color_key)return -1;*key=s->color_key;return 0; }
SDL_bool SDL_SetClipRect(SDL_Surface *s,const SDL_Rect *r) {
    if (!valid_surface(s)) return SDL_FALSE;
    SDL_Rect n = r ? *r : (SDL_Rect){0,0,s->w,s->h};
    int x0=n.x<0?0:n.x, y0=n.y<0?0:n.y, x1=n.x+n.w, y1=n.y+n.h; if(x1>s->w)x1=s->w;if(y1>s->h)y1=s->h;if(x1<x0)x1=x0;if(y1<y0)y1=y0;
    s->clip_rect=(SDL_Rect){x0,y0,x1-x0,y1-y0}; return s->clip_rect.w>0&&s->clip_rect.h>0?SDL_TRUE:SDL_FALSE;
}
Uint32 SDL_MapRGB(const SDL_PixelFormat *f,Uint8 r,Uint8 g,Uint8 b) { (void)r;(void)g;(void)b; return f&&f->format==SDL_PIXELFORMAT_INDEX8?0:0; }
int SDL_FillRect(SDL_Surface *d,const SDL_Rect *r,Uint32 color) {
    if(!valid_surface(d)){seterr("invalid fill destination");return -1;} SDL_Rect a=r?*r:(SDL_Rect){0,0,d->w,d->h};
    int x0=a.x>d->clip_rect.x?a.x:d->clip_rect.x, y0=a.y>d->clip_rect.y?a.y:d->clip_rect.y;
    int x1=a.x+a.w, y1=a.y+a.h, cx=d->clip_rect.x+d->clip_rect.w, cy=d->clip_rect.y+d->clip_rect.h;
    if (x1 > cx) x1 = cx;
    if (y1 > cy) y1 = cy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > d->w) x1 = d->w;
    if (y1 > d->h) y1 = d->h;
    if (x1 <= x0 || y1 <= y0) return 0;
    for (int y = y0; y < y1; y++)
        memset((Uint8*)d->pixels + (size_t)y*d->pitch + x0, (Uint8)color, (size_t)(x1-x0));
    return 0;
}
int SDL_FillRects(SDL_Surface *d,const SDL_Rect *rs,int n,Uint32 color) { if(n<0||(!rs&&n)){seterr("invalid fill rect list");return -1;}for(int i=0;i<n;i++)if(SDL_FillRect(d,&rs[i],color))return -1;return 0; }
/* Colour map for blitting between surfaces whose logical palettes differ
 * (SDL 1.2 does the same: nearest colour in the destination palette). NULL = identical. */
static const Uint8 *get_blit_map(SDL_Surface *s, SDL_Surface *d) {
    const SDL_Palette *sp = s->format->palette, *dp = d->format->palette;
    if (!sp->hash || !dp->hash || sp->hash == dp->hash) return NULL;
    if (s->blit_map && s->blit_map_src_hash == sp->hash && s->blit_map_dst_hash == dp->hash) return s->blit_map;
    if (!s->blit_map && !(s->blit_map = (Uint8*)malloc(256))) return NULL;
    for (int i = 0; i < 256; i++) {
        const SDL_Color *c = &sp->colors[i];
        if (dp->is_ramp) { s->blit_map[i] = (Uint8)(((int)c->r + c->g + c->b + 1) / 3); continue; }
        int best = 0, bd = 1 << 30;
        for (int j = 0; j < dp->ncolors; j++) {
            int dr = (int)c->r - dp->colors[j].r, dg = (int)c->g - dp->colors[j].g, db = (int)c->b - dp->colors[j].b;
            int dist = dr*dr + dg*dg + db*db;
            if (dist < bd) { bd = dist; best = j; if (!dist) break; }
        }
        s->blit_map[i] = (Uint8)best;
    }
    s->blit_map_src_hash = sp->hash; s->blit_map_dst_hash = dp->hash;
    return s->blit_map;
}
int SDL_BlitSurface(SDL_Surface *s,const SDL_Rect *sr,SDL_Surface *d,SDL_Rect *dr) {
    if(!valid_surface(s)||!valid_surface(d)){seterr("blit requires INDEX8 surfaces");return -1;}
    SDL_Rect a=sr?*sr:(SDL_Rect){0,0,s->w,s->h}; int dx=dr?dr->x:0,dy=dr?dr->y:0;
    if(a.x<0){dx-=a.x;a.w+=a.x;a.x=0;}if(a.y<0){dy-=a.y;a.h+=a.y;a.y=0;}
    if (a.x + a.w > s->w) a.w = s->w - a.x;
    if (a.y + a.h > s->h) a.h = s->h - a.y;
    if(dx<d->clip_rect.x){int n=d->clip_rect.x-dx;a.x+=n;a.w-=n;dx+=n;}if(dy<d->clip_rect.y){int n=d->clip_rect.y-dy;a.y+=n;a.h-=n;dy+=n;}
    int right=d->clip_rect.x+d->clip_rect.w,bottom=d->clip_rect.y+d->clip_rect.h;if(dx+a.w>right)a.w=right-dx;if(dy+a.h>bottom)a.h=bottom-dy;
    if (dx + a.w > d->w) a.w = d->w - dx;
    if (dy + a.h > d->h) a.h = d->h - dy;
    if (a.w <= 0 || a.h <= 0) return 0;
    const Uint8 *map = (s != d) ? get_blit_map(s, d) : NULL;
    if (map) {
        for (int y = 0; y < a.h; y++) {
            const Uint8 *sp = (Uint8*)s->pixels + (size_t)(a.y + y) * s->pitch + a.x;
            Uint8 *dp = (Uint8*)d->pixels + (size_t)(dy + y) * d->pitch + dx;
            if (s->has_color_key) { for (int x = 0; x < a.w; x++) if (sp[x] != s->color_key) dp[x] = map[sp[x]]; }
            else for (int x = 0; x < a.w; x++) dp[x] = map[sp[x]];
        }
        if (dr) { dr->w = a.w; dr->h = a.h; }
        return 0;
    }
    /* Use row-wise direction when same-surface overlap occurs. */
    if(s==d && dy>a.y){for(int y=a.h-1;y>=0;y--){Uint8 *sp=(Uint8*)s->pixels+(size_t)(a.y+y)*s->pitch+a.x;Uint8 *dp=(Uint8*)d->pixels+(size_t)(dy+y)*d->pitch+dx;if(!s->has_color_key)memmove(dp,sp,(size_t)a.w);else for(int x=a.w-1;x>=0;x--)if(sp[x]!=s->color_key)dp[x]=sp[x];}}
    else {for(int y=0;y<a.h;y++){Uint8 *sp=(Uint8*)s->pixels+(size_t)(a.y+y)*s->pitch+a.x;Uint8 *dp=(Uint8*)d->pixels+(size_t)(dy+y)*d->pitch+dx;if(!s->has_color_key)memmove(dp,sp,(size_t)a.w);else for(int x=0;x<a.w;x++)if(sp[x]!=s->color_key)dp[x]=sp[x];}}
    if(dr){dr->w=a.w;dr->h=a.h;}return 0;
}
