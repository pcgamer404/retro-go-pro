#include "retrogo_input_adapter.h"
#include "sdl_compat/oj_sdl_event_compat.h"
#include "sdl_compat/oj_sdl_audio_compat.h"
#include "rg_system.h"

/* SDL2 keycode values (SDL_Keycode = scancode | SDLK_SCANCODE_MASK for
 * non-printable keys). We deliberately avoid including SDL.h here so this
 * adapter does not depend on the full SDL compatibility layer.
 */
enum {
    OJ_SDLK_RETURN = 13,
    OJ_SDLK_ESCAPE = 27,
    OJ_SDLK_SPACE = 32,
    OJ_SDLK_p = 112,
    OJ_SDLK_UP = 1073741906,
    OJ_SDLK_DOWN = 1073741905,
    OJ_SDLK_RIGHT = 1073741903,
    OJ_SDLK_LEFT = 1073741904,
    OJ_SDLK_LALT = 1073742050,
    OJ_SDLK_RCTRL = 1073742052,
};

typedef struct {
    uint32_t rg_mask;
    int32_t sdl_key;
} input_binding_t;

/* START and SELECT are not in this table: Retro-Go builds its MENU combo
 * (START+SELECT) and OPTION combo (START+B) out of them, so they act as a
 * "tap" on release instead (see oj_rg_input_pump_events). */
static const input_binding_t s_bindings[] = {
    { RG_KEY_UP,     OJ_SDLK_UP },
    { RG_KEY_DOWN,   OJ_SDLK_DOWN },
    { RG_KEY_LEFT,   OJ_SDLK_LEFT },
    { RG_KEY_RIGHT,  OJ_SDLK_RIGHT },
    { RG_KEY_A,      OJ_SDLK_SPACE },  /* jump/swim */
    { RG_KEY_A,      OJ_SDLK_RETURN }, /* ...and "select" in menus (Enter only acts in menus/cutscenes) */
    { RG_KEY_B,      OJ_SDLK_LALT },   /* fire */
    { RG_KEY_X,      OJ_SDLK_RCTRL },  /* change weapon */
    { RG_KEY_Y,      OJ_SDLK_RETURN }, /* Start */
    { RG_KEY_R,      OJ_SDLK_p },      /* pause */
};
#define BINDING_COUNT (sizeof(s_bindings) / sizeof(s_bindings[0]))

#define SYSTEM_KEYS (RG_KEY_START | RG_KEY_SELECT | RG_KEY_MENU | RG_KEY_OPTION)

static uint32_t s_previous_mask;

int oj_rg_input_update(uint32_t held_mask,
                       oj_rg_key_transition_t out[OJ_RG_INPUT_MAX_TRANSITIONS])
{
    if (!out) return 0;
    held_mask &= ~SYSTEM_KEYS;
    int count = 0;
    for (unsigned i = 0; i < BINDING_COUNT; ++i) {
        const uint32_t mask = s_bindings[i].rg_mask;
        const bool was_down = (s_previous_mask & mask) != 0;
        const bool is_down = (held_mask & mask) != 0;
        if (was_down == is_down) continue;
        if (count < OJ_RG_INPUT_MAX_TRANSITIONS) {
            out[count].keycode = s_bindings[i].sdl_key;
            out[count].pressed = is_down;
            ++count;
        }
    }
    s_previous_mask = held_mask;
    return count;
}

void oj_rg_input_reset(void)
{
    s_previous_mask = 0;
}

static int push_transitions(uint32_t held_mask)
{
    oj_rg_key_transition_t transitions[OJ_RG_INPUT_MAX_TRANSITIONS];
    int count = oj_rg_input_update(held_mask, transitions);
    for (int i = 0; i < count; ++i)
        oj_sdl_push_key_event(transitions[i].keycode, transitions[i].pressed);
    return count;
}

/* Open Retro-Go's own menu (save/load state, reset, screenshot, scaling, quit...).
 * The engine is blocked here, so hide the time and silence the audio meanwhile. */
static void open_retrogo_menu(uint32_t held_mask)
{
    oj_sdl_audio_suspend(1);
    const Uint32 t0 = SDL_GetTicks();
    if (held_mask & RG_KEY_MENU)
        rg_gui_game_menu();
    else
        rg_gui_options_menu();
    oj_sdl_time_skip(SDL_GetTicks() - t0);
    oj_sdl_audio_suspend(0);
    oj_sdl_event_queue_clear();
}

int oj_rg_input_pump_events(uint32_t held_mask)
{
    static bool menu_latched;       /* menu already opened for the current press */
    static bool combo_seen;         /* START/SELECT are part of a combo: no tap */
    static uint32_t system_prev;    /* START/SELECT state last time */
    static int32_t release_next[2]; /* key-ups owed from last frame (taps) */
    static int release_count;
    int count = 0;

    /* A tap needs its key-up one frame after the key-down, otherwise the engine
     * sees both in one update and never notices the press. */
    for (int i = 0; i < release_count; ++i, ++count)
        oj_sdl_push_key_event(release_next[i], 0);
    release_count = 0;

    if (held_mask & (RG_KEY_MENU | RG_KEY_OPTION)) {
        combo_seen = true;
        if (!menu_latched) {
            menu_latched = true;
            count += push_transitions(0);   /* let go of everything first */
            open_retrogo_menu(held_mask);
            system_prev = held_mask & (RG_KEY_START | RG_KEY_SELECT);
            return count;
        }
    } else {
        menu_latched = false;
    }

    count += push_transitions(held_mask);

    const uint32_t system_now = held_mask & (RG_KEY_START | RG_KEY_SELECT);
    const uint32_t released = system_prev & ~system_now;
    if (released && !combo_seen) {
        if (released & RG_KEY_SELECT) {
            oj_sdl_push_key_event(OJ_SDLK_ESCAPE, 1);
            release_next[release_count++] = OJ_SDLK_ESCAPE;
            ++count;
        }
        if (released & RG_KEY_START) {
            oj_sdl_push_key_event(OJ_SDLK_RETURN, 1);
            release_next[release_count++] = OJ_SDLK_RETURN;
            ++count;
        }
    }
    if (!system_now && !(held_mask & (RG_KEY_MENU | RG_KEY_OPTION)))
        combo_seen = false;
    system_prev = system_now;

    return count;
}
