#include "shared.h"

#if RG_SCREEN_PIXEL_FORMAT == 0
#define FB_PIXEL_FORMAT RG_PIXEL_PAL565_BE
#define FB_PAL(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))
#else
#define FB_PIXEL_FORMAT RG_PIXEL_PAL565_LE
#define FB_PAL(c) ((uint16_t)(c))
#endif
#include <stdio.h>
#include <stdlib.h>

// Atari 7800 — ProSystem (components/prosystem), atari-core.
// MARIA draws 320 pixels a line in palette indices; the visible area is about
// 223 lines (NTSC) or 272 (PAL). TIA (and POKEY on some cartridges) gives two
// 8-bit samples a scanline: 262 x 2 x 60 = 31440 Hz (NTSC).

#include "ProSystem.h"
#include "Cartridge.h"
#include "Database.h"
#include "Bios.h"
#include "Maria.h"
#include "Palette.h"
#include "Region.h"
#include "Tia.h"
// Pokey.h by its declarations: the macOS checkout is case-insensitive, and the
// Atari 5200 core's pokey.h comes first in the include path
extern uint8_t pokey_buffer[];

#define A78_WIDTH 320
#define A78_MAX_HEIGHT 272
#define A78_STATE_SIZE 49221 // ProSystem's full save state (libretro's SAVE_STATE_SIZE)

static rg_app_t *app;
static rg_surface_t *update;

/* Pad -> the 17 inputs ProSystem reads each frame:
 *   0-3   joystick 1 right, left, down, up      <- D-pad
 *   4, 5  joystick 1 buttons 1 and 2            <- A, B (X also fires 1)
 *   6-11  joystick 2                             (none)
 *   12    console Reset (starts most games)     <- START
 *   13    console Select (game variation)       <- SELECT
 *   14    console Pause                         <- Y
 *   15,16 left and right difficulty switches    <- L, R flip them
 * MENU and OPTION open retro-go's menus. */
static uint8_t input[17];

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(update, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    char *buf = calloc(1, A78_STATE_SIZE);
    bool ok = buf && prosystem_Save(buf, false);
    FILE *fp = ok ? fopen(filename, "wb") : NULL;
    ok = fp && fwrite(buf, 1, A78_STATE_SIZE, fp) == A78_STATE_SIZE;
    if (fp)
        fclose(fp);
    free(buf);
    return ok;
}

static bool load_state_handler(const char *filename)
{
    char *buf = calloc(1, A78_STATE_SIZE);
    FILE *fp = fopen(filename, "rb");
    bool ok = buf && fp && fread(buf, 1, A78_STATE_SIZE, fp) == A78_STATE_SIZE && prosystem_Load(buf, false);
    if (fp)
        fclose(fp);
    free(buf);
    return ok;
}

static bool reset_handler(bool hard)
{
    prosystem_Reset();
    return true;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
        rg_display_submit(update, 0);
}

static void set_palette(void)
{
    for (int i = 0; i < 256; i++)
    {
        const uint16_t r = palette_data[i * 3], g = palette_data[i * 3 + 1], b = palette_data[i * 3 + 2];
        const uint16_t rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        update->palette[i] = FB_PAL(rgb565);
    }
}

