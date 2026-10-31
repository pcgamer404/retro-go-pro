// Thin C bridge between ScummVM (C++, its own MIN/MAX/byte/uint types) and Retro-Go.
// ScummVM files include ONLY this header, never rg_*.h, so the two headers never meet.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Buttons (bitmask returned by rgx_buttons)
enum {
    RGX_UP = 1 << 0, RGX_DOWN = 1 << 1, RGX_LEFT = 1 << 2, RGX_RIGHT = 1 << 3,
    RGX_A = 1 << 4, RGX_B = 1 << 5, RGX_X = 1 << 6, RGX_Y = 1 << 7,
    RGX_START = 1 << 8, RGX_SELECT = 1 << 9, RGX_L = 1 << 10, RGX_R = 1 << 11,
    RGX_MENU = 1 << 12, RGX_OPTION = 1 << 13,
};

// Video: one 8-bit paletted surface, ScummVM's framebuffer is copied into it on present
void     rgx_video_init(int width, int height);
uint8_t *rgx_video_pixels(void);                       // width*height bytes, stride == width
void     rgx_video_set_palette(const uint8_t *rgb, int start, int count);
void     rgx_progress(uint32_t done_kb, uint32_t total_kb, int stage); // simple bar screen
void     rgx_video_present(void);                      // pushes palette + frame, blocks until consumed

// Audio: callback fills 'frames' interleaved stereo int16 frames
typedef void (*rgx_audio_cb)(int16_t *stereo, int frames, void *userdata);
void rgx_audio_start(rgx_audio_cb cb, void *userdata);
void rgx_audio_pause(bool pause);
void rgx_audio_stop(void);
bool rgx_sound_enabled(void);

// Input / menus
uint32_t rgx_buttons(void);
void     rgx_menu(bool options);                       // false = game menu, true = options menu
int      rgx_cursor_speed(void);                       // 0 slow, 1 normal, 2 fast
int      rgx_take_quicksave_request(void);             // 0 none, 1 save, 2 load (set by Retro-Go save/load slots)
void     rgx_request_quicksave(int what);

// Time / sync / lifecycle
int64_t rgx_time_us(void);
void    rgx_sleep_us(uint64_t us);
void    rgx_tick(int64_t busy_us);
void   *rgx_mutex_create(void);
void    rgx_mutex_lock(void *m);
void    rgx_mutex_unlock(void *m);
void    rgx_mutex_delete(void *m);
void    rgx_exit(void);

// Game location from the launched file (romPath). Fills dir and an optional game id hint.
bool rgx_game_location(char *dir, int dir_size, char *gameid, int gameid_size);

// Surface access for screenshots (main.c only; opaque to ScummVM code)
void *rgx_get_surface(void);

#ifdef __cplusplus
}
#endif
