#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config.h"
#include "rg_audio.h"
#include "rg_display.h"
#include "rg_gui.h"
#include "rg_input.h"
#include "rg_storage.h"
#include "rg_surface.h"
#include "rg_system.h"
#include "rg_utils.h"

#include "client/client.h"
#include "client/keys.h"
#include "client/snd_loc.h"
#include "quake2.h"
#include "quakegeneric.h"

// Model/skin image built by tools/mkassets.py and linked into flash rodata
// (main/assets.S.in), so the engine uses the models in place instead of
// loading them into the PSRAM arena.
extern const uint8_t q2_assets_start[];
extern const uint8_t q2_assets_end[];
extern int Q_Assets_Init(const void *base, int size);

#define Q2_WIDTH 320
#define Q2_HEIGHT 240
#define Q2_AUDIO_RATE 22050
#define Q2_DMA_FRAMES 4096
#define Q2_TASK_STACK_BYTES (64 * 1024)
// The engine paints up to Q2_DMA_FRAMES (186 ms) ahead of the playback cursor,
// so the app-side ring must hold at least that much plus some slack.
#define Q2_AUDIO_RING_FRAMES (Q2_AUDIO_RATE / 2)
// One submit = one i2s DMA block (drivers/audio/i2s.c: DMA_BUFFER_LEN 180)
#define Q2_AUDIO_SUBMIT_CHUNK 180

static rg_app_t *app;
static rg_surface_t q2_surface;
static uint16_t q2_palette[256];
static uint8_t palette[256 * 3];
static volatile bool engine_running;
static volatile bool engine_done;
static volatile TaskHandle_t engine_task_handle;
static StaticTask_t engine_task_tcb;
static StackType_t *engine_task_stack;
static rg_audio_frame_t *audio_buffer;
static int audio_submitted_frame;
// The engine mixes into audio_buffer and SNDDMA_Submit hands the frames to this
// ring; a low priority task feeds them to I2S. Feeding inside the engine task
// made i2s_write block for up to 33 ms per frame (its DMA ring) because one
// frame of game audio (66 ms at 15 fps) is far longer than the DMA ring.
static rg_audio_frame_t *audio_ring;
static uint32_t audio_ring_write;
static uint32_t audio_ring_read;
static SemaphoreHandle_t audio_ring_lock;
static int64_t last_frame_time;
static uint32_t previous_buttons;
static int64_t menu_pressed_at;
static bool menu_was_down;
static bool menu_opened;

static void shutdown_video(void);

static SemaphoreHandle_t worker_start;
static SemaphoreHandle_t worker_finish;
static void (*parallel_fn)(void *, int);
static void *parallel_arg;

static void renderer_worker(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(worker_start, portMAX_DELAY);
        parallel_fn(parallel_arg, 1);
        xSemaphoreGive(worker_finish);
    }
}

void QG_Parallel2(void (*fn)(void *arg, int part), void *arg)
{
    parallel_fn = fn;
    parallel_arg = arg;
    xSemaphoreGive(worker_start);
    fn(arg, 0);
    xSemaphoreTake(worker_finish, portMAX_DELAY);
}

int QG_Milliseconds(void)
{
    return (int)(rg_system_timer() / 1000);
}

int QG_Microseconds(void)
{
    return (int)rg_system_timer();
}

int QG_IsGameTask(void)
{
    return xTaskGetCurrentTaskHandle() == engine_task_handle;
}

void QG_GetMouseMove(int *dx, int *dy)
{
    *dx = 0;
    *dy = 0;
}

int QG_GetKey(int *down, int *key)
{
    (void)down;
    (void)key;
    return 0;
}

void QG_SetPalette(unsigned char colors[768])
{
    memcpy(palette, colors, sizeof(palette));
    for (int i = 0; i < 256; i++) {
        uint8_t r = palette[i * 3];
        uint8_t g = palette[i * 3 + 1];
        uint8_t b = palette[i * 3 + 2];
        q2_palette[i] = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
    }
}

void QG_DrawFrame(void *pixels)
{
    if (!pixels)
        return;
    q2_surface.data = pixels;
    rg_display_submit(&q2_surface, 0);
}

