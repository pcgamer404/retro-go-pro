#include "SDL_event.h"
#include "rg_system.h"
#include "rg_input.h"
#include "rg_gui.h"
#include <string.h>

// Replaces the original raw-GPIO / ISR-queue implementation. retro-go
// already debounces and abstracts the gamepad for us, so this is a plain
// edge-detector: on each RG_KEY_* transition we synthesize one SDL_KEYDOWN
// or SDL_KEYUP event, queued and drained by SDL_PollEvent() exactly like
// the original did from its GPIO ISR queue.

typedef struct {
    uint32_t rg_key;
    SDL_Scancode scancode;
    SDL_Keycode keycode;
} key_mapping_t;

// Tyrian's default bindings (see config.c): UP/DOWN/LEFT/RIGHT, SPACE
// (main fire), RETURN (change fire / confirm), LCTRL (rear sidekick),
// LALT (front sidekick). ESCAPE is the in-game menu/back key.
// Adjust freely to taste -- these are just sane defaults for a 2-stick-less
// handheld with A/B/X/Y/Start/Select.
static const key_mapping_t keymap[] = {
    {RG_KEY_UP,     SDL_SCANCODE_UP,     SDLK_UP},
    {RG_KEY_DOWN,   SDL_SCANCODE_DOWN,   SDLK_DOWN},
    {RG_KEY_LEFT,   SDL_SCANCODE_LEFT,   SDLK_LEFT},
    {RG_KEY_RIGHT,  SDL_SCANCODE_RIGHT,  SDLK_RIGHT},
    {RG_KEY_A,      SDL_SCANCODE_SPACE,  SDLK_SPACE},   // main fire
    {RG_KEY_B,      SDL_SCANCODE_LCTRL,  SDLK_LCTRL},   // rear sidekick
    {RG_KEY_Y,      SDL_SCANCODE_LALT,   SDLK_LALT},    // front sidekick
    {RG_KEY_X,      SDL_SCANCODE_RETURN, SDLK_RETURN},  // change fire / confirm
    {RG_KEY_SELECT, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE},  // back / in-game menu
};
#define KEYMAP_COUNT (sizeof(keymap) / sizeof(keymap[0]))

#define EVENT_QUEUE_LEN 16
static SDL_Event event_queue[EVENT_QUEUE_LEN];
static int queue_head = 0;
static int queue_tail = 0;

static inline void push_event(Uint32 type, SDL_Scancode sc, SDL_Keycode kc)
{
    int next = (queue_tail + 1) % EVENT_QUEUE_LEN;
    if (next == queue_head)
        return; // queue full, drop (shouldn't happen with 16 slots / 9 keys)

    SDL_Event *ev = &event_queue[queue_tail];
    memset(ev, 0, sizeof(*ev));
    ev->type = type;
    ev->key.type = type;
    ev->key.state = (type == SDL_KEYDOWN) ? SDL_PRESSED : SDL_RELEASED;
    ev->key.keysym.scancode = sc;
    ev->key.keysym.sym = kc;
    ev->key.keysym.mod = 0;
    queue_tail = next;
}

static void poll_gamepad(void)
{
    static uint32_t last_state = 0;
    uint32_t state = rg_input_read_gamepad();

    // RG_KEY_MENU is reserved for retro-go's own game menu, not forwarded
    // to Tyrian.
    if ((state & RG_KEY_MENU) && !(last_state & RG_KEY_MENU))
    {
        rg_gui_game_menu();
        state = rg_input_read_gamepad(); // re-sample after the menu closes
    }

    uint32_t changed = state ^ last_state;
    if (changed)
    {
        for (size_t i = 0; i < KEYMAP_COUNT; i++)
        {
            if (changed & keymap[i].rg_key)
            {
                bool pressed = state & keymap[i].rg_key;
                push_event(pressed ? SDL_KEYDOWN : SDL_KEYUP, keymap[i].scancode, keymap[i].keycode);
            }
        }
    }
    last_state = state;
}

int SDL_PollEvent(SDL_Event *event)
{
    poll_gamepad();

    if (queue_head == queue_tail)
        return 0;

    if (event)
        *event = event_queue[queue_head];
    queue_head = (queue_head + 1) % EVENT_QUEUE_LEN;
    return 1;
}
