#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <rg_system.h>
#include <rg_display.h>
#include <rg_audio.h>
#include <rg_input.h>
#include <rg_gui.h>
#include <rg_settings.h>
#include <rg_surface.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>
#include "rg_glue.h"

#if RG_SCREEN_PIXEL_FORMAT == 0
#define FB_PIXEL_FORMAT RG_PIXEL_PAL565_BE
#else
#define FB_PIXEL_FORMAT RG_PIXEL_PAL565_LE
#endif

// ---------------------------------------------------------------- video
static rg_surface_t *surface = NULL;
static uint16_t cached_palette[256];
static bool palette_dirty = true;
static int surf_w = 0, surf_h = 0;

void rgx_video_init(int width, int height)
{
    if (surface && surf_w == width && surf_h == height)
        return;
    if (surface)
    {
        while (rg_display_is_busy())
            rg_task_yield();
        rg_surface_free(surface);
        surface = NULL;
    }
    // Display expansion reads this sequentially, so PSRAM is fine
    surface = rg_surface_create(width, height, FB_PIXEL_FORMAT, MEM_SLOW);
    if (!surface)
        RG_PANIC("Failed to allocate ScummVM surface!");
    memset(surface->data, 0, (size_t)width * height);
    surf_w = width;
    surf_h = height;
    palette_dirty = true;
}

uint8_t *rgx_video_pixels(void)
{
    return surface ? (uint8_t *)surface->data : NULL;
}

void *rgx_get_surface(void)
{
    return surface;
}

void rgx_video_set_palette(const uint8_t *rgb, int start, int count)
{
    for (int i = start; i < start + count && i < 256; i++, rgb += 3)
    {
        uint16_t v = ((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3);
#if RG_SCREEN_PIXEL_FORMAT == 0
        cached_palette[i] = (v >> 8) | (v << 8);
#else
        cached_palette[i] = v;
#endif
    }
    palette_dirty = true;
}

void rgx_video_present(void)
{
    if (!surface)
        return;
    if (palette_dirty)
    {
        memcpy(surface->palette, cached_palette, sizeof(cached_palette));
        palette_dirty = false;
    }
    rg_display_submit(surface, 0);
    while (rg_display_is_busy())
        rg_task_yield();
}


// ---------------------------------------------------------------- progress screen
// Shown while a ZIP is unpacked for the first time. Draws a bar plus "percent" and
// "done / total MB" with a tiny built-in digit font, so it does not depend on any
// retro-go drawing API and the handheld visibly keeps working.
static const uint8_t glyph35[11][5] = { // 3x5 digits 0-9, then '/'
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7},{1,1,2,4,4}};

static void prog_rect(uint8_t *px, int w, int h, int x, int y, int rw, int rh, uint8_t c)
{
    for (int j = y; j < y + rh && j < h; j++)
        if (j >= 0)
            for (int i = x; i < x + rw && i < w; i++)
                if (i >= 0)
                    px[j * w + i] = c;
}

static int prog_text(uint8_t *px, int w, int h, int x, int y, int scale, const char *t)
{
    for (; *t; t++, x += 4 * scale)
    {
        int g = (*t >= '0' && *t <= '9') ? *t - '0' : (*t == '/' ? 10 : -1);
        if (g < 0)
            continue;
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (glyph35[g][r] & (4 >> c))
                    prog_rect(px, w, h, x + c * scale, y + r * scale, scale, scale, 1);
    }
    return x;
}

void rgx_progress(uint32_t done_kb, uint32_t total_kb, int stage)
{
    static int64_t last_us = 0;
    int64_t now = rg_system_timer();
    if (stage == 1 && last_us && now - last_us < 150000) // keep file writing fast
        return;
    last_us = now;

    rgx_video_init(320, 200);
    uint8_t *px = rgx_video_pixels();
    if (!px)
        return;
    static const uint8_t pal[4 * 3] = {0, 0, 0, 255, 255, 255, 60, 60, 90, 80, 220, 120};
    rgx_video_set_palette(pal, 0, 4);
    memset(px, 0, 320 * 200);

    const int bx = 30, by = 120, bw = 260, bh = 18;
    prog_rect(px, 320, 200, bx - 2, by - 2, bw + 4, bh + 4, 1);
    prog_rect(px, 320, 200, bx, by, bw, bh, 2);
    uint32_t pct = 0;
    if (total_kb)
    {
        pct = (uint32_t)(((uint64_t)done_kb * 100) / total_kb);
        if (pct > 100) pct = 100;
        prog_rect(px, 320, 200, bx, by, (int)((uint64_t)bw * pct / 100), bh, 3);
    }
    else // scanning: moving block so the screen is visibly alive
    {
        int pos = (int)((now / 8000) % (bw - 40));
        prog_rect(px, 320, 200, bx + pos, by, 40, bh, 3);
    }
    char t[48];
    snprintf(t, sizeof(t), "%u", (unsigned)pct);
    prog_text(px, 320, 200, 130, 50, 6, t);
    snprintf(t, sizeof(t), "%u/%u", (unsigned)(done_kb / 1024), (unsigned)(total_kb / 1024));
    prog_text(px, 320, 200, 100, 150, 3, t);
    rgx_video_present();
    rg_system_tick(0);
}

// ---------------------------------------------------------------- audio
static rgx_audio_cb audio_cb = NULL;
static void *audio_ud = NULL;
static volatile bool audio_running = false;
static volatile bool audio_done = true;
static volatile bool audio_paused = true;
static volatile bool audio_in_submit = false;

#define AUDIO_FRAMES 512
static rg_audio_frame_t audio_buf[AUDIO_FRAMES];