void *QG_ScreenBuffer(int size)
{
    (void)size;
    return NULL;
}

void QG_Quit(void)
{
    engine_running = false;
}

void QG_FatalError(const char *message)
{
    engine_running = false;
    rg_system_log(RG_LOG_ERROR, "Quake II", "%s", message);
    rg_gui_alert("Quake II error", message);
    shutdown_video();
    extern void FS_ClosePakCache(void);
    FS_ClosePakCache();
    engine_task_handle = NULL;
    engine_done = true;
    vTaskDelete(NULL);
}

static void key_event(uint32_t bit, int key, uint32_t buttons)
{
    bool was_down = (previous_buttons & bit) != 0;
    bool is_down = (buttons & bit) != 0;
    if (was_down != is_down)
        Quake2_SendKey(key, is_down);
}

static void poll_input(void)
{
    uint32_t buttons = rg_input_read_gamepad();
    bool menu_down = (buttons & RG_KEY_MENU) != 0;
    int64_t now = rg_system_timer();
    if (menu_down && !menu_was_down) {
        menu_pressed_at = now;
        menu_opened = false;
    }
    if (menu_down && !menu_opened && now - menu_pressed_at >= 500000) {
        key_event(RG_KEY_UP, K_UPARROW, 0);
        key_event(RG_KEY_DOWN, K_DOWNARROW, 0);
        key_event(RG_KEY_LEFT, K_LEFTARROW, 0);
        key_event(RG_KEY_RIGHT, K_RIGHTARROW, 0);
        key_event(RG_KEY_A, K_MOUSE1, 0);
        key_event(RG_KEY_B, K_SPACE, 0);
        key_event(RG_KEY_X, 'c', 0);
        key_event(RG_KEY_SELECT, K_TAB, 0);
        key_event(RG_KEY_START, K_ESCAPE, 0);
        previous_buttons = buttons;
        menu_opened = true;
        rg_gui_game_menu();
        previous_buttons = rg_input_read_gamepad();
        return;
    }
    if (!menu_down && menu_was_down && !menu_opened) {
        Quake2_SendKey(K_ESCAPE, 1);
        Quake2_SendKey(K_ESCAPE, 0);
    }
    menu_was_down = menu_down;

    uint32_t effective_buttons = buttons & ~RG_KEY_MENU;
    int fire_key = cls.key_dest == key_game ? K_MOUSE1 : K_ENTER;
    int jump_key = cls.key_dest == key_game ? K_SPACE : K_ESCAPE;

    // Y is the look modifier: Y + Up/Down pitches the view, since the pad has
    // no second stick. While it is held the d-pad no longer walks.
    static bool look_up_sent, look_down_sent;
    bool looking = (effective_buttons & RG_KEY_Y) && cls.key_dest == key_game;
    bool want_up = looking && (effective_buttons & RG_KEY_UP);
    bool want_down = looking && (effective_buttons & RG_KEY_DOWN);
    if (want_up != look_up_sent) {
        Quake2_SendKey('a', want_up);   // default.cfg: bind a +lookup
        look_up_sent = want_up;
    }
    if (want_down != look_down_sent) {
        Quake2_SendKey('z', want_down); // default.cfg: bind z +lookdown
        look_down_sent = want_down;
    }
    uint32_t walk_buttons = looking ? (effective_buttons & ~(RG_KEY_UP | RG_KEY_DOWN))
                                    : effective_buttons;

    key_event(RG_KEY_UP, K_UPARROW, walk_buttons);
    key_event(RG_KEY_DOWN, K_DOWNARROW, walk_buttons);
    key_event(RG_KEY_LEFT, K_LEFTARROW, effective_buttons);
    key_event(RG_KEY_RIGHT, K_RIGHTARROW, effective_buttons);
    key_event(RG_KEY_A, fire_key, effective_buttons);
    key_event(RG_KEY_B, jump_key, effective_buttons);
    key_event(RG_KEY_X, 'c', effective_buttons);
    key_event(RG_KEY_SELECT, K_TAB, effective_buttons);
    key_event(RG_KEY_START, K_ESCAPE, effective_buttons);
    previous_buttons = effective_buttons;
}

