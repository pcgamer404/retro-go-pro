/*
 * mame-go: MAME 0.37b5 (mame2000-libretro) on retro-go.
 *
 * The core keeps its own libretro frontend (src/libretro/), which already
 * implements every osd_* function MAME needs. This file is the libretro
 * *host*: it answers the environment calls and turns the video, audio and
 * input callbacks into rg_display / rg_audio / rg_input.
 */
#include <rg_system.h>
#include <esp_partition.h>
#include <string.h>
#include <stdlib.h>

#include "libretro.h"

void hs_close(void); /* hiscore.c */

/* Same rate as the other apps: the PDM driver derives its DAC-mode clocks
 * from sample_rate / 100, and 22050 made mame-go far louder than the volume
 * setting allowed. */
#define AUDIO_SAMPLE_RATE 32000
#define SYSTEM_DIR RG_STORAGE_ROOT "/retro-go/mame"
#define SAVE_DIR RG_BASE_PATH_SAVES "/arcade"

/* retro-go-pro: rg_display_sync(void) does not wait, so wait for the display task here */
static void display_wait(void)
{
    while (rg_display_is_busy())
        rg_task_yield();
}

static rg_app_t *app;
static rg_surface_t *updates[2];
static int current;
static uint32_t joystick;
static retro_audio_buffer_status_callback_t audio_buffer_status;

/* ------------------------------------------------------------------ libretro callbacks */

static bool environment_cb(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = SYSTEM_DIR;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = SAVE_DIR;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        struct retro_variable *var = data;
        if (strcmp(var->key, "mame2000-sample_rate") == 0)
            var->value = "32000"; /* AUDIO_SAMPLE_RATE */
        else if (strcmp(var->key, "mame2000-frameskip") == 0)
            var->value = "auto"; /* driven by audio_buffer_status, see mame_task() */
        else if (strcmp(var->key, "mame2000-frameskip_interval") == 0)
            var->value = "2"; /* at most 2 skipped frames in a row */
        else
            return false;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
    {
        const struct retro_audio_buffer_status_callback *cb = data;
        audio_buffer_status = cb ? cb->callback : NULL;
        return true;
    }
    /* RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER is refused on purpose:
     * the core's blitter only redraws dirty blocks, so it needs one persistent
     * buffer. Handing it our two alternating surfaces made them diverge (flicker). */
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (!data) /* duplicate frame */
        return;
    static bool single;
    static unsigned failed_w, failed_h;   /* no room for a surface of this size */
    if ((!updates[0] && (width != failed_w || height != failed_h))
        || (updates[0] && (updates[0]->width != (int)width || updates[0]->height != (int)height)))
    {
        if (updates[1] != updates[0])
            rg_surface_free(updates[1]);
        rg_surface_free(updates[0]);
        updates[0] = updates[1] = NULL;
        for (int i = 0; i < 2; i++)
            /* internal RAM for the small 8-bit boards; a 384x224 CPS1 frame
               (172 KB, twice) does not fit there */
            updates[i] = rg_surface_create(width, height, RG_PIXEL_565_LE, width * height * 2 <= 128 * 1024 ? MEM_FAST : MEM_ANY);
        /* no room for the second one (CPS1 with 16-bit graphics): one surface,
           the display finishes with it before the next frame is copied in */
        single = !updates[1];
        failed_w = updates[0] ? 0 : width;
        failed_h = updates[0] ? 0 : height;
        if (single)
            updates[1] = updates[0];
    }
    if (!updates[0])
    {
        /* not even one (Street Fighter II: 2 MB of tiles in PSRAM): the
           display reads the core's own frame buffer, and is waited for
           before the core may draw the next frame into it */
        static rg_surface_t *direct;
        if (!direct && !(direct = rg_surface_create(0, 0, RG_PIXEL_565_LE, 0)))
            return;
        direct->width = width;
        direct->height = height;
        direct->stride = pitch;
        direct->data = (void *)data;
        rg_display_submit(direct, 0);
        display_wait();
        return;
    }
    if (single)
        display_wait();
    rg_surface_t *surface = updates[current];
    for (unsigned y = 0; y < height; y++)
        memcpy((uint8_t *)surface->data + y * surface->stride, (const uint8_t *)data + y * pitch, width * 2);
    rg_display_submit(surface, 0);
    current ^= 1;
}