void a78_main(void)
{
    const rg_handlers_t handlers = {
        .options = &core_options_handler,
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
    };

    void *rom_data;
    size_t rom_size;
    const char *rom_path = rg_system_get_app()->romPath;
    if (rg_extension_match(rom_path, "zip"))
    {
        if (!rg_storage_unzip_file(rom_path, NULL, &rom_data, &rom_size, 0))
            RG_PANIC("ROM file unzipping failed!");
    }
    else if (!rg_storage_read_file(rom_path, &rom_data, &rom_size, 0))
        RG_PANIC("ROM load failed!");

    if (!cartridge_Load(false, (const uint8_t *)rom_data, rom_size))
        RG_PANIC("Not an Atari 7800 cartridge");
    free(rom_data);
    database_Load(cartridge_digest);

    // The BIOS is optional (ProSystem starts the cartridge without it); with it
    // the game starts after the Atari logo, as on the console.
    // The cartridge's region first, then the other one (a card may hold only
    // the European BIOS: board, 2026-10-07).
    const char *bios_names[2] = {"7800 BIOS (U).rom", "7800 BIOS (E).rom"};
    const int first = cartridge_region == REGION_PAL ? 1 : 0;
    char bios[RG_PATH_MAX];
    for (int i = 0; i < 2 && !bios_enabled; i++)
    {
        snprintf(bios, sizeof(bios), RG_BASE_PATH_BIOS "/%s", bios_names[(first + i) & 1]);
        bios_enabled = bios_Load(bios);
    }
    RG_LOGI("BIOS: %s", bios_enabled ? bios : "none found, starting the cartridge without it");

    // left difficulty B ("novice"), right A: the defaults ProSystem's libretro
    // front end uses (Tower Toppler needs the right one at A)
    input[15] = 1;
    input[16] = 0;
    prosystem_Reset();

    const int fps = prosystem_frequency;
    const int sample_rate = prosystem_frequency * prosystem_scanlines * 2;
    app = rg_system_reinit(sample_rate, &handlers, NULL);
    rg_audio_set_sample_rate(sample_rate); // rg_system_reinit() applies the previous rate, so set it explicitly
    update = rg_surface_create(A78_WIDTH, A78_MAX_HEIGHT, FB_PIXEL_FORMAT, MEM_FAST);
    set_palette();
    RG_LOGI("%s, %d Hz, %d fps, sound %d Hz", cartridge_region == REGION_PAL ? "PAL" : "NTSC",
            fps, fps, sample_rate);

    if (app->bootFlags & RG_BOOT_RESUME)
        rg_emu_load_state(app->saveSlot);

    rg_system_set_tick_rate(fps);
    app->frameskip = 1;
    const int samples = prosystem_scanlines * 2;
    rg_audio_sample_t *stereo = malloc((samples + 16) * sizeof(rg_audio_sample_t));
    uint32_t previous = 0;
    int skipFrames = 0;
    int64_t prof_since = rg_system_timer(), prof_frame_us = 0;
    int prof_frames = 0;

    while (true)
    {
        uint32_t joystick = rg_input_read_gamepad();

        if (joystick & RG_KEY_MENU)
            rg_gui_game_menu();
        else if (joystick & RG_KEY_OPTION)
            rg_gui_options_menu();

        const uint32_t pressed = joystick & ~previous;
        previous = joystick;
        input[0] = (joystick & RG_KEY_RIGHT) != 0;
        input[1] = (joystick & RG_KEY_LEFT) != 0;
        input[2] = (joystick & RG_KEY_DOWN) != 0;
        input[3] = (joystick & RG_KEY_UP) != 0;
        input[4] = (joystick & (RG_KEY_A | RG_KEY_X)) != 0;
        input[5] = (joystick & RG_KEY_B) != 0;
        input[12] = (joystick & RG_KEY_START) != 0;
        input[13] = (joystick & RG_KEY_SELECT) != 0;
        input[14] = (joystick & RG_KEY_Y) != 0;
        if (pressed & RG_KEY_L)
            input[15] ^= 1;
        if (pressed & RG_KEY_R)
            input[16] ^= 1;

        int64_t startTime = rg_system_timer();
        bool drawFrame = skipFrames == 0;
        bool slowFrame = false;

        const int64_t prof_start = rg_system_timer();
        prosystem_ExecuteFrame(input);
        prof_frame_us += rg_system_timer() - prof_start;
        prof_frames++;
        if (rg_system_timer() - prof_since >= 1000000)
        {
            // A78PROF: the emulation of a frame (6502 and MARIA, including
            // the drawing into its own picture), the rest of the loop
            const int64_t period = rg_system_timer() - prof_since;
            extern uint32_t prosystem_prof_maria_us;
            RG_LOGI("A78PROF frames=%d emulation=%d us a frame (MARIA %d, the 6502 and the rest %d), outside it %d us",
                    prof_frames, (int)(prof_frame_us / prof_frames), (int)(prosystem_prof_maria_us / prof_frames),
                    (int)((prof_frame_us - prosystem_prof_maria_us) / prof_frames),
                    (int)((period - prof_frame_us) / prof_frames));
            prosystem_prof_maria_us = 0;
            prof_since = rg_system_timer();
            prof_frame_us = 0;
            prof_frames = 0;
        }

        if (drawFrame)
        {
            const int width = maria_visibleArea.right - maria_visibleArea.left + 1;
            int height = maria_visibleArea.bottom - maria_visibleArea.top + 1;
            if (height > A78_MAX_HEIGHT)
                height = A78_MAX_HEIGHT;
            const uint8_t *src = maria_surface + (maria_visibleArea.top - maria_displayArea.top) * width;
            update->width = width;
            update->height = height;
            memcpy(update->data, src, width * height);
            slowFrame = rg_display_is_busy();
            rg_display_submit(update, 0);
        }

        // TIA (the two channels' volumes added, 0 to 30), mixed with POKEY on
        // the cartridges that have one; scaled as ProSystem's libretro front end does
        for (int i = 0; i < samples; i++)
        {
            int s = tia_buffer[i];
            if (cartridge_pokey_address)
                s = (s + pokey_buffer[i]) >> 1;
            stereo[i].left = stereo[i].right = (int16_t)(s << 8);
        }

        rg_system_tick(rg_system_timer() - startTime);
        rg_audio_submit(stereo, samples);

        if (skipFrames == 0)
        {
            int elapsed = rg_system_timer() - startTime;
            if (core_frameskip(app) > 0)
                skipFrames = core_frameskip(app);
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
