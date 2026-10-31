#include "retrogo_video_adapter.h"
#include "rg_system.h"
#include "rg_display.h"
#include "rg_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

/* Keep one persistent surface: rg_display_submit queues a pointer asynchronously. */
static rg_surface_t *s_frame = NULL;

bool oj_rg_present_indexed(const uint8_t *pixels, int width, int height, int pitch,
                           const oj_rg_palette_color_t palette[256])
{
    if (!pixels || !palette || width <= 0 || height <= 0 || pitch < width)
        return false;

    if (s_frame && (s_frame->width != width || s_frame->height != height)) {
        while (rg_display_is_busy()) vTaskDelay(1);
        rg_display_sync();
        rg_surface_free(s_frame);
        s_frame = NULL;
    }
    if (!s_frame) {
        s_frame = rg_surface_create(width, height, RG_PIXEL_565_LE, MEM_SLOW);
        if (!s_frame || !s_frame->data) {
            if (s_frame) rg_surface_free(s_frame);
            s_frame = NULL;
            return false;
        }
    }

    /* The previous submit may still be queued or being flushed to the LCD. */
    while (rg_display_is_busy()) vTaskDelay(1);
    rg_display_sync();

    uint16_t *dst = (uint16_t *)s_frame->data;
    const int dst_stride = s_frame->stride / (int)sizeof(uint16_t);
    if (!oj_indexed_to_rgb565(pixels, width, height, pitch, dst, dst_stride, palette))
        return false;

    rg_display_submit(s_frame, 0);
    /* Tell Retro-Go the app is alive (FPS counters, and avoids the
     * "App unresponsive" overlay when MENU is tapped). */
    rg_system_tick(0);
    return true;
}

/* Last presented frame (RGB565). Used for screenshots and for redraws after Retro-Go
 * closes its own menu. */
rg_surface_t *oj_rg_get_frame(void)
{
    return s_frame;
}