static void audio_task(void *arg)
{
    while (audio_running)
    {
        if (audio_paused || rg_audio_get_mute() || !audio_cb)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        memset(audio_buf, 0, sizeof(audio_buf));
        audio_cb((int16_t *)audio_buf, AUDIO_FRAMES, audio_ud);
        if (audio_running && !audio_paused && !rg_audio_get_mute())
        {
            audio_in_submit = true;
            rg_audio_submit(audio_buf, AUDIO_FRAMES);
            audio_in_submit = false;
        }
    }
    audio_done = true;
}

void rgx_audio_start(rgx_audio_cb cb, void *userdata)
{
    if (audio_running)
        return;
    audio_cb = cb;
    audio_ud = userdata;
    audio_running = true;
    audio_done = false;
    audio_paused = false;
    rg_audio_set_mute(false);
    rg_task_create("scumm_audio", audio_task, NULL, 16 * 1024, 1, RG_TASK_PRIORITY_2, 1);
}

void rgx_audio_pause(bool pause)
{
    audio_paused = pause;
    if (pause)
        while (audio_in_submit)
            vTaskDelay(pdMS_TO_TICKS(5));
}

void rgx_audio_stop(void)
{
    rg_audio_set_mute(true);
    if (!audio_running)
        return;
    audio_running = false;
    audio_paused = true;
    for (int i = 0; i < 50 && !audio_done; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
}

bool rgx_sound_enabled(void)
{
    return rg_settings_get_number(NS_APP, "sound", 1) != 0;
}

// ---------------------------------------------------------------- input / menus
uint32_t rgx_buttons(void)
{
    uint32_t k = rg_input_read_gamepad(), r = 0;
    if (k & RG_KEY_UP)     r |= RGX_UP;
    if (k & RG_KEY_DOWN)   r |= RGX_DOWN;
    if (k & RG_KEY_LEFT)   r |= RGX_LEFT;
    if (k & RG_KEY_RIGHT)  r |= RGX_RIGHT;
    if (k & RG_KEY_A)      r |= RGX_A;
    if (k & RG_KEY_B)      r |= RGX_B;
    if (k & RG_KEY_X)      r |= RGX_X;
    if (k & RG_KEY_Y)      r |= RGX_Y;
    if (k & RG_KEY_START)  r |= RGX_START;
    if (k & RG_KEY_SELECT) r |= RGX_SELECT;
    if (k & RG_KEY_L)      r |= RGX_L;
    if (k & RG_KEY_R)      r |= RGX_R;
    if (k & RG_KEY_MENU)   r |= RGX_MENU;
    if (k & RG_KEY_OPTION) r |= RGX_OPTION;
    return r;
}

void rgx_menu(bool options)
{
    rgx_audio_pause(true);
    if (options)
        rg_gui_options_menu();
    else
        rg_gui_game_menu();
    rg_input_wait_for_key(RG_KEY_MENU | RG_KEY_OPTION, false, 500);
    rgx_audio_pause(false);
}

int rgx_cursor_speed(void)
{
    return (int)rg_settings_get_number(NS_APP, "cursor_speed", 1);
}

static volatile int quicksave_request = 0;
void rgx_request_quicksave(int what) { quicksave_request = what; }
int rgx_take_quicksave_request(void)
{
    int r = quicksave_request;
    quicksave_request = 0;
    return r;
}

// ---------------------------------------------------------------- time / sync / lifecycle
int64_t rgx_time_us(void) { return rg_system_timer(); }
void rgx_sleep_us(uint64_t us) { rg_usleep(us); }
void rgx_tick(int64_t busy_us) { rg_system_tick(busy_us); }

void *rgx_mutex_create(void) { return xSemaphoreCreateRecursiveMutex(); }
void rgx_mutex_lock(void *m) { if (m) xSemaphoreTakeRecursive((SemaphoreHandle_t)m, portMAX_DELAY); }
void rgx_mutex_unlock(void *m) { if (m) xSemaphoreGiveRecursive((SemaphoreHandle_t)m); }
void rgx_mutex_delete(void *m) { if (m) vSemaphoreDelete((SemaphoreHandle_t)m); }

void rgx_exit(void)
{
    rgx_audio_stop();
    rg_system_exit();
}

// ---------------------------------------------------------------- game location
bool rgx_game_location(char *dir, int dir_size, char *gameid, int gameid_size)
{
    rg_app_t *app = rg_system_get_app();
    if (!app || !app->romPath || !app->romPath[0])
        return false;

    gameid[0] = 0;
    snprintf(dir, dir_size, "%s", app->romPath);

    const char *ext = strrchr(app->romPath, '.');
    bool is_stub = ext && strcasecmp(ext, ".scummvm") == 0;
    if (is_stub)
    {
        // Optional: first line of the .scummvm stub is a game id (e.g. "monkey")
        FILE *fp = fopen(app->romPath, "r");
        if (fp)
        {
            if (fgets(gameid, gameid_size, fp))
            {
                char *nl = strpbrk(gameid, "\r\n");
                if (nl) *nl = 0;
            }
            fclose(fp);
        }
    }

    // Keep archive files (.zip/.svm/.scummvm) intact so scummvm_run() can
    // unpack them. Normal files/stubs still resolve to their containing folder.
    struct stat st;
    bool is_dir = (stat(dir, &st) == 0 && S_ISDIR(st.st_mode));
    const char *p = strrchr(dir, '.');
    bool is_archive = p && (strcasecmp(p, ".zip") == 0 ||
                            strcasecmp(p, ".svm") == 0);
    if (!is_dir && !is_archive)
    {
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir)
            *slash = 0;
    }
    return true;
}
