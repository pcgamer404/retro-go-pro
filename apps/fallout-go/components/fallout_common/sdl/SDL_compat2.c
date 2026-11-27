#include "SDL.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <rg_system.h>
#include <esp_timer.h>

// Subsystems
void SDL_QuitSubSystem(Uint32 flags) {}

// Window / Renderer / Texture dummy instances
struct SDL_Window { int dummy; };
struct SDL_Renderer { int dummy; };
struct SDL_Texture { int dummy; };
struct SDL_Cursor { int dummy; };

static struct SDL_Window s_dummy_window;
static struct SDL_Renderer s_dummy_renderer;
static struct SDL_Texture s_dummy_texture;
static struct SDL_Cursor s_dummy_cursor;

SDL_Window* SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    return &s_dummy_window;
}

void SDL_DestroyWindow(SDL_Window *window) {}
void SDL_SetWindowTitle(SDL_Window *window, const char *title) {}

SDL_Renderer* SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags)
{
    return &s_dummy_renderer;
}

void SDL_DestroyRenderer(SDL_Renderer *renderer) {}
int SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h) { return 0; }
int SDL_RenderClear(SDL_Renderer *renderer) { return 0; }
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect) { return 0; }
void SDL_RenderPresent(SDL_Renderer *renderer) {}

SDL_Texture* SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access, int w, int h)
{
    return &s_dummy_texture;
}

void SDL_DestroyTexture(SDL_Texture *texture) {}

int SDL_QueryTexture(SDL_Texture *texture, Uint32 *format, int *access, int *w, int *h)
{
    if (format) *format = SDL_PIXELFORMAT_RGB888;
    return 0;
}

int SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch)
{
    return 0;
}

SDL_Surface* SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width, int height, int depth, Uint32 format)
{
    return SDL_CreateRGBSurface(flags, width, height, depth, 0, 0, 0, 0);
}

int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors, int firstcolor, int ncolors)
{
    if (!palette || !colors) return -1;
    for (int i = 0; i < ncolors; i++) {
        int idx = firstcolor + i;
        if (idx >= palette->ncolors) break;
        palette->colors[idx] = colors[i];
    }
    // Update shadow palette for Retro-Go display
    SDL_SetColors(NULL, (SDL_Color*)colors, firstcolor, ncolors);
    return 0;
}

int SDL_SetSurfacePalette(SDL_Surface *surface, SDL_Palette *palette)
{
    if (!surface || !surface->format) return -1;
    if (!palette) return 0;
    if (!surface->format->palette) {
        surface->format->palette = SDL_AllocPalette(palette->ncolors);
        if (!surface->format->palette) return -1;
    }
    int count = palette->ncolors;
    if (count > surface->format->palette->ncolors) {
        count = surface->format->palette->ncolors;
    }
    if (palette->colors && surface->format->palette->colors && count > 0) {
        memcpy(surface->format->palette->colors, palette->colors, count * sizeof(SDL_Color));
    }
    return 0;
}

SDL_Palette* SDL_AllocPalette(int ncolors)
{
    SDL_Palette *palette = (SDL_Palette*)malloc(sizeof(SDL_Palette));
    if (!palette) return NULL;
    palette->ncolors = ncolors;
    palette->colors = (SDL_Color*)calloc(ncolors, sizeof(SDL_Color));
    return palette;
}

void SDL_FreePalette(SDL_Palette *palette)
{
    if (palette) {
        if (palette->colors) free(palette->colors);
        free(palette);
    }
}

int SDL_SetHint(const char *name, const char *value) { return 1; }
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window) { return 0; }

SDL_Cursor* SDL_CreateSystemCursor(int id) { return &s_dummy_cursor; }
void SDL_SetCursor(SDL_Cursor *cursor) {}
void SDL_FreeCursor(SDL_Cursor *cursor) {}
SDL_Cursor* SDL_GetCursor(void) { return &s_dummy_cursor; }

void SDL_FlushEvents(Uint32 minType, Uint32 maxType) {}
int SDL_SetRelativeMouseMode(int enabled) { return 0; }

// Audio Device & Mixing
static SDL_AudioSpec s_current_audio_spec;

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const SDL_AudioSpec *desired, SDL_AudioSpec *obtained,
                                      int allowed_changes)
{
    if (desired) {
        s_current_audio_spec = *desired;
        if (obtained) *obtained = *desired;
    }
    return 1;
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on) {}
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) {}

void SDL_MixAudioFormat(Uint8 *dst, const Uint8 *src, Uint16 format, Uint32 len, int volume)
{
    if (!dst || !src || len == 0 || volume == 0) return;
    int16_t *d = (int16_t*)dst;
    const int16_t *s = (const int16_t*)src;
    int count = len / 2;
    for (int i = 0; i < count; i++) {
        int32_t val = (int32_t)d[i] + ((int32_t)s[i] * volume / SDL_MIX_MAXVOLUME);
        if (val > 32767) val = 32767;
        else if (val < -32768) val = -32768;
        d[i] = (int16_t)val;
    }
}