/* Neo Geo frames converted on the second core (mame2000 libretro/video.c,
 * mamego_present_indexed): the core hands over its pen bitmap and the pen
 * -> RGB565 table instead of blitting + copying on core 0 (Metal Slug 2:
 * 3.6 ms per frame). The table is copied here, the bitmap is left alone by
 * MAME until the next call, which first waits for this conversion. */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
static struct
{
    const void *pix;
    int bits, width, height, pitch;
    uint16_t *pal;
    int pal_size;
    void (*render)(void);
} present;
static TaskHandle_t present_task;
static SemaphoreHandle_t present_done;
static volatile bool present_busy;

static void present_task_main(void *arg)
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#ifdef NEOPROF
        extern volatile int64_t mamego_core1_us[3];
        int64_t t0 = esp_timer_get_time();
#endif
        if (present.render) /* Neo Geo sprites of this frame (vidhrdw/neogeo.c) */
        {
            present.render();
#ifdef NEOPROF
            extern volatile int mamego_render_count[3];
            mamego_render_count[1]++;
#endif
        }
#ifdef NEOPROF
        int64_t t1 = esp_timer_get_time();
        mamego_core1_us[0] += t1 - t0;
#endif
        rg_surface_t *surface = updates[current];
        const uint16_t *pal = present.pal;
        for (int y = 0; y < present.height; y++)
        {
            uint16_t *dst = (uint16_t *)((uint8_t *)surface->data + y * surface->stride);
            if (present.bits == 16)
            {
                const uint16_t *src = (const uint16_t *)present.pix + y * present.pitch;
                for (int x = 0; x < present.width; x++)
                    dst[x] = pal[src[x]];
            }
            else
            {
                const uint8_t *src = (const uint8_t *)present.pix + y * present.pitch;
                for (int x = 0; x < present.width; x++)
                    dst[x] = pal[src[x]];
            }
        }
        rg_display_submit(surface, 0);
#ifdef NEOPROF
        mamego_core1_us[1] += esp_timer_get_time() - t1;
#endif
        current ^= 1;
        present_busy = false;
        xSemaphoreGive(present_done);
    }
}

#ifdef NEOPROF
/* core 1 idle time: the idle hook runs back to back while core 1 has nothing
   to do; gaps longer than 50 us mean a task ran in between */
#include <esp_freertos_hooks.h>
static volatile int64_t core1_idle_us, core1_idle_last;
static bool core1_idle_hook(void)
{
    int64_t t = esp_timer_get_time();
    if (core1_idle_last && t - core1_idle_last < 50)
        core1_idle_us += t - core1_idle_last;
    core1_idle_last = t;
    return false; /* keep spinning, so the next call comes right away */
}
int64_t mamego_core1_idle_us(void)
{
    static bool registered;
    if (!registered)
        registered = esp_register_freertos_idle_hook_for_cpu(core1_idle_hook, 1) == ESP_OK;
    return core1_idle_us;
}
int64_t mamego_display_busy_us(void)
{
    return rg_display_get_counters().busyTime;
}
#endif

