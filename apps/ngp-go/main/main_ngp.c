#include "shared.h"

// Neo Geo Pocket / Color — libretro RACE core (components/race), esp32-emu-turbo 2026-09-14.
// The core renders 160x152 RGB565 through graphics_paint() and mixes mono audio
// per frame; this file replaces libretro.c: video, audio, input, save states.

#include <libretro.h>
#include <main.h>
#include <graphics.h>
#include <race-memory.h>
#include <tlcs900h.h>
#include <flash.h>
#include <state.h>
#include <neopopsound.h>
#include <neopop_blip.h>
#include <libretro.h>

#define NGP_WIDTH  160
#define NGP_HEIGHT 152
#define NGP_CPU_FREQ 6144000
#define NGP_FPS 60
/* The sound chip runs at 16 kHz and the output is doubled (interpolated) to
 * 32 kHz, the rate every app uses (the PDM sink misbehaves at 22050). Running
 * the chip itself at 32 kHz pushed the CPU to 95-97% on Metal Slug. */
#define NGP_SAMPLE_RATE 16000
#define NGP_OUTPUT_RATE (NGP_SAMPLE_RATE * 2)

/* Globals the core expects from the frontend (were in libretro.c) */
struct ngp_screen *screen;
int setting_ngp_language = 0; /* 0 = English, 1 = Japanese */
int gfx_hacks = 0;
int tipo_consola = 0;         /* 0 = Color, 1 = mono NGP */
char retro_save_directory[2048];
retro_log_printf_t log_cb = NULL;
uint8_t ngpInputState = 0;

static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static bool frame_rendered;

void graphics_paint(unsigned char render)
{
    frame_rendered = render;
}

void handle_error(const char *error)
{
    RG_LOGE("RACE: %s", error);
}

static bool save_state_handler(const char *filename)
{
    size_t size = state_get_size();
    void *buf = malloc(size);
    bool ok = buf && state_store_mem(buf);
    if (ok)
    {
        FILE *fp = fopen(filename, "wb");
        ok = fp && fwrite(buf, 1, size, fp) == size;
        if (fp) fclose(fp);
    }
    free(buf);
    flashShutdown(); /* also commit the cartridge flash (.ngf) */
    return ok;
}

static bool load_state_handler(const char *filename)
{
    size_t size = state_get_size();
    void *buf = malloc(size);
    bool ok = false;
    FILE *fp = fopen(filename, "rb");
    if (buf && fp && fread(buf, 1, size, fp) == size)
        ok = state_restore_mem(buf) == 1;
    if (fp) fclose(fp);
    free(buf);
    return ok;
}

static bool reset_handler(bool hard)
{
    flashShutdown();
    system_sound_chipreset(NGP_SAMPLE_RATE);
    mainemuinit();
    return true;
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
        rg_display_submit(currentUpdate, 0);
    else if (event == RG_EVENT_SHUTDOWN)
        flashShutdown();
}