static bool init_video(void)
{
    memset(&q2_surface, 0, sizeof(q2_surface));
    q2_surface.width = Q2_WIDTH;
    q2_surface.height = Q2_HEIGHT;
    q2_surface.stride = Q2_WIDTH;
    q2_surface.format = RG_PIXEL_PAL565_LE;
    q2_surface.palette = q2_palette;
    QG_SetPalette(palette);
    return true;
}

static void shutdown_video(void)
{
    while (rg_display_is_busy())
        rg_task_yield();
    q2_surface.data = NULL;
}

static bool setup_game_paths(const char *pak_path, char *rom_root, size_t rom_root_size)
{
    if (!pak_path || !rg_extension_match(pak_path, "pak"))
        return false;
    const char *slash = strrchr(pak_path, '/');
    if (!slash || slash == pak_path)
        return false;
    size_t len = (size_t)(slash - pak_path);
    if (len >= rom_root_size)
        return false;
    memcpy(rom_root, pak_path, len);
    rom_root[len] = '\0';

    // Quake II resolves PAKs under <cddir>/baseq2. Retro-Go's picker starts
    // at quake2/, so normalize a selected baseq2/pak*.pak to its parent.
    char *leaf = strrchr(rom_root, '/');
    if (leaf && !strcasecmp(leaf + 1, "baseq2"))
        *leaf = '\0';
    return true;
}

static void engine_task(void *arg)
{
    (void)arg;
    char rom_root[RG_PATH_MAX + 1];
    char save_root[RG_PATH_MAX + 1];
    if (!setup_game_paths(app->romPath, rom_root, sizeof(rom_root))) {
        rg_gui_alert("Quake II data", "Select a Quake II PAK file in baseq2.");
        engine_done = true;
        vTaskDelete(NULL);
        return;
    }

    snprintf(save_root, sizeof(save_root), "%s/quake2", RG_BASE_PATH_SAVES);
    rg_storage_mkdir(RG_BASE_PATH_SAVES);
    rg_storage_mkdir(save_root);
    char save_game_root[RG_PATH_MAX + 8];
    snprintf(save_game_root, sizeof(save_game_root), "%s/baseq2", save_root);
    rg_storage_mkdir(save_game_root);

    if (!init_video()) {
        rg_gui_alert("Quake II video", "Could not allocate the 320x240 display buffers.");
        engine_done = true;
        vTaskDelete(NULL);
        return;
    }

    char *argv[] = {"quake2-go", "+set", "basedir", save_root,
                    "+set", "cddir", rom_root};
    engine_running = true;
    last_frame_time = rg_system_timer();
    extern void FS_InitPakCache(void);
    FS_InitPakCache();

    int asset_size = (int)(q2_assets_end - q2_assets_start);
    int asset_count = Q_Assets_Init(q2_assets_start, asset_size);
    rg_system_log(asset_count ? RG_LOG_INFO : RG_LOG_WARN, "Quake II",
                  "flash assets: %d files, %d bytes", asset_count, asset_size);

    Quake2_Init((int)(sizeof(argv) / sizeof(argv[0])), argv);
    while (engine_running) {
        int64_t frame_start = rg_system_timer();
        int64_t now = rg_system_timer();
        int msec = (int)((now - last_frame_time) / 1000);
        if (msec < 1)
            msec = 1;
        if (msec > 200)
            msec = 200;
        last_frame_time = now;
        poll_input();
        while (rg_display_is_busy())
            rg_task_yield();
        Quake2_Frame(msec);
        int64_t elapsed = rg_system_timer() - frame_start;
        rg_system_tick((int)elapsed);
        int64_t remaining = app->frameTime - elapsed;
        if (remaining > 0)
            rg_task_delay((uint32_t)((remaining + 999) / 1000));
    }
    shutdown_video();
    extern void FS_ClosePakCache(void);
    FS_ClosePakCache();
    engine_done = true;
    engine_task_handle = NULL;
    vTaskDelete(NULL);
}