static int present_indexed(const void *pix, int bits, int width, int height, int pitch, const void *palette, int colors)
{
    static bool no_memory;
    if (present_busy)
        xSemaphoreTake(present_done, portMAX_DELAY);
    if (!pix) /* the core only waits for the previous frame */
        return 1;
    if (no_memory) /* declined once for memory: the core's own path from now on */
        return 0;
    if (!present_task)
    {
        present_done = xSemaphoreCreateBinary();
        if (xTaskCreatePinnedToCore(present_task_main, "mame_present", 6144, NULL, 5, &present_task, 1) != pdPASS)
            return 0;
    }
    if (colors > present.pal_size)
    {
        free(present.pal);
        present.pal = malloc(colors * sizeof(uint16_t));
        present.pal_size = present.pal ? colors : 0;
        if (!present.pal)
            return 0;
    }
    if (!updates[0] || updates[0]->width != width || updates[0]->height != height)
    {
        /* PSRAM: with the YM2610 and present tasks on core 1 there is no
           room for 2 x 136 KB of internal RAM (the core 1 task writes it) */
        if (updates[1] != updates[0])
            rg_surface_free(updates[1]);
        rg_surface_free(updates[0]);
        updates[0] = updates[1] = NULL;
        for (int i = 0; i < 2; i++)
            updates[i] = rg_surface_create(width, height, RG_PIXEL_565_LE, MEM_SLOW);
        if (!updates[0] || !updates[1])
        {
            /* no room for two frames (SF2): stay on the core's path */
            rg_surface_free(updates[0]);
            rg_surface_free(updates[1]);
            updates[0] = updates[1] = NULL;
            no_memory = true;
            return 0;
        }
    }
    if (bits == 16)
        for (int i = 0; i < colors; i++)
            present.pal[i] = (uint16_t)((const uint32_t *)palette)[i];
    else
        memcpy(present.pal, palette, colors * sizeof(uint16_t));
    {
        extern void (*mamego_frame_render)(void);
        present.render = mamego_frame_render; /* drawn on this core before converting */
        mamego_frame_render = NULL;
    }
    present.pix = pix;
    present.bits = bits;
    present.width = width;
    present.height = height;
    present.pitch = pitch;
    present_busy = true;
    xTaskNotifyGive(present_task);
    return 1;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    rg_audio_submit((const rg_audio_frame_t *)data, frames);
    return frames;
}

static void audio_cb(int16_t left, int16_t right)
{
    rg_audio_frame_t frame = {left, right};
    rg_audio_submit(&frame, 1);
}

static void input_poll_cb(void)
{
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;

    static const struct { uint32_t rg; unsigned retro; } map[] = {
        {RG_KEY_UP, RETRO_DEVICE_ID_JOYPAD_UP},       {RG_KEY_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN},
        {RG_KEY_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT},   {RG_KEY_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT},
        {RG_KEY_A, RETRO_DEVICE_ID_JOYPAD_A},         {RG_KEY_B, RETRO_DEVICE_ID_JOYPAD_B},
        {RG_KEY_X, RETRO_DEVICE_ID_JOYPAD_X},         {RG_KEY_Y, RETRO_DEVICE_ID_JOYPAD_Y},
        {RG_KEY_L, RETRO_DEVICE_ID_JOYPAD_L},         {RG_KEY_R, RETRO_DEVICE_ID_JOYPAD_R},
        {RG_KEY_START, RETRO_DEVICE_ID_JOYPAD_START}, {RG_KEY_SELECT, RETRO_DEVICE_ID_JOYPAD_SELECT}, /* coin */
    };

    int16_t bits = 0;
    for (size_t i = 0; i < RG_COUNT(map); i++)
        if (joystick & map[i].rg)
            bits |= 1 << map[i].retro;

    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return bits;
    return (bits >> id) & 1;
}

/* ------------------------------------------------------------------ retro-go handlers */

static bool screenshot_handler(const char *filename, int width, int height)
{
    const rg_surface_t *s = updates[current ^ 1];
    if (!s)
        return false;
    size_t n = strlen(filename);
    if (n > 4 && !strcmp(filename + n - 4, ".raw"))
    {
        /* bench (console `shot`): the frame as is, RGB565 with a width/height
           header, no PNG encoder (it needs memory the big games do not leave) */
        FILE *fp = fopen(filename, "wb");
        if (!fp)
            return false;
        uint16_t wh[2] = {(uint16_t)s->width, (uint16_t)s->height};
        fwrite(wh, 2, 2, fp);
        for (int y = 0; y < s->height; y++)
            fwrite((const uint8_t *)s->data + s->offset + y * s->stride, 2, s->width, fp);
        fclose(fp);
        return true;
    }
    return rg_surface_save_image_file(s, filename, width, height);
}

/* Big read-only ROM regions (gfx, sound samples) live in the "mamerom" flash
 * partition, memory-mapped, instead of PSRAM (common.c mamego_regions_to_flash).
 * A region already there from the last run of the game is only mapped; the
 * flash is rewritten only when its content differs. */
