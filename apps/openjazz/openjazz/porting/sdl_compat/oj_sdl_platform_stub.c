#include "oj_sdl_compat.h"
#include <stddef.h>

static const SDL_version s_version = { 2, 0, 0 };
const SDL_version *SDL_Linked_Version(void) { return &s_version; }

static const char s_no_error[] = "no SDL platform services in Retro-Go mode";
int SDL_Init(Uint32 flags) { (void)flags; return 0; }
void SDL_Quit(void) { }
int SDL_ShowCursor(int toggle) { (void)toggle; return 0; }
const char *SDL_GetError(void) { return s_no_error; }
char *SDL_GetKeyName(Sint32 key)
{
    switch (key) {
    case SDLK_RETURN: return (char *)"Return";
    case SDLK_ESCAPE: return (char *)"Escape";
    case SDLK_SPACE: return (char *)"Space";
    case SDLK_UP: return (char *)"Up";
    case SDLK_DOWN: return (char *)"Down";
    case SDLK_LEFT: return (char *)"Left";
    case SDLK_RIGHT: return (char *)"Right";
    default: return (char *)"Unknown";
    }
}
int SDL_NumJoysticks(void) { return 0; }
int SDL_HasJoystick(void) { return 0; }
SDL_Joystick *SDL_JoystickOpen(int index) { (void)index; return NULL; }
void SDL_JoystickClose(SDL_Joystick *joystick) { (void)joystick; }
int SDL_OpenJoystick(int index) { (void)index; return 0; }
void SDL_CloseJoystick(int index) { (void)index; }
void SDL_WM_SetCaption(const char *title, const char *icon) { (void)title; (void)icon; }
int SDL_SaveBMP(SDL_Surface *surface, const char *file) { (void)surface; (void)file; return -1; }
