#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * First Retro-Go platform bridge for DevilutionX-Go.
 *
 * This API is deliberately independent of SDL. It provides the basic services
 * a future SDL/platform adapter will need. It does NOT make upstream
 * DevilutionX compile or run by itself.
 */
typedef enum {
    DXGO_KEY_UP      = 1u << 0,
    DXGO_KEY_DOWN    = 1u << 1,
    DXGO_KEY_LEFT    = 1u << 2,
    DXGO_KEY_RIGHT   = 1u << 3,
    DXGO_KEY_CONFIRM = 1u << 4,
    DXGO_KEY_CANCEL  = 1u << 5,
    DXGO_KEY_ACTION1 = 1u << 6,
    DXGO_KEY_ACTION2 = 1u << 7,
    DXGO_KEY_START   = 1u << 8,
    DXGO_KEY_SELECT  = 1u << 9,
    DXGO_KEY_MENU    = 1u << 10,
    DXGO_KEY_L       = 1u << 11,
    DXGO_KEY_R       = 1u << 12,
} dxgo_key_t;

/* Call after rg_system_init() has initialized the Retro-Go display. */
bool dxgo_platform_video_init(int source_width, int source_height);
void dxgo_platform_video_deinit(void);

/*
 * Present an RGB565 little-endian frame. stride_pixels is the number of
 * uint16_t pixels between source rows (must be >= width). The bridge scales
 * to the physical Retro-Go display using nearest-neighbour surface copying.
 */
bool dxgo_platform_video_present_rgb565(const uint16_t *pixels,
                                        int width,
                                        int height,
                                        int stride_pixels);

/* Current held-button state, mapped from Retro-Go's gamepad bitfield. */
uint32_t dxgo_platform_poll_keys(void);

/* Submit interleaved signed 16-bit stereo samples through Retro-Go audio. */
bool dxgo_platform_submit_audio_s16(const int16_t *interleaved_stereo,
                                    size_t frame_count);

#ifdef __cplusplus
}
#endif
