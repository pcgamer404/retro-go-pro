#ifndef SDL_h_
#define SDL_h_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#include "SDL_version.h"
#include "SDL_system.h"

#include "SDL_input.h"
#include "SDL_stdinc.h"
#include "SDL_endian.h"
#include "SDL_audio.h"
#include "SDL_wave.h"

#include "SDL_video.h"
#include "SDL_event.h"
#include "SDL_log.h"

#include "SDL_scancode.h"
#include "SDL_blit.h"

typedef int SDLMod;

#define PLATFORM_ESP32 1

#define SDLK_LSUPER SDLK_LGUI
#define SDLK_RSUPER SDLK_RGUI

#define SDLK_FIRST 0
#define SDLK_LAST 1024

#define SDLK_KP4 SDLK_LEFT
#define SDLK_KP6 SDLK_RIGHT
#define SDLK_KP8 SDLK_UP
#define SDLK_KP2 SDLK_DOWN
#define SDLK_KP5 SDLK_BACKSLASH
#define SDLK_KP0 SDLK_RETURN
#define SDLK_KP9 SDLK_PAGEUP
#define SDLK_KP3 SDLK_PAGEDOWN

#define SDLK_KP7 0
#define SDLK_KP1 0
#define SDLK_PRINT 0

#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON_WHEELUP 4
#define SDL_BUTTON_WHEELDOWN 5
#define SDLK_NUMLOCK 999
#define SDLK_SCROLLOCK 1000

#define SDL_strlcpy strlcpy
#define SDL_snprintf snprintf

#define SDL_INIT_TIMER          0x00000001u
#define SDL_INIT_AUDIO          0x00000010u
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_HAPTIC         0x00001000u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_INIT_EVENTS         0x00004000u
#define SDL_INIT_SENSOR         0x00008000u
#define SDL_INIT_NOPARACHUTE    0x00100000u
#define SDL_INIT_EVERYTHING ( \
                SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS | \
                SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_GAMECONTROLLER | SDL_INIT_SENSOR \
            )

/* SDL2 types and constants */
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef struct SDL_Cursor SDL_Cursor;
typedef struct SDL_AudioStream SDL_AudioStream;

#define SDL_WINDOWPOS_UNDEFINED 0
#define SDL_WINDOW_FULLSCREEN 0x00000001
#define SDL_WINDOW_OPENGL 0x00000002
#define SDL_WINDOW_SHOWN 0x00000004
#define SDL_WINDOW_HIDDEN 0x00000008
#define SDL_WINDOW_BORDERLESS 0x00000010
#define SDL_WINDOW_RESIZABLE 0x00000020
#define SDL_WINDOW_ALLOW_HIGHDPI 0x00002000

#define SDL_PIXELFORMAT_UNKNOWN 0
#define SDL_PIXELFORMAT_INDEX8 1
#define SDL_PIXELFORMAT_RGB888 2
#define SDL_PIXELFORMAT_RGBA8888 3
#define SDL_PIXELFORMAT_RGB565 4

#define SDL_TEXTUREACCESS_STATIC 0
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_TEXTUREACCESS_TARGET 2

#define SDL_BITSPERPIXEL(X) 8

#define SDL_HINT_RENDER_DRIVER "SDL_RENDER_DRIVER"
#define SDL_HINT_MOUSE_TOUCH_EVENTS "SDL_MOUSE_TOUCH_EVENTS"
#define SDL_HINT_TOUCH_MOUSE_EVENTS "SDL_TOUCH_MOUSE_EVENTS"

#define SDL_SYSTEM_CURSOR_ARROW 0
#define SDL_DISABLE 0
#define SDL_ENABLE 1
#define SDL_MIX_MAXVOLUME 128
#define SDL_AUDIO_ALLOW_ANY_CHANGE 0

#define SDL_MESSAGEBOX_ERROR 0x00000010
#define SDL_MESSAGEBOX_WARNING 0x00000020
#define SDL_MESSAGEBOX_INFORMATION 0x00000040

