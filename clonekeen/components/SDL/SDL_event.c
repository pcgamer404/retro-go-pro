// SDL_event.c -- retro-go backend
//
// Original implementation read hardware buttons via GPIO interrupts and
// pushed synthesized SDL_KEYDOWN/KEYUP events into a queue. This version
// polls retro-go's rg_input_read_gamepad() bitmask instead and does the
// same edge-detection + key synthesis in software, so keen's game/menu
// code (which only ever calls SDL_PollEvent) needs no changes.

#include "SDL_event.h"
#include <rg_system.h>

// Same key mapping as the original non-ODROID-GO keymap: D-pad + two
// action buttons in-game, two menu buttons in menus. Adjust freely to
// taste -- these SDLK_* values are what components/keen's input code
// checks for.
typedef struct {
    uint32_t rg_key;
    SDL_Scancode scancode;
    SDL_Keycode keycode;
} RGKeyMap;

int keyMode = 1;          // kept for compatibility (referenced elsewhere)
int weaponToggle = 1;
int volumeToggle = 1;

static const RGKeyMap keymap[6] = {
    {RG_KEY_UP,     SDL_SCANCODE_UP,    SDLK_UP},
    {RG_KEY_RIGHT,  SDL_SCANCODE_RIGHT, SDLK_RIGHT},
    {RG_KEY_DOWN,   SDL_SCANCODE_DOWN,  SDLK_DOWN},
    {RG_KEY_LEFT,   SDL_SCANCODE_LEFT,  SDLK_LEFT},
    {RG_KEY_A,      SDL_SCANCODE_LCTRL, SDLK_LCTRL},   // shoot
    {RG_KEY_B,      SDL_SCANCODE_SPACE, SDLK_SPACE},   // jump/open
};
#define NUM_MAPPED_KEYS (sizeof(keymap) / sizeof(keymap[0]))

// Extra buttons that aren't part of the 6-button game map but still need
// to reach the game as key events (start/menu/select).
static const RGKeyMap extra_keymap[3] = {
    {RG_KEY_START,  SDL_SCANCODE_RETURN,   SDLK_RETURN},
    {RG_KEY_MENU,   SDL_SCANCODE_ESCAPE,   SDLK_ESCAPE},
    {RG_KEY_SELECT, SDL_SCANCODE_CAPSLOCK, SDLK_CAPSLOCK},
};
#define NUM_EXTRA_KEYS (sizeof(extra_keymap) / sizeof(extra_keymap[0]))

static bool initInput = false;
static uint32_t lastJoystick = 0;

void inputInit(void)
{
    initInput = true;
    lastJoystick = rg_input_read_gamepad();
}

// Fills `event` and returns 1 if a key transition happened, 0 otherwise.
// Called repeatedly by SDL_PollEvent (once per game-loop iteration) until
// it returns 0, exactly like the original ISR-queue-draining version did.
int SDL_PollEvent(SDL_Event *event)
{
    if (!initInput)
        inputInit();

    uint32_t joystick = rg_input_read_gamepad();
    uint32_t changed = joystick ^ lastJoystick;

    if (changed == 0)
    {
        lastJoystick = joystick;
        return 0;
    }

    for (int i = 0; i < (int)NUM_MAPPED_KEYS; i++)
    {
        if (changed & keymap[i].rg_key)
        {
            bool down = (joystick & keymap[i].rg_key) != 0;
            event->key.keysym.scancode = keymap[i].scancode;
            event->key.keysym.sym = keymap[i].keycode;
            event->key.keysym.mod = 0;
            event->key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
            event->key.state = down ? SDL_PRESSED : SDL_RELEASED;
            // Only mark this one bit consumed so remaining changes are
            // reported on subsequent SDL_PollEvent calls this frame.
            lastJoystick ^= keymap[i].rg_key;
            return 1;
        }
    }

    for (int i = 0; i < (int)NUM_EXTRA_KEYS; i++)
    {
        if (changed & extra_keymap[i].rg_key)
        {
            bool down = (joystick & extra_keymap[i].rg_key) != 0;
            event->key.keysym.scancode = extra_keymap[i].scancode;
            event->key.keysym.sym = extra_keymap[i].keycode;
            event->key.keysym.mod = 0;
            event->key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
            event->key.state = down ? SDL_PRESSED : SDL_RELEASED;

            if (extra_keymap[i].rg_key == RG_KEY_SELECT && down)
            {
                volumeToggle++;
                if (volumeToggle > 4)
                    volumeToggle = 0;
                extern char global_volume;
                extern char volumeLevel[];
                global_volume = volumeLevel[volumeToggle];
            }

            lastJoystick ^= extra_keymap[i].rg_key;
            return 1;
        }
    }

    // Any remaining changed bits belong to keys we don't map (unlikely) --
    // clear them so we don't loop forever.
    lastJoystick = joystick;
    return 0;
}

int SDL_WaitEvent(SDL_Event *event)
{
    return 0;
}

Uint8 SDL_EventState(Uint32 type, int state)
{
    return 0;
}
