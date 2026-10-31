#ifndef OJ_SDL_COMPAT_H
#define OJ_SDL_COMPAT_H
/* Retro-Go-specific SDL compatibility surface/event subset. Not a full SDL ABI. */
#include <stddef.h>
#include "oj_sdl_surface_compat.h"
#include "oj_sdl_event_compat.h"
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SDL_DISABLE 0
#define SDL_ENABLE 1
#define SDL_HWSURFACE 0
#define SDL_SWSURFACE 0
#define SDL_HWPALETTE 0
#define SDL_DOUBLEBUF 0
#define SDL_FULLSCREEN 0
#define SDL_RESIZABLE 0
#define SDL_VIDEOEXPOSE 0x401
#define SDL_VIDEORESIZE 0x402
#define SDL_MESSAGEBOX_ERROR 0
#define SDL_INIT_TIMER 0x00000001u
#define SDL_INIT_AUDIO 0x00000010u
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_INIT_JOYSTICK 0x00000200u
#define SDL_BUTTON(X) (1u << ((X) - 1))
typedef struct SDL_version { Uint8 major, minor, patch; } SDL_version;
#define SDL_VERSION(X) do { (X)->major = 2; (X)->minor = 0; (X)->patch = 0; } while (0)
const SDL_version *SDL_Linked_Version(void);
typedef Sint32 SDLKey;
typedef struct SDL_Joystick SDL_Joystick;
int SDL_Init(Uint32 flags);
void SDL_Quit(void);
int SDL_ShowCursor(int toggle);
const char *SDL_GetError(void);
char *SDL_GetKeyName(Sint32 key);
int SDL_NumJoysticks(void);
int SDL_HasJoystick(void);
SDL_Joystick *SDL_JoystickOpen(int index);
void SDL_JoystickClose(SDL_Joystick *joystick);
int SDL_OpenJoystick(int index);
void SDL_CloseJoystick(int index);
void SDL_WM_SetCaption(const char *title, const char *icon);
int SDL_SaveBMP(SDL_Surface *surface, const char *file);
#ifdef __cplusplus
}
#endif
#endif
