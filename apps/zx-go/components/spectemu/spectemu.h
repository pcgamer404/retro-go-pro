#ifndef SPECTEMU_H
#define SPECTEMU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZX_JOY_KEMPSTON = 0,
    ZX_JOY_SINCLAIR1,
    ZX_JOY_SINCLAIR2,
    ZX_JOY_CURSOR,
    ZX_JOY_QAOP,
    ZX_JOY_COUNT
} zx_joy_type_t;

// Initialize emulator core (loads ROM, clears memory)
bool spectemu_init(void);

// Reset emulator (soft = keep memory content, hard = reload ROM and clear RAM)
void spectemu_reset(bool hard);

// Load ROM / snapshot (.sna, .z80, .tap)
bool spectemu_load_file(const char *path);

// Save snapshot (.sna)
bool spectemu_save_snapshot(const char *path);

// Run one video frame (50Hz PAL)
void spectemu_run_frame(void);

// Render the RGB565 screen buffer (256x192 borderless or 320x240 with border)
void spectemu_render_screen(uint16_t *dest, int width, int height);

// Fetch beeper audio samples for the frame into stereo 16-bit buffer
// Returns the number of frames (samples per channel)
int spectemu_get_audio(int16_t *dest_stereo, int max_frames);

// Update gamepad buttons and joystick mode
void spectemu_set_input(uint32_t rg_buttons, zx_joy_type_t joy_type);

// Inject a single ASCII key press (held for a few frames)
void spectemu_inject_key(char ascii_char);

// Inject a sequence of ASCII key presses with pacing
void spectemu_inject_string(const char *str);

#ifdef __cplusplus
}
#endif

#endif // SPECTEMU_H
