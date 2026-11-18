#include "devilutionx_platform.h"

#include <string.h>

#include "esp_log.h"
#include "rg_system.h"
#include "rg_utils.h"

static const char *TAG = "devilutionx-platform";
static rg_surface_t *source_surface;
static rg_surface_t *screen_surface;
static int screen_width;
static int screen_height;

static void release_surfaces(void)
{
    if (source_surface != NULL) {
        rg_surface_free(source_surface);
        source_surface = NULL;
    }
    if (screen_surface != NULL) {
        rg_surface_free(screen_surface);
        screen_surface = NULL;
    }
    screen_width = 0;
    screen_height = 0;
}

bool dxgo_platform_video_init(int source_width, int source_height)
{
    release_surfaces();

    if (source_width < 1 || source_height < 1 || source_width > 640 || source_height > 480) {
        ESP_LOGE(TAG, "Unsupported source resolution %dx%d (limit 640x480)",
                 source_width, source_height);
        return false;
    }

    screen_width = rg_display_get_width();
    screen_height = rg_display_get_height();
    if (screen_width < 1 || screen_height < 1) {
        ESP_LOGE(TAG, "Retro-Go display is not initialized");
        release_surfaces();
        return false;
    }

    source_surface = rg_surface_create(source_width, source_height, RG_PIXEL_565_LE, MEM_SLOW);
    screen_surface = rg_surface_create(screen_width, screen_height, RG_PIXEL_565_LE, MEM_SLOW);
    if (source_surface == NULL || screen_surface == NULL) {
        ESP_LOGE(TAG, "Framebuffer allocation failed for %dx%d -> %dx%d",
                 source_width, source_height, screen_width, screen_height);
        release_surfaces();
        return false;
    }

    ESP_LOGI(TAG, "RGB565 video bridge ready: %dx%d -> %dx%d",
             source_width, source_height, screen_width, screen_height);
    return true;
}

void dxgo_platform_video_deinit(void)
{
    release_surfaces();
}

bool dxgo_platform_video_present_rgb565(const uint16_t *pixels,
                                        int width,
                                        int height,
                                        int stride_pixels)
{
    if (pixels == NULL || source_surface == NULL || screen_surface == NULL) {
        return false;
    }
    if (width != source_surface->width || height != source_surface->height ||
        stride_pixels < width) {
        ESP_LOGE(TAG, "Frame mismatch: got %dx%d stride=%d; expected %dx%d",
                 width, height, stride_pixels,
                 source_surface->width, source_surface->height);
        return false;
    }

    uint8_t *dst = (uint8_t *)source_surface->data;
    const uint8_t *src = (const uint8_t *)pixels;
    const size_t row_bytes = (size_t)width * sizeof(uint16_t);
    const size_t src_stride_bytes = (size_t)stride_pixels * sizeof(uint16_t);
    for (int y = 0; y < height; ++y) {
        memcpy(dst + (size_t)y * source_surface->stride,
               src + (size_t)y * src_stride_bytes, row_bytes);
    }

    if (!rg_surface_copy(source_surface, NULL, screen_surface, NULL, true)) {
        ESP_LOGE(TAG, "Scaling frame to display failed");
        return false;
    }

    /* Synchronous write: the source buffer remains valid until transfer ends. */
    rg_display_write_rect(0, 0, screen_width, screen_height,
                          screen_surface->stride, screen_surface->data, 0);
    return true;
}

uint32_t dxgo_platform_poll_keys(void)
{
    const uint32_t held = rg_input_read_gamepad();
    uint32_t result = 0;

    if (held & RG_KEY_UP)     result |= DXGO_KEY_UP;
    if (held & RG_KEY_DOWN)   result |= DXGO_KEY_DOWN;
    if (held & RG_KEY_LEFT)   result |= DXGO_KEY_LEFT;
    if (held & RG_KEY_RIGHT)  result |= DXGO_KEY_RIGHT;
    if (held & RG_KEY_A)      result |= DXGO_KEY_CONFIRM;
    if (held & RG_KEY_B)      result |= DXGO_KEY_CANCEL;
    if (held & RG_KEY_X)      result |= DXGO_KEY_ACTION1;
    if (held & RG_KEY_Y)      result |= DXGO_KEY_ACTION2;
    if (held & RG_KEY_START)  result |= DXGO_KEY_START;
    if (held & RG_KEY_SELECT) result |= DXGO_KEY_SELECT;
    if (held & RG_KEY_MENU)   result |= DXGO_KEY_MENU;
    if (held & RG_KEY_L)      result |= DXGO_KEY_L;
    if (held & RG_KEY_R)      result |= DXGO_KEY_R;

    return result;
}

bool dxgo_platform_submit_audio_s16(const int16_t *interleaved_stereo,
                                    size_t frame_count)
{
    if (interleaved_stereo == NULL && frame_count != 0) {
        return false;
    }

    /* Small batches avoid a large temporary allocation on the ESP32-S3. */
    enum { AUDIO_BATCH_FRAMES = 128 };
    rg_audio_frame_t batch[AUDIO_BATCH_FRAMES];
    size_t offset = 0;

    while (offset < frame_count) {
        size_t count = frame_count - offset;
        if (count > AUDIO_BATCH_FRAMES) {
            count = AUDIO_BATCH_FRAMES;
        }
        for (size_t i = 0; i < count; ++i) {
            batch[i].left = interleaved_stereo[(offset + i) * 2];
            batch[i].right = interleaved_stereo[(offset + i) * 2 + 1];
        }
        rg_audio_submit(batch, count);
        offset += count;
    }
    return true;
}

#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "esp_random.h"

int chmod(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    return 0;
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    (void)path;
    (void)buf;
    (void)bufsiz;
    errno = EINVAL;
    return -1;
}

void __attribute__((weak)) esp_fill_random(void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        uint32_t r = esp_random();
        size_t chunk = (len < 4) ? len : 4;
        memcpy(p, &r, chunk);
        p += chunk;
        len -= chunk;
    }
}