unsigned char *mamego_flash_store(const unsigned char *data, size_t len, size_t *offset)
{
    static const esp_partition_t *part;
    static const uint8_t *map;
    static esp_partition_mmap_handle_t handle;
    size_t off = *offset, size = (len + 0xFFF) & ~(size_t)0xFFF; /* erase sectors */

    if (!part)
    {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "mamerom");
        if (!part || esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, (const void **)&map, &handle) != ESP_OK)
        {
            RG_LOGW("mamerom partition unavailable, ROMs stay in PSRAM");
            part = NULL;
            return NULL;
        }
    }
    if (off + size > part->size)
        return NULL;
    if (memcmp(map + off, data, len) != 0)
    {
        RG_LOGI("mamerom: writing %u bytes at 0x%x", (unsigned)len, (unsigned)off);
        if (esp_partition_erase_range(part, off, size) != ESP_OK || esp_partition_write(part, off, data, len) != ESP_OK ||
            memcmp(map + off, data, len) != 0)
        {
            RG_LOGE("mamerom: write failed, region stays in PSRAM");
            return NULL;
        }
    }
    *offset = off + size;
    return (unsigned char *)map + off;
}

/* State buffer: the heap, or on a Neo Geo game (PSRAM nearly all given to
   the sprite page cache) the page cache itself, borrowed between frames. */
static int present_indexed(const void *pix, int bits, int width, int height, int pitch, const void *palette, int colors);

static void *state_buffer(size_t size, bool *borrowed)
{
    extern void *neospr_borrow(size_t size);
    if (present_task)
        present_indexed(NULL, 0, 0, 0, 0, NULL, 0); /* core 1 done with the frame and the page cache */
    void *buf = size ? malloc(size) : NULL;
    *borrowed = false;
    if (!buf && size && (buf = neospr_borrow(size)))
        *borrowed = true;
    return buf;
}