// Audio Stream
#define AUDIO_STREAM_BUF_SIZE (64 * 1024)
struct SDL_AudioStream {
    Uint16 src_format;
    Uint8 src_channels;
    int src_rate;
    Uint16 dst_format;
    Uint8 dst_channels;
    int dst_rate;
    uint8_t *buffer;
    int buf_size;
    int read_pos;
    int write_pos;
};

SDL_AudioStream* SDL_NewAudioStream(Uint16 src_format, Uint8 src_channels, int src_rate,
                                    Uint16 dst_format, Uint8 dst_channels, int dst_rate)
{
    SDL_AudioStream *stream = (SDL_AudioStream*)calloc(1, sizeof(SDL_AudioStream));
    if (!stream) return NULL;
    stream->src_format = src_format;
    stream->src_channels = src_channels;
    stream->src_rate = src_rate;
    stream->dst_format = dst_format;
    stream->dst_channels = dst_channels;
    stream->dst_rate = dst_rate;
    stream->buf_size = AUDIO_STREAM_BUF_SIZE;
    stream->buffer = (uint8_t*)malloc(stream->buf_size);
    return stream;
}

void SDL_FreeAudioStream(SDL_AudioStream *stream)
{
    if (stream) {
        if (stream->buffer) free(stream->buffer);
        free(stream);
    }
}

int SDL_AudioStreamPut(SDL_AudioStream *stream, const void *buf, int len)
{
    if (!stream || !stream->buffer || !buf || len <= 0) return -1;
    int available = stream->buf_size - (stream->write_pos - stream->read_pos);
    if (len > available) len = available;
    if (len <= 0) return 0;

    int wp = stream->write_pos % stream->buf_size;
    int first = stream->buf_size - wp;
    if (first > len) first = len;
    memcpy(stream->buffer + wp, buf, first);
    if (len > first) {
        memcpy(stream->buffer, (const uint8_t*)buf + first, len - first);
    }
    stream->write_pos += len;
    return 0;
}

int SDL_AudioStreamGet(SDL_AudioStream *stream, void *buf, int len)
{
    if (!stream || !stream->buffer || !buf || len <= 0) return 0;
    int queued = stream->write_pos - stream->read_pos;
    if (queued <= 0) return 0;
    if (len > queued) len = queued;

    int rp = stream->read_pos % stream->buf_size;
    int first = stream->buf_size - rp;
    if (first > len) first = len;
    memcpy(buf, stream->buffer + rp, first);
    if (len > first) {
        memcpy((uint8_t*)buf + first, stream->buffer, len - first);
    }
    stream->read_pos += len;
    if (stream->read_pos == stream->write_pos) {
        stream->read_pos = stream->write_pos = 0;
    }
    return len;
}

// Timer
typedef struct {
    esp_timer_handle_t timer;
    SDL_TimerCallback callback;
    void *param;
} FalloutTimer;

static FalloutTimer s_timers[4];

static void timer_cb(void *arg)
{
    FalloutTimer *t = (FalloutTimer*)arg;
    if (t && t->callback) {
        t->callback(40, t->param);
    }
}

SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void *param)
{
    for (int i = 0; i < 4; i++) {
        if (!s_timers[i].timer) {
            s_timers[i].callback = callback;
            s_timers[i].param = param;
            esp_timer_create_args_t args = {
                .callback = timer_cb,
                .arg = &s_timers[i],
                .name = "fo_timer"
            };
            esp_timer_create(&args, &s_timers[i].timer);
            esp_timer_start_periodic(s_timers[i].timer, (uint64_t)interval * 1000);
            return i + 1;
        }
    }
    return 0;
}

SDL_bool SDL_RemoveTimer(SDL_TimerID id)
{
    int idx = id - 1;
    if (idx >= 0 && idx < 4 && s_timers[idx].timer) {
        esp_timer_stop(s_timers[idx].timer);
        esp_timer_delete(s_timers[idx].timer);
        s_timers[idx].timer = NULL;
        return SDL_TRUE;
    }
    return SDL_FALSE;
}

// String helpers
int SDL_strcasecmp(const char *s1, const char *s2) { return strcasecmp(s1, s2); }
int SDL_strncasecmp(const char *s1, const char *s2, size_t n) { return strncasecmp(s1, s2, n); }

char* SDL_strupr(char *s)
{
    if (!s) return NULL;
    for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p);
    return s;
}

char* SDL_strlwr(char *s)
{
    if (!s) return NULL;
    for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
    return s;
}

char* SDL_itoa(int value, char *str, int radix)
{
    if (!str) return NULL;
    if (radix == 10) sprintf(str, "%d", value);
    else if (radix == 16) sprintf(str, "%x", (unsigned int)value);
    else if (radix == 8) sprintf(str, "%o", (unsigned int)value);
    else sprintf(str, "%d", value);
    return str;
}

char* SDL_strdup(const char *s) { return strdup(s); }
char* SDL_GetBasePath(void) { return strdup(""); }
const char* SDL_AndroidGetExternalStoragePath(void) { return ""; }
