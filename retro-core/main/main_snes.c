#include "shared.h"

#include <snes9x.h>
#include <math.h>
// pjcau's retro-go has an on-screen debug HUD; this retro-go does not: the profiler's HUD text is dropped
#define rg_system_set_hud_text(text) ((void)0)
#if SNES_PROF
#include "../components/snes9x/src/snes_prof.h"
#endif

typedef struct
{
	char name[16];
	struct {
		uint16_t snes9x_mask;
		uint16_t local_mask;
		uint16_t mod_mask;
	} keys[16];
} keymap_t;

static const keymap_t KEYMAPS[] = {
	// esp32-emu-turbo has all twelve SNES buttons: map them 1:1 (default).
	// The Odroid-style Type A/B/C presets below stay selectable.
	{"Native", {
		{SNES_A_MASK, RG_KEY_A, 0},
		{SNES_B_MASK, RG_KEY_B, 0},
		{SNES_X_MASK, RG_KEY_X, 0},
		{SNES_Y_MASK, RG_KEY_Y, 0},
		{SNES_TL_MASK, RG_KEY_L, 0},
		{SNES_TR_MASK, RG_KEY_R, 0},
		{SNES_START_MASK, RG_KEY_START, 0},
		{SNES_SELECT_MASK, RG_KEY_SELECT, 0},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
	{"Type A", {
		{SNES_A_MASK, RG_KEY_A, 0},
		{SNES_B_MASK, RG_KEY_B, 0},
		{SNES_X_MASK, RG_KEY_START, 0},
		{SNES_Y_MASK, RG_KEY_SELECT, 0},
		{SNES_TL_MASK, RG_KEY_B, RG_KEY_MENU},
		{SNES_TR_MASK, RG_KEY_A, RG_KEY_MENU},
		{SNES_START_MASK, RG_KEY_START, RG_KEY_MENU},
		{SNES_SELECT_MASK, RG_KEY_SELECT, RG_KEY_MENU},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
	{"Type B", {
		{SNES_A_MASK, RG_KEY_START, 0},
		{SNES_B_MASK, RG_KEY_A, 0},
		{SNES_X_MASK, RG_KEY_SELECT, 0},
		{SNES_Y_MASK, RG_KEY_B, 0},
		{SNES_TL_MASK, RG_KEY_B, RG_KEY_MENU},
		{SNES_TR_MASK, RG_KEY_A, RG_KEY_MENU},
		{SNES_START_MASK, RG_KEY_START, RG_KEY_MENU},
		{SNES_SELECT_MASK, RG_KEY_SELECT, RG_KEY_MENU},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
	{"Type C", {
		{SNES_A_MASK, RG_KEY_A, 0},
		{SNES_B_MASK, RG_KEY_B, 0},
		{SNES_X_MASK, 0, 0},
		{SNES_Y_MASK, 0, 0},
		{SNES_TL_MASK, 0, 0},
		{SNES_TR_MASK, 0, 0},
		{SNES_START_MASK, RG_KEY_START, 0},
		{SNES_SELECT_MASK, RG_KEY_SELECT, 0},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
};

static const size_t KEYMAPS_COUNT = (sizeof(KEYMAPS) / sizeof(keymap_t));

static const char *SNES_BUTTONS[] = {
	"None", "None", "None", "None", "R", "L", "X", "A", "Right", "Left", "Down", "Up", "Start", "Select", "Y", "B"
};

#define AUDIO_LOW_PASS_RANGE ((60 * 65536) / 100)

static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_audio_sample_t *audioBuffer;

static bool apu_enabled = true;
static bool lowpass_filter = false;
static bool transparency = true;

static int keymap_id = 0;
static keymap_t keymap;

static const char *SETTING_KEYMAP = "keymap";
static const char *SETTING_APU_EMULATION = "apu";
static const char *SETTING_TRANSPARENCY = "transp";
// --- MAIN

static void update_keymap(int id)
{
    keymap_id = id % KEYMAPS_COUNT;
    keymap = KEYMAPS[keymap_id];
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    return S9xSaveState(filename);
}

static bool load_state_handler(const char *filename)
{
    return S9xLoadState(filename);
}

static bool reset_handler(bool hard)
{
    S9xReset();
    return true;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(currentUpdate, 0);
    }
}

static rg_gui_event_t apu_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        apu_enabled = !apu_enabled;
        rg_settings_set_number(NS_APP, SETTING_APU_EMULATION, apu_enabled);
    }

    strcpy(option->value, apu_enabled ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t lowpass_filter_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        lowpass_filter = !lowpass_filter;

    strcpy(option->value, lowpass_filter ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t transparency_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        transparency = !transparency;
        Settings.NoTransparency = !transparency;
        rg_settings_set_number(NS_APP, SETTING_TRANSPARENCY, transparency);
    }

    strcpy(option->value, transparency ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t change_keymap_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        if (event == RG_DIALOG_PREV && --keymap_id < 0)
            keymap_id = KEYMAPS_COUNT - 1;
        if (event == RG_DIALOG_NEXT && ++keymap_id > KEYMAPS_COUNT - 1)
            keymap_id = 0;
        update_keymap(keymap_id);
        rg_settings_set_number(NS_APP, SETTING_KEYMAP, keymap_id);
        return RG_DIALOG_REDRAW;
    }

    if (event == RG_DIALOG_ENTER)
    {
        return RG_DIALOG_CANCEL;
    }

    if (option->arg == -1)
    {
        strcat(strcat(strcpy(option->value, "< "), keymap.name), " >");
    }
    else if (option->arg >= 0)
    {
        int local_button = keymap.keys[option->arg].local_mask;
        int mod_button = keymap.keys[option->arg].mod_mask;
        int snes9x_button = log2(keymap.keys[option->arg].snes9x_mask); // convert bitmask to bit number

        if (snes9x_button < 4 || (local_button & (RG_KEY_UP|RG_KEY_DOWN|RG_KEY_LEFT|RG_KEY_RIGHT)))
        {
            option->flags = RG_DIALOG_FLAG_HIDDEN;
            return RG_DIALOG_VOID;
        }

        if (keymap.keys[option->arg].mod_mask)
            sprintf(option->value, "%s + %s", rg_input_get_key_name(mod_button), rg_input_get_key_name(local_button));
        else
            sprintf(option->value, "%s", rg_input_get_key_name(local_button));

        option->label = SNES_BUTTONS[snes9x_button];
        option->flags = RG_DIALOG_FLAG_NORMAL;
    }

    return RG_DIALOG_VOID;
}

static rg_gui_event_t menu_keymap_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_gui_option_t options[] = {
            {-1, _("Profile"), "-", RG_DIALOG_FLAG_NORMAL, &change_keymap_cb},
            {-2, "", NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
            {-3, "snes9x  ", "handheld", RG_DIALOG_FLAG_MESSAGE, NULL},
            {0, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {1, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {2, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {3, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {4, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {5, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {6, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {7, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {8, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {9, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {10, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {11, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {12, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {13, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {14, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {15, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(option->label, options, 0);
        return RG_DIALOG_REDRAW;
    }

    strcpy(option->value, keymap.name);
    return RG_DIALOG_VOID;
}

bool S9xInitDisplay(void)
{
    GFX.Pitch = SNES_WIDTH * 2;
    GFX.ZPitch = SNES_WIDTH;
    GFX.Screen = currentUpdate->data;
    GFX.SubScreen = malloc(GFX.Pitch * SNES_HEIGHT_EXTENDED);
    // The z-buffers see a read-modify-write per drawn pixel (tile.c
    // WRITE_4PIXELS16*); at 61 KB each they were landing in PSRAM through
    // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=32768. Ask for internal SRAM;
    // rg_alloc falls back to PSRAM (with a log warning) if it does not fit.
    GFX.ZBuffer = rg_alloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED, MEM_FAST);
    GFX.SubZBuffer = malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED); // 2nd one does not fit next to the cache upgrade (117 KB free, 56 KB largest block)
    return GFX.Screen && GFX.SubScreen && GFX.ZBuffer && GFX.SubZBuffer;
}

void S9xDeinitDisplay(void)
{
}

uint32_t S9xReadJoypad(int32_t port)
{
    if (port != 0)
        return 0;

    uint32_t joystick = rg_input_read_gamepad();
    uint32_t joypad = 0;

    for (int i = 0; i < RG_COUNT(keymap.keys); ++i)
    {
        uint32_t bitmask = keymap.keys[i].local_mask | keymap.keys[i].mod_mask;
        if (bitmask && bitmask == (joystick & bitmask))
        {
            joypad |= keymap.keys[i].snes9x_mask;
        }
    }

    return joypad;
}

bool S9xReadMousePosition(int32_t which1, int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool JustifierOffscreen(void)
{
    return true;
}

void JustifierButtons(uint32_t *justifiers)
{
    (void)justifiers;
}

#ifdef USE_BLARGG_APU
static void S9xAudioCallback(void)
{
    S9xFinalizeSamples();
    size_t available_samples = S9xGetSampleCount();
    S9xMixSamples((void *)audioBuffer, available_samples);
    rg_audio_submit(audioBuffer, available_samples >> 1);
}
#endif

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Audio enable"), "-", RG_DIALOG_FLAG_NORMAL, &apu_toggle_cb};
    *dest++ = (rg_gui_option_t){0, _("Audio filter"), "-", RG_DIALOG_FLAG_NORMAL, &lowpass_filter_cb};
    *dest++ = (rg_gui_option_t){0, _("Transparency"), "-", RG_DIALOG_FLAG_NORMAL, &transparency_cb};
    *dest++ = (rg_gui_option_t){0, _("Controls"),     "-", RG_DIALOG_FLAG_NORMAL, &menu_keymap_cb};
    *dest++ = CORE_MAXSKIP_OPTION;
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

void snes_main(void)
{
    const rg_handlers_t handlers = {
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
        .options = &options_handler,
    };
    app = rg_system_reinit(AUDIO_SAMPLE_RATE, &handlers, NULL);

    apu_enabled = rg_settings_get_number(NS_APP, SETTING_APU_EMULATION, 1);
    transparency = rg_settings_get_number(NS_APP, SETTING_TRANSPARENCY, 1);

    updates[0] = rg_surface_create(SNES_WIDTH, SNES_HEIGHT_EXTENDED, RG_PIXEL_565_LE, 0);
    updates[0]->height = SNES_HEIGHT;
    currentUpdate = updates[0];

    audioBuffer = (rg_audio_sample_t *)malloc(AUDIO_BUFFER_LENGTH * 4);

    update_keymap(rg_settings_get_number(NS_APP, SETTING_KEYMAP, 0));

    Settings.CyclesPercentage = 100;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = AUDIO_SAMPLE_RATE;
    Settings.SoundInputRate = AUDIO_SAMPLE_RATE;
    Settings.DisableSoundEcho = false;
    Settings.InterpolatedSound = true;
    Settings.NoTransparency = !transparency;

    if (!S9xInitDisplay())
        RG_PANIC("Display init failed!");

    if (!S9xInitMemory())
        RG_PANIC("Memory init failed!");

    if (!S9xInitAPU())
        RG_PANIC("APU init failed!");

    if (!S9xInitSound(0, 0))
        RG_PANIC("Sound init failed!");

    if (!S9xInitGFX())
        RG_PANIC("Graphics init failed!");

    const char *filename = app->romPath;

    if (rg_extension_match(filename, "zip"))
    {
        if (!rg_storage_unzip_file(filename, NULL, (void **)&Memory.ROM, &Memory.ROM_AllocSize, RG_FILE_USER_BUFFER))
            RG_PANIC("ROM file unzipping failed!");
        filename = NULL;
    }

    if (!LoadROM(filename))
        RG_PANIC("ROM loading failed!");

#ifdef USE_BLARGG_APU
    S9xSetSamplesAvailableCallback(S9xAudioCallback);
#else
    S9xSetPlaybackRate(Settings.SoundPlaybackRate);
#endif

    if (app->bootFlags & RG_BOOT_RESUME)
    {
        rg_emu_load_state(app->saveSlot);
    }

    rg_system_set_tick_rate(Memory.ROMFramesPerSecond);
    app->frameskip = 3;

#ifndef USE_BLARGG_APU
    // S-DSP mixing on core 1 (snes9x apu.c, roadmap Phase 5): the frame's
    // DSP register writes are replayed there while core 0 runs the next
    // frame; audio one frame later. Same samples as inline (PC harness).
    if (!S9xAudioOffload(true))
        RG_LOGW("S-DSP stays on core 0 (no memory for the queues)\n");
#endif

    // Samples to mix and submit per emulated frame. AUDIO_BUFFER_LENGTH
    // (rate/50+1) is only right for PAL: on a 60 fps ROM it hands the sink
    // 20% more audio than real time, so the sink's pacing caps emulation at
    // 50 fps and the APU underruns every frame (measured on the first
    // article: 42-50 fps at 50-70% CPU, crackling audio). The buffer is
    // allocated at the PAL size, which is the larger of the two.
    int samplesPerFrame = AUDIO_SAMPLE_RATE / Memory.ROMFramesPerSecond;
    if (samplesPerFrame > AUDIO_BUFFER_LENGTH)
        samplesPerFrame = AUDIO_BUFFER_LENGTH;
    RG_LOGI("ROM '%s': region=%d PAL=%d fps=%d -> %d samples/frame @ %d Hz, frameskip=%d\n",
            Memory.ROMName, Memory.ROMRegion, Settings.PAL, Memory.ROMFramesPerSecond,
            samplesPerFrame, AUDIO_SAMPLE_RATE, app->frameskip);

    bool menuCancelled = false;
    bool menuPressed = false;
    int skipFrames = 0;

#if SNES_PROF
    // Per-second breakdown of where the loop's wall time goes (Phase 4 step
    // 4.0, esp32-emu-turbo). Build with SNES_PROF=1 in the environment: it
    // adds only these counters, not -finstrument-functions (which is what
    // RG_ENABLE_PROFILING does, and which inflates every small function).
    // First-article measurement (Super Mario World, 2026-09-12): a frame
    // without rendering costs ~8.5 ms, a rendered frame ~40-50 ms — the PPU
    // renderer, not the CPU/APU emulation, the audio or the display path.
    int64_t prof_t0 = rg_system_timer();
    int64_t prof_main_drawn = 0, prof_main_skip = 0, prof_disp = 0, prof_mix = 0, prof_audio = 0, prof_loop = 0;
    int prof_n = 0, prof_drawn = 0;
    char hud_text[192] = "";
#endif

    while (1)
    {
#if SNES_PROF
        int64_t loopStart = rg_system_timer();
#endif
        uint32_t joystick = rg_input_read_gamepad();

        if (menuPressed && !(joystick & RG_KEY_MENU))
        {
            if (!menuCancelled)
            {
                rg_task_delay(50);
                rg_gui_game_menu();
            }
            menuCancelled = false;
        }
        else if (joystick & RG_KEY_OPTION)
        {
            rg_gui_options_menu();
        }

        menuPressed = joystick & RG_KEY_MENU;

        if (menuPressed && joystick & ~RG_KEY_MENU)
        {
            menuCancelled = true;
        }

        int64_t startTime = rg_system_timer();
        bool drawFrame = (skipFrames == 0);
        bool slowFrame = false;

        IPPU.RenderThisFrame = drawFrame;
        GFX.Screen = currentUpdate->data;

        S9xMainLoop();
#if SNES_PROF
        int64_t tMain = rg_system_timer();
#endif

        if (drawFrame)
        {
            slowFrame = rg_display_is_busy();
            rg_display_submit(currentUpdate, 0);
        }
#if SNES_PROF
        int64_t tDisp = rg_system_timer();
#endif

    #ifndef USE_BLARGG_APU
        if (S9xAudioOffloaded())
        {
            // last frame's samples (mixed on core 1), then this frame's job
            int16_t *mixed = S9xAudioFrame(samplesPerFrame, lowpass_filter, AUDIO_LOW_PASS_RANGE);
            if (mixed)
                memcpy(audioBuffer, mixed, samplesPerFrame * 4);
            else
                memset(audioBuffer, 0, samplesPerFrame * 4);
        }
        else if (apu_enabled && lowpass_filter)
            S9xMixSamplesLowPass((void *)audioBuffer, samplesPerFrame << 1, AUDIO_LOW_PASS_RANGE);
        else if (apu_enabled)
            S9xMixSamples((void *)audioBuffer, samplesPerFrame << 1);
    #endif
#if SNES_PROF
        int64_t tMix = rg_system_timer();
#endif

        rg_system_tick(rg_system_timer() - startTime);

    #ifndef USE_BLARGG_APU
        if (apu_enabled)
            rg_audio_submit(audioBuffer, samplesPerFrame);
    #endif
#if SNES_PROF
        int64_t tAudio = rg_system_timer();

        if (drawFrame) { prof_main_drawn += tMain - startTime; prof_drawn++; }
        else prof_main_skip += tMain - startTime;
        prof_disp += tDisp - tMain; prof_mix += tMix - tDisp;
        prof_audio += tAudio - tMix; prof_loop += tAudio - loopStart; prof_n++;
        if (tAudio - prof_t0 >= 1000000)
        {
            int64_t wall = tAudio - prof_t0;
            int skip = prof_n - prof_drawn, d = prof_drawn ? prof_drawn : 1;
            // R = renderer cost per drawn frame = drawn-frame S9xMainLoop minus a non-drawn one
            int main_drawn = (int)(prof_main_drawn / d);
            int main_skip = skip ? (int)(prof_main_skip / skip) : 0;
            int R = main_drawn - main_skip;
            rg_stats_t st = rg_system_get_stats();
            RG_LOGI("PROF n=%d drawn=%d wall=%dms fps=%.0f busy=%.0f%% | us/frame: main(drawn)=%d main(skip)=%d R=%d disp=%d mix=%d audio=%d loop=%d other=%d%%\n",
                    prof_n, prof_drawn, (int)(wall / 1000), st.totalFPS, st.busyPercent,
                    main_drawn, main_skip, R, (int)(prof_disp / prof_n),
                    (int)(prof_mix / prof_n), (int)(prof_audio / prof_n), (int)(prof_loop / prof_n),
                    (int)((wall - prof_loop) * 100 / wall));
            snes_prof_t *sp = &snes_prof;
            RG_LOGI("PROF/drawn-frame: strips=%.1f lines=%.0f sub=%.2f tiles=%.0f blank=%.0f conv=%.1f objsetup=%.2f | us: update=%d clear=%d sub=%d main=%d combine=%d obj=%d bg0=%d bg1=%d bg2=%d bg3=%d objsetup=%d | modes 0:%lu 1:%lu 2:%lu 3:%lu 4:%lu 5:%lu 6:%lu 7:%lu\n",
                    (float)sp->strips / d, (float)sp->strip_lines / d, (float)sp->sub_passes / d,
                    (float)sp->tiles / d, (float)sp->tiles_blank / d, (float)sp->tile_conv / d, (float)sp->obj_setup / d,
                    (int)(sp->t_update / d), (int)(sp->t_clear / d), (int)(sp->t_sub / d), (int)(sp->t_main / d),
                    (int)(sp->t_combine / d), (int)(sp->t_obj / d), (int)(sp->t_bg[0] / d), (int)(sp->t_bg[1] / d),
                    (int)(sp->t_bg[2] / d), (int)(sp->t_bg[3] / d), (int)(sp->t_objsetup / d),
                    sp->mode_hist[0], sp->mode_hist[1], sp->mode_hist[2], sp->mode_hist[3],
                    sp->mode_hist[4], sp->mode_hist[5], sp->mode_hist[6], sp->mode_hist[7]);
            RG_LOGI("PROF/layers: bg0 %.0f/%.0f/%.0f bg1 %.0f/%.0f/%.0f bg2 %.0f/%.0f/%.0f bg3 %.0f/%.0f/%.0f obj %.0f/%.0f/%.0f (tiles/clipped/lines per drawn frame) m7lines=%.0f m7us=%d m7var=%lu/%lu/%lu/%lu/%lu subempty_why=%lu m7runs=%.0f\n",
                    (float)sp->l_tiles[0] / d, (float)sp->l_clipped[0] / d, (float)sp->l_lines[0] / d,
                    (float)sp->l_tiles[1] / d, (float)sp->l_clipped[1] / d, (float)sp->l_lines[1] / d,
                    (float)sp->l_tiles[2] / d, (float)sp->l_clipped[2] / d, (float)sp->l_lines[2] / d,
                    (float)sp->l_tiles[3] / d, (float)sp->l_clipped[3] / d, (float)sp->l_lines[3] / d,
                    (float)sp->l_tiles[4] / d, (float)sp->l_clipped[4] / d, (float)sp->l_lines[4] / d,
                    (float)sp->m7_lines / d, (int)(sp->t_m7 / d),
                    sp->m7_variant[0], sp->m7_variant[1], sp->m7_variant[2], sp->m7_variant[3], sp->m7_variant[4], sp->subempty_why, (float)sp->m7_runs / d);
            {
                char fl[200] = ""; int n = 0;
                for (int i = 0; i < 0x40 && n < 180; i++)
                    if (sp->flush_hist[i]) n += snprintf(fl + n, sizeof(fl) - n, " %02x:%.1f", 0x00 + i, (float)sp->flush_hist[i] / d);
                RG_LOGI("PROF/flush (strips per drawn frame by register 0x21xx):%s\n", fl);
                n = 0; fl[0] = 0;
                for (int i = 0; i < 32 && n < 180; i++)
                    if (sp->cg_hist[i]) n += snprintf(fl + n, sizeof(fl) - n, " %s%d:%.1f", i < 16 ? "e" : "g", i < 16 ? i : (i - 16) * 16, (float)sp->cg_hist[i] / d);
                RG_LOGI("PROF/cgram (mid-strip palette changes per drawn frame by entry group):%s zero=%.1f\n", fl, (float)sp->cg_zero / d);
            }
            // On-screen HUD: 7 columns fit the 57 px letterbox bar left of the
            // 366x320 game viewport, which the display task never rewrites.
            // Drawn below, outside the timed sections, only when the display
            // is idle (rg_gui_draw_text blocks until pending updates finish).
            int mode = 0;
            for (int i = 1; i < 8; ++i) if (sp->mode_hist[i] > sp->mode_hist[mode]) mode = i;
            // Extra lines for the common Debug HUD (rg_system): renderer cost R,
            // CPU-only frame N, strips, sub passes, tiles, conversions, OBJ/BG/clear ms, mode.
            snprintf(hud_text, sizeof(hud_text),
                     "R %5.1f\nN %5.1f\nSTR%4.1f\nSUB%4.1f\nTIL%4.0f\nCNV%4.0f\nOBJ%4.1f\nBG %4.1f\nCLR%4.1f\nM%d %3d%%",
                     R / 1000.0f, main_skip / 1000.0f,
                     (float)sp->strips / d, (float)sp->sub_passes / d, (float)sp->tiles / d, (float)sp->tile_conv / d,
                     sp->t_obj / 1000.0f / d, (sp->t_bg[0] + sp->t_bg[1] + sp->t_bg[2] + sp->t_bg[3]) / 1000.0f / d,
                     sp->t_clear / 1000.0f / d, mode, sp->strips ? (int)(sp->mode_hist[mode] * 100 / sp->strips) : 0);
            rg_system_set_hud_text(hud_text);
            memset(sp, 0, sizeof(*sp));
            prof_t0 = tAudio; prof_main_drawn = prof_main_skip = prof_disp = prof_mix = prof_audio = prof_loop = 0;
            prof_n = prof_drawn = 0;
        }
#endif

        if (skipFrames == 0)
        {
            int elapsed = rg_system_timer() - startTime;
            // SNES is tuned for a baseline of 3 (set above), so its ceiling never goes below that
            const int snesSkip = (app->speed > 1.01f) ? app->frameskip : RG_MIN(app->frameskip, RG_MAX(core_maxskip, 3));
            if (snesSkip > 0)
                skipFrames = snesSkip;
            else if (elapsed > app->frameTime + 1500) // Allow some jitter
                skipFrames = 1; // (elapsed / frameTime)
            else if (drawFrame && slowFrame)
                skipFrames = 1;
        }
        else if (skipFrames > 0)
        {
            skipFrames--;
        }
    }
}