/* SDL2 Subsystem */
int SDL_Init(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
void SDL_QuitSubSystem(Uint32 flags);
Uint32 SDL_WasInit(Uint32 flags);
void SDL_Quit(void);

/* SDL2 Window / Renderer / Texture */
SDL_Window* SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window *window);
void SDL_SetWindowTitle(SDL_Window *window, const char *title);
SDL_Renderer* SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer *renderer);
int SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h);
int SDL_RenderClear(SDL_Renderer *renderer);
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect);
void SDL_RenderPresent(SDL_Renderer *renderer);
SDL_Texture* SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access, int w, int h);
void SDL_DestroyTexture(SDL_Texture *texture);
int SDL_QueryTexture(SDL_Texture *texture, Uint32 *format, int *access, int *w, int *h);
int SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch);
SDL_Surface* SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width, int height, int depth, Uint32 format);
int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors, int firstcolor, int ncolors);
int SDL_SetSurfacePalette(SDL_Surface *surface, SDL_Palette *palette);
SDL_Palette* SDL_AllocPalette(int ncolors);
void SDL_FreePalette(SDL_Palette *palette);
int SDL_SetHint(const char *name, const char *value);
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window);

/* SDL2 Cursor & Input */
SDL_Cursor* SDL_CreateSystemCursor(int id);
void SDL_SetCursor(SDL_Cursor *cursor);
void SDL_FreeCursor(SDL_Cursor *cursor);
SDL_Cursor* SDL_GetCursor(void);
void SDL_PumpEvents(void);
void SDL_FlushEvents(Uint32 minType, Uint32 maxType);
Uint32 SDL_GetRelativeMouseState(int *x, int *y);
int SDL_SetRelativeMouseMode(int enabled);
static inline const Uint8* SDL_GetKeyboardState(int *numkeys) { return SDL_GetKeyState(numkeys); }

/* SDL2 Audio */
typedef struct SDL_AudioSpec SDL_AudioSpec;
#ifndef SDL_AUDIO_DEVICE_ID_DEFINED
#define SDL_AUDIO_DEVICE_ID_DEFINED
typedef Uint32 SDL_AudioDeviceID;
#endif
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed_changes);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
void SDL_MixAudioFormat(Uint8 *dst, const Uint8 *src, Uint16 format, Uint32 len, int volume);
SDL_AudioStream* SDL_NewAudioStream(Uint16 src_format, Uint8 src_channels, int src_rate, Uint16 dst_format, Uint8 dst_channels, int dst_rate);
int SDL_AudioStreamPut(SDL_AudioStream *stream, const void *buf, int len);
int SDL_AudioStreamGet(SDL_AudioStream *stream, void *buf, int len);
void SDL_FreeAudioStream(SDL_AudioStream *stream);

/* SDL Timer */
typedef int SDL_TimerID;
typedef Uint32 (*SDL_TimerCallback)(Uint32 interval, void *param);
SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void *param);
SDL_bool SDL_RemoveTimer(SDL_TimerID id);

/* String & platform */
int SDL_strcasecmp(const char *s1, const char *s2);
int SDL_strncasecmp(const char *s1, const char *s2, size_t n);
char* SDL_strupr(char *s);
char* SDL_strlwr(char *s);
char* SDL_itoa(int value, char *str, int radix);
char* SDL_strdup(const char *s);
char* SDL_GetBasePath(void);
const char* SDL_AndroidGetExternalStoragePath(void);

/* SDL TextInput */
static inline void SDL_StartTextInput(void) {}
static inline void SDL_StopTextInput(void) {}
static inline SDL_bool SDL_IsTextInputActive(void) { return SDL_FALSE; }
static inline void SDL_SetTextInputRect(const SDL_Rect *rect) { (void)rect; }

#ifdef __cplusplus
}
#endif

#endif
