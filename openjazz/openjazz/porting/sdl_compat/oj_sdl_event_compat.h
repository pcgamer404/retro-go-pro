#ifndef OJ_SDL_EVENT_COMPAT_H
#define OJ_SDL_EVENT_COMPAT_H
/* Small SDL2-shaped event/time subset for the OpenJazz Retro-Go port. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef int16_t Sint16;

enum {
    SDL_QUIT = 0x100,
    SDL_KEYDOWN = 0x300,
    SDL_KEYUP = 0x301,
    SDL_JOYAXISMOTION = 0x600,
    SDL_JOYHATMOTION = 0x602,
    SDL_JOYBUTTONDOWN = 0x603,
    SDL_JOYBUTTONUP = 0x604,
    SDL_MOUSEMOTION = 0x400,
    SDL_MOUSEBUTTONDOWN = 0x401,
    SDL_MOUSEBUTTONUP = 0x402,
    SDL_EVENT_QUIT = SDL_QUIT,
    SDL_EVENT_KEY_DOWN = SDL_KEYDOWN,
    SDL_EVENT_KEY_UP = SDL_KEYUP,
    SDL_EVENT_JOYSTICK_AXIS_MOTION = SDL_JOYAXISMOTION,
    SDL_EVENT_JOYSTICK_HAT_MOTION = SDL_JOYHATMOTION,
    SDL_EVENT_JOYSTICK_BUTTON_DOWN = SDL_JOYBUTTONDOWN,
    SDL_EVENT_JOYSTICK_BUTTON_UP = SDL_JOYBUTTONUP,
    SDL_EVENT_MOUSE_MOTION = SDL_MOUSEMOTION,
    SDL_EVENT_MOUSE_BUTTON_DOWN = SDL_MOUSEBUTTONDOWN,
    SDL_EVENT_MOUSE_BUTTON_UP = SDL_MOUSEBUTTONUP
};

enum {
    SDLK_RETURN = 13, SDLK_ESCAPE = 27, SDLK_SPACE = 32,
    SDLK_p = 112, SDLK_UP = 1073741906, SDLK_DOWN = 1073741905,
    SDLK_RIGHT = 1073741903, SDLK_LEFT = 1073741904,
    SDLK_LALT = 1073742050, SDLK_RCTRL = 1073742052,
    SDLK_LCTRL = 1073742048, SDLK_RALT = 1073742054,
    SDLK_LSHIFT = 1073742049, SDLK_BACKSPACE = 8, SDLK_TAB = 9,
    SDLK_DELETE = 127, SDLK_F9 = 1073741890,
    SDLK_1 = '1', SDLK_2 = '2', SDLK_3 = '3', SDLK_4 = '4', SDLK_5 = '5',
    SDLK_j = 'j', SDLK_k = 'k', SDLK_n = 'n', SDLK_o = 'o', SDLK_y = 'y',
    SDLK_N = 'N', SDLK_Y = 'Y', SDLK_P = 'P', SDLK_AC_BACK = 1073742094
};
#define SDL_KMOD_ALT 0x0300u
#define KMOD_ALT SDL_KMOD_ALT
#define SDL_BUTTON_LEFT 1
#define SDL_HAT_CENTERED 0x00
#define SDL_HAT_UP 0x01
#define SDL_HAT_RIGHT 0x02
#define SDL_HAT_DOWN 0x04
#define SDL_HAT_LEFT 0x08

typedef struct SDL_Keysym { Sint32 sym; Uint16 mod; Uint16 unused; } SDL_Keysym;
typedef struct SDL_KeyboardEvent { Uint32 type; Uint8 repeat; Uint8 padding2, padding3; SDL_Keysym keysym; Sint32 key; Uint16 mod; } SDL_KeyboardEvent;
typedef struct SDL_JoyButtonEvent { Uint32 type; Uint8 which, button, state, padding1; } SDL_JoyButtonEvent;
typedef struct SDL_JoyAxisEvent { Uint32 type; Uint8 which, axis; Sint16 value; } SDL_JoyAxisEvent;
typedef struct SDL_JoyHatEvent { Uint32 type; Uint8 which, hat, value, padding1; } SDL_JoyHatEvent;
typedef struct SDL_MouseMotionEvent { Uint32 type; Uint32 state; Sint32 x, y, xrel, yrel; } SDL_MouseMotionEvent;
typedef struct SDL_MouseButtonEvent { Uint32 type; Uint8 button, state, clicks, padding1; Sint32 x, y; } SDL_MouseButtonEvent;
typedef union SDL_Event {
    Uint32 type;
    union {
        SDL_KeyboardEvent key;
        SDL_JoyButtonEvent jbutton;
        SDL_JoyAxisEvent jaxis;
        SDL_JoyHatEvent jhat;
        SDL_MouseMotionEvent motion;
        SDL_MouseButtonEvent button;
    };
} SDL_Event;

/* Queue semantics mirror SDL_PollEvent: 1 when an event was returned. */
int SDL_PollEvent(SDL_Event *event);
int SDL_PushEvent(SDL_Event *event);
void oj_sdl_event_queue_clear(void);
void oj_sdl_push_key_event(Sint32 keycode, int pressed);
Uint32 SDL_GetTicks(void);
/* Port extension: hide time spent in Retro-Go's menu from the game clock. */
void oj_sdl_time_skip(Uint32 ms);
void SDL_Delay(Uint32 ms);
#ifdef __cplusplus
}
#endif
#endif