void ngp_main(void)
{
    rg_config_t config;
    memset(&config, 0, sizeof(config));
    config.sampleRate = NGP_OUTPUT_RATE;
    config.frameRate = NGP_FPS;
    config.storageRequired = true;
    config.romRequired = true;
    config.handlers.loadState = load_state_handler;
    config.handlers.saveState = save_state_handler;
    config.handlers.reset = reset_handler;
    config.handlers.screenshot = screenshot_handler;
    config.handlers.event = event_handler;

    app = rg_system_init(&config);

    updates[0] = rg_surface_create(NGP_WIDTH, NGP_HEIGHT, RG_PIXEL_565_LE, MEM_FAST);
    currentUpdate = updates[0];

    static struct ngp_screen ngp_screen;
    ngp_screen.w = NGP_WIDTH;
    ngp_screen.h = NGP_HEIGHT;
    ngp_screen.pixels = currentUpdate->data;
    screen = &ngp_screen;

    snprintf(retro_save_directory, sizeof(retro_save_directory), "%s/", RG_BASE_PATH_SAVES);

    /* Core memory in PSRAM, only while this app runs */
    mainram = rg_alloc(MAINRAM_SIZE, MEM_SLOW);
    mainrom = rg_alloc(MAINROM_SIZE_MAX, MEM_SLOW);
    cpurom = rg_alloc(CPUROM_SIZE, MEM_SLOW);
    dacBufferL = rg_alloc(DAC_BUFFERSIZE * sizeof(uint16_t), MEM_SLOW);
    totalpalette = rg_alloc(32 * 32 * 32 * sizeof(int), MEM_SLOW);
    extern void race_cz80_bind(void);
    extern void race_alloc_sprites(void *, void *, void *);
    race_cz80_bind();
    race_alloc_sprites(rg_alloc(40 * 1024, MEM_SLOW), rg_alloc(40 * 1024, MEM_SLOW), rg_alloc(40 * 1024, MEM_SLOW));

    void *rom_data;
    size_t rom_size;
    if (!rg_storage_read_file(app->romPath, &rom_data, &rom_size, 0))
        RG_PANIC("ROM load failed!");
    if (!handleInputFile(app->romPath, rom_data, rom_size))
        RG_PANIC("Not a valid NGP/NGPC ROM");
    free(rom_data);

    system_sound_chipreset(NGP_SAMPLE_RATE);
    neopop_audio_accurate = 0;
    mainemuinit();

    if (app->bootFlags & RG_BOOT_RESUME)
        rg_emu_load_state(app->saveSlot);

    rg_system_set_tick_rate(NGP_FPS);
    app->frameskip = 1;

    const int samplesPerFrame = NGP_SAMPLE_RATE / NGP_FPS;
    static int16_t mono[NGP_SAMPLE_RATE / NGP_FPS + 16];
    static rg_audio_sample_t stereo[(NGP_SAMPLE_RATE / NGP_FPS + 16) * 2];
    int16_t prev = 0;
    int skipFrames = 0;

    while (true)
    {
        uint32_t joystick = rg_input_read_gamepad();

        if (joystick & RG_KEY_MENU)
            rg_gui_game_menu();
        else if (joystick & RG_KEY_OPTION)
            rg_gui_options_menu();

        int64_t startTime = rg_system_timer();
        bool drawFrame = skipFrames == 0;
        bool slowFrame = false;

        /* NGP pad: 0x01 up 0x02 down 0x04 left 0x08 right 0x10 A 0x20 B 0x40 option */
        ngpInputState = 0;
        if (joystick & RG_KEY_UP)    ngpInputState |= 0x01;
        if (joystick & RG_KEY_DOWN)  ngpInputState |= 0x02;
        if (joystick & RG_KEY_LEFT)  ngpInputState |= 0x04;
        if (joystick & RG_KEY_RIGHT) ngpInputState |= 0x08;
        if (joystick & RG_KEY_A)     ngpInputState |= 0x10;
        if (joystick & RG_KEY_B)     ngpInputState |= 0x20;
        if (joystick & RG_KEY_START) ngpInputState |= 0x40;

        rtc_tick_frame();
        frame_rendered = false;
        tlcs_execute(NGP_CPU_FREQ / NGP_FPS, !drawFrame);

        if (drawFrame)
        {
            slowFrame = rg_display_is_busy();
            rg_display_submit(currentUpdate, 0);
        }

        ngp_sound_update((uint16_t *)mono, samplesPerFrame * sizeof(int16_t));
        dac_update((uint16_t *)mono, samplesPerFrame * sizeof(int16_t));
        for (int i = 0; i < samplesPerFrame; i++)
        {
            int16_t mid = (prev + mono[i]) / 2;
            stereo[2 * i].left = stereo[2 * i].right = mid;
            stereo[2 * i + 1].left = stereo[2 * i + 1].right = mono[i];
            prev = mono[i];
        }

        rg_system_tick(rg_system_timer() - startTime);
        rg_audio_submit(stereo, samplesPerFrame * 2);

        if (skipFrames == 0)
        {
            int elapsed = rg_system_timer() - startTime;
            if (app->frameskip > 0)
                skipFrames = app->frameskip;
            else if (elapsed > app->frameTime + 1500)
                skipFrames = 1;
            else if (drawFrame && slowFrame)
                skipFrames = 1;
        }
        else if (skipFrames > 0)
        {
            skipFrames--;
        }
    }
}
