#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "rg_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Converts Retro-Go's held-button bitmask into SDL2-compatible keyboard
 * transitions. This keeps input polling independent from the SDL event queue;
 * the future SDL compatibility layer can turn each transition into a
 * SDL_KEYDOWN/SDL_KEYUP event for OpenJazz's existing Controls class.
 */
typedef struct {
    int32_t keycode; /* SDL2 SDLK_* numeric value */
    bool pressed;    /* true = keydown, false = keyup */
} oj_rg_key_transition_t;

/* Returns number of transitions written (maximum is OJ_RG_INPUT_MAX_TRANSITIONS).
 * Pass the current rg_input_read_gamepad() mask on each game-loop iteration.
 */
#define OJ_RG_INPUT_MAX_TRANSITIONS 20
int oj_rg_input_update(uint32_t held_mask,
                       oj_rg_key_transition_t out[OJ_RG_INPUT_MAX_TRANSITIONS]);
void oj_rg_input_reset(void);

/* Poll current Retro-Go held buttons and enqueue SDL_KEYDOWN/SDL_KEYUP events.
 * Call from OpenJazz's main loop before it drains SDL_PollEvent(). Returns
 * the number of key transitions queued. */
int oj_rg_input_pump_events(uint32_t held_mask);

#ifdef __cplusplus
} /* extern "C" */
#endif