static void game_event(int event, void *data)
{
    (void)data;
    if (event == RG_EVENT_SHUTDOWN)
        engine_running = false;
    else if (event == RG_EVENT_REDRAW && q2_surface.data)
        rg_display_submit(&q2_surface, 0);
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return q2_surface.data &&
           rg_surface_save_image_file(&q2_surface, filename, width, height);
}

void __real_rg_system_exit(void);

void __wrap_rg_system_exit(void)
{
    if (engine_task_handle && xTaskGetCurrentTaskHandle() == engine_task_handle) {
        engine_running = false;
        shutdown_video();
        extern void FS_ClosePakCache(void);
        FS_ClosePakCache();
        engine_task_handle = NULL;
        engine_done = true;
        vTaskDelete(NULL);
    }
    __real_rg_system_exit();
}

static void audio_ring_push(const rg_audio_frame_t *frames, int count)
{
    if (!audio_ring || count <= 0)
        return;

    uint32_t write = audio_ring_write;
    uint32_t offset = write % Q2_AUDIO_RING_FRAMES;
    int first = Q2_AUDIO_RING_FRAMES - offset;
    if (first > count)
        first = count;

    memcpy(audio_ring + offset, frames, first * sizeof(*frames));
    if (count > first)
        memcpy(audio_ring, frames + first, (count - first) * sizeof(*frames));

    audio_ring_write = write + count;
}

// Drains the ring at the speed the I2S DMA accepts it. Its i2s_write() call
// blocks when the driver ring is full, which is exactly the pacing we want, and
// it happens on core 0 where the renderer worker sleeps between frames.
static void audio_feeder(void *arg)
{
    (void)arg;
    rg_audio_frame_t chunk[Q2_AUDIO_SUBMIT_CHUNK];

    for (;;) {
        uint32_t count = 0;

        // Everything that touches read/write happens under the lock: the
        // engine task can reset the ring (SNDDMA_Shutdown/S_Init) at any time.
        xSemaphoreTake(audio_ring_lock, portMAX_DELAY);
        if (audio_ring) {
            uint32_t read = audio_ring_read;
            count = audio_ring_write - read;
            if (count > Q2_AUDIO_SUBMIT_CHUNK)
                count = Q2_AUDIO_SUBMIT_CHUNK;
            if (count) {
                uint32_t offset = read % Q2_AUDIO_RING_FRAMES;
                uint32_t first = Q2_AUDIO_RING_FRAMES - offset;
                if (first > count)
                    first = count;
                memcpy(chunk, audio_ring + offset, first * sizeof(*chunk));
                if (count > first)
                    memcpy(chunk + first, audio_ring,
                           (count - first) * sizeof(*chunk));
                audio_ring_read = read + count;
            }
        }
        xSemaphoreGive(audio_ring_lock);

        if (!count || !rg_audio_get_driver()) {
            vTaskDelay(1);
            continue;
        }

        rg_audio_submit(chunk, count);
    }
}

void app_main(void)
{
    const rg_config_t config = {
        .sampleRate = Q2_AUDIO_RATE,
        .frameRate = 30,
        .storageRequired = true,
        .romRequired = true,
        .handlers = {.event = game_event, .screenshot = screenshot_handler},
    };
    app = rg_system_init(&config);
    app->frameskip = -1;

    worker_start = xSemaphoreCreateBinary();
    worker_finish = xSemaphoreCreateBinary();
    audio_ring_lock = xSemaphoreCreateMutex();
    if (!worker_start || !worker_finish || !audio_ring_lock ||
        xTaskCreatePinnedToCore(renderer_worker, "q2_render", 4096, NULL,
                                RG_TASK_PRIORITY_5, NULL, 0) != pdPASS) {
        rg_gui_alert("Quake II", "Could not start the renderer worker.");
        rg_system_exit();
    }
    if (xTaskCreatePinnedToCore(audio_feeder, "q2_audio", 3584, NULL,
                                RG_TASK_PRIORITY_6, NULL, 0) != pdPASS) {
        rg_gui_alert("Quake II", "Could not start the audio task.");
        rg_system_exit();
    }

    engine_task_stack = heap_caps_malloc(Q2_TASK_STACK_BYTES,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!engine_task_stack) {
        rg_gui_alert("Quake II", "Not enough PSRAM for the game task.");
        rg_system_exit();
    }
    engine_task_handle = xTaskCreateStaticPinnedToCore(
        engine_task, "quake2", Q2_TASK_STACK_BYTES / sizeof(StackType_t), NULL,
        RG_TASK_PRIORITY_5, engine_task_stack, &engine_task_tcb, 1);
    if (!engine_task_handle) {
        heap_caps_free(engine_task_stack);
        engine_task_stack = NULL;
        rg_gui_alert("Quake II", "Could not start the game task.");
        rg_system_exit();
    }

    while (!engine_done)
        rg_task_delay(50);
    rg_system_exit();
}