size_t mamego_psram_free(size_t *largest)
{
    *largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

void mamego_mem_report(const char *where)
{
    printf("mem %s: internal %u KB (largest %u), PSRAM %u KB (largest %u)\n", where,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
}

static bool save_state_handler(const char *filename)
{
    size_t size = retro_serialize_size();
    bool borrowed;
    void *buf = state_buffer(size, &borrowed);
    bool ok = buf && retro_serialize(buf, size);
    if (ok)
    {
        FILE *fp = fopen(filename, "wb");
        ok = fp && fwrite(buf, 1, size, fp) == size;
        if (fp)
            fclose(fp);
    }
    if (!borrowed)
        free(buf);
    return ok;
}

static bool load_state_handler(const char *filename)
{
    size_t size = retro_serialize_size();
    bool borrowed;
    void *buf = state_buffer(size, &borrowed);
    FILE *fp = fopen(filename, "rb");
    bool ok = buf && fp && fread(buf, 1, size, fp) == size && fgetc(fp) == EOF && retro_unserialize(buf, size);
    if (fp)
        fclose(fp);
    if (!borrowed)
        free(buf);
    return ok;
}

static bool reset_handler(bool hard)
{
    retro_reset();
    return true;
}

static void event_handler(int event, void *arg)
{
    /* Records (.hi) are written when the machine stops, which never happens
     * on retro-go: the app is simply rebooted into the launcher. */
    if (event == RG_EVENT_SHUTDOWN)
        hs_close();
    if (event == RG_EVENT_REDRAW && updates[current ^ 1])
        rg_display_submit(updates[current ^ 1], 0);
}

/* MAME needs far more than the 8 KB main task stack (ROM loading, drivers),
 * so the core runs on its own task, like duke3d-go. */
static void mame_task(void *arg)
{
    retro_set_environment(environment_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    struct retro_game_info game = {app->romPath, NULL, 0, NULL};
    {
        extern int (*mamego_present_indexed)(const void *, int, int, int, int, const void *, int);
        mamego_present_indexed = present_indexed;
    }
    if (!retro_load_game(&game))
        RG_PANIC("This game is not supported, or its ROM set is incomplete");

    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    RG_LOGI("mame-go: %ux%u @ %.2f fps, audio %.0f Hz", av.geometry.base_width, av.geometry.base_height,
            av.timing.fps, av.timing.sample_rate);
    rg_system_set_tick_rate(av.timing.fps);

    /* Default to 1:1: these boards are at most 224-256 lines and a non-integer
     * upscale (Pac-Man 288 -> 320 lines) doubles every ninth line of the maze.
     * "DispScaling" is rg_display.c's per-app key; absent = never chosen. */
    bool scaling_never_chosen = rg_settings_get_number(NS_APP, "DispScaling", -1) == -1;
    /* A 2026-09-28 bench build saved "filter off" for mame-go (it looked bad
     * at the Neo Geo's 1.43x): put the default back once. */
    if (!rg_settings_get_number(NS_APP, "DispFilterFix", 0))
    {
        rg_display_set_filter(RG_DISPLAY_FILTER_BOTH);
        rg_settings_set_number(NS_APP, "DispFilterFix", 1);
    }

    if (scaling_never_chosen)
        rg_display_set_scaling(RG_DISPLAY_SCALING_OFF);
    if (rg_display_get_scaling() == RG_DISPLAY_SCALING_OFF &&
        (av.geometry.base_width > rg_display_get_width() || av.geometry.base_height > rg_display_get_height()))
        rg_display_set_scaling(RG_DISPLAY_SCALING_FIT); /* would be cropped */
    /* Neo Geo (304x224, /roms/neogeo/): nearly full screen, the whole height
     * with the aspect kept (434x320 on this panel). The menu can still pick
     * another mode for the session. */
    if (app->romPath && strstr(app->romPath, "/neogeo/"))
        rg_display_set_scaling(RG_DISPLAY_SCALING_FIT);
    /* Wide 16-bit boards (CPS1: 384x224) fit 1:1 but leave a third of the
     * panel empty: the whole width with the aspect kept (480x280). Only when
     * the user never chose a mode for mame-go. */
    else if (av.geometry.base_width >= 384 && scaling_never_chosen)
        rg_display_set_scaling(RG_DISPLAY_SCALING_FIT);

    if (app->bootFlags & RG_BOOT_RESUME)
    {
        retro_run(); /* the machine is fully up only after its first frame */
        rg_emu_load_state(app->saveSlot);
    }

    /* Auto frameskip: when the game keeps up, audio_batch_cb blocks on the
     * I2S DMA and the loops average one frame period (individual loops
     * jitter, the DMA drains in chunks). The accumulated lateness says when
     * the emulation is really behind -- a whole frame -- and the core then
     * skips drawing (not emulating) the next frame. */
    const int64_t frame_us = 1000000 / av.timing.fps;
    int64_t loop_start = rg_system_timer(), late = 0;

    while (true)
    {
        int64_t now = rg_system_timer();
        late += (now - loop_start) - frame_us;
        late = RG_MAX(late, -2 * frame_us); /* ahead time cannot be banked */
        late = RG_MIN(late, 8 * frame_us);  /* forget long stalls (menus) */
        loop_start = now;
        if (audio_buffer_status)
            audio_buffer_status(true, 50, late > frame_us);

        joystick = rg_input_read_gamepad();
        if (joystick & RG_KEY_MENU)
            rg_gui_game_menu();
        else if (joystick & RG_KEY_OPTION)
            rg_gui_options_menu();

        int64_t start = rg_system_timer();
        retro_run(); /* audio_batch_cb blocks on the I2S DMA, which paces the loop */
        rg_system_tick(rg_system_timer() - start);
    }
}

void app_main(void)
{
    const rg_config_t config = {
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameRate = 60, /* adjusted from the driver's fps in mame_task() */
        .storageRequired = true,
        .romRequired = true,
        .handlers.loadState = &load_state_handler,
        .handlers.saveState = &save_state_handler,
        .handlers.reset = &reset_handler,
        .handlers.screenshot = &screenshot_handler,
        .handlers.event = &event_handler,
    };

    app = rg_system_init(&config);
    rg_storage_mkdir(SYSTEM_DIR);
    rg_storage_mkdir(SAVE_DIR);

    rg_task_create("mame", &mame_task, NULL, 32 * 1024, RG_TASK_PRIORITY_5, 0);
    while (1)
        rg_task_delay(1000);
}