qboolean SNDDMA_Init(void)
{
    if (!audio_buffer)
        audio_buffer = heap_caps_calloc(Q2_DMA_FRAMES, sizeof(*audio_buffer),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!audio_buffer)
        return false;
    if (!audio_ring)
        audio_ring = heap_caps_calloc(Q2_AUDIO_RING_FRAMES, sizeof(*audio_ring),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!audio_ring)
        return false;
    dma.channels = 2;
    dma.samples = Q2_DMA_FRAMES * 2;
    dma.samplepos = 0;
    dma.samplebits = 16;
    dma.speed = Q2_AUDIO_RATE;
    dma.submission_chunk = 256;
    dma.buffer = (byte *)audio_buffer;
    audio_submitted_frame = 0;
    if (audio_ring_lock)
        xSemaphoreTake(audio_ring_lock, portMAX_DELAY);
    audio_ring_write = 0;
    audio_ring_read = 0;
    if (audio_ring_lock)
        xSemaphoreGive(audio_ring_lock);
    return true;
}

int SNDDMA_GetDMAPos(void)
{
    // Frames handed to the audio driver by the feeder task: that cursor moves
    // at playback speed, so the engine paints exactly as far ahead as the
    // speaker has consumed.
    int64_t samples = rg_audio_get_counters().totalSamples;
    return (int)((samples % Q2_DMA_FRAMES) * 2);
}

void SNDDMA_BeginPainting(void) {}

void SNDDMA_Submit(void)
{
    if (!audio_buffer || !audio_ring || paintedtime <= audio_submitted_frame)
        return;

    int frames = paintedtime - audio_submitted_frame;
    int start = audio_submitted_frame;
    if (frames > Q2_DMA_FRAMES) {
        // the frame took longer than the whole mixing buffer: keep the newest
        start = paintedtime - Q2_DMA_FRAMES;
        frames = Q2_DMA_FRAMES;
    }
    audio_submitted_frame = paintedtime;

    // The I2S driver blocks when its 33 ms DMA ring is full, which used to
    // stall the renderer for a third of every frame. Hand the frames to the
    // feeder instead; if it fell behind (map load), drop this frame's audio
    // rather than stalling the game.
    uint32_t queued = audio_ring_write - audio_ring_read;
    if (queued + (uint32_t)frames > (uint32_t)Q2_AUDIO_RING_FRAMES)
        return;

    if (audio_ring_lock)
        xSemaphoreTake(audio_ring_lock, portMAX_DELAY);
    int offset = start % Q2_DMA_FRAMES;
    int first = Q2_DMA_FRAMES - offset;
    if (first > frames)
        first = frames;
    audio_ring_push(audio_buffer + offset, first);
    if (frames > first)
        audio_ring_push(audio_buffer, frames - first);
    if (audio_ring_lock)
        xSemaphoreGive(audio_ring_lock);
}

void SNDDMA_Shutdown(void)
{
    if (audio_ring_lock)
        xSemaphoreTake(audio_ring_lock, portMAX_DELAY);
    audio_ring_read = audio_ring_write;
    if (audio_ring_lock)
        xSemaphoreGive(audio_ring_lock);
    if (audio_buffer) {
        heap_caps_free(audio_buffer);
        audio_buffer = NULL;
    }
    dma.buffer = NULL;
}
