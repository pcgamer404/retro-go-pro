#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>

#include <gwenesis.h>

#define AUDIO_SAMPLE_RATE (53267)

// GEN_PROF=1 env var at build (gwenesis/CMakeLists.txt): per-second cost split
// of the frame loop, one timer read per section per scanline.
#if GEN_PROF
static struct { int64_t frame, m68k, z80, ym, sn, vdp; } gen_prof;
#define GEN_PROF_T0(v)      int64_t v = rg_system_timer()
#define GEN_PROF_ACC(f, v)  do { int64_t _n = rg_system_timer(); gen_prof.f += _n - (v); (v) = _n; } while (0)
#else
#define GEN_PROF_T0(v)      ((void)0)
#define GEN_PROF_ACC(f, v)  ((void)0)
#endif
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 60 + 1)
/* The YM2612 renders at AUDIO_SAMPLE_RATE / 2 (26633 Hz); the PDM sink wants
 * 32 kHz like every other app (at other rates its DAC-mode clocks, derived
 * from rate / 100, misbehave), so the frame is resampled before submitting. */
#define OUTPUT_RATE 32000

extern unsigned char* VRAM;
extern int zclk;
int system_clock;
int scan_line;

int16_t gwenesis_sn76489_buffer[AUDIO_BUFFER_LENGTH];
int sn76489_index;
int sn76489_clock;
int16_t gwenesis_ym2612_buffer[AUDIO_BUFFER_LENGTH];
int ym2612_index;
int ym2612_clock;

static FILE *savestate_fp = NULL;
static int savestate_errors = 0;

static bool yfm_enabled = true;
static bool z80_enabled = true;
static bool sn76489_enabled = true;

static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_app_t *app;

static const char *SETTING_YFM_EMULATION = "yfm_enable";
static const char *SETTING_Z80_EMULATION = "z80_enable";
static const char *SETTING_SN76489_EMULATION = "sn_enable";
// --- MAIN

typedef struct {
    char key[28];
    uint32_t length;
} svar_t;

SaveState* saveGwenesisStateOpenForRead(const char* fileName)
{
    return (void*)1;
}

SaveState* saveGwenesisStateOpenForWrite(const char* fileName)
{
    return (void*)1;
}

int saveGwenesisStateGet(SaveState* state, const char* tagName)
{
    int value = 0;
    saveGwenesisStateGetBuffer(state, tagName, &value, sizeof(int));
    return value;
}

void saveGwenesisStateSet(SaveState* state, const char* tagName, int value)
{
    saveGwenesisStateSetBuffer(state, tagName, &value, sizeof(int));
}

void saveGwenesisStateGetBuffer(SaveState* state, const char* tagName, void* buffer, int length)
{
    size_t initial_pos = ftell(savestate_fp);
    bool from_start = false;
    svar_t var;

    // Odds are that calls to this func will be in order, so try searching from current file position.
    while (!from_start || ftell(savestate_fp) < initial_pos)
    {
        if (!fread(&var, sizeof(svar_t), 1, savestate_fp))
        {
            if (!from_start)
            {
                fseek(savestate_fp, 0, SEEK_SET);
                from_start = true;
                continue;
            }
            break;
        }
        if (strncmp(var.key, tagName, sizeof(var.key)) == 0)
        {
            fread(buffer, RG_MIN(var.length, length), 1, savestate_fp);
            RG_LOGI("Loaded key '%s'\n", tagName);
            return;
        }
        fseek(savestate_fp, var.length, SEEK_CUR);
    }
    RG_LOGW("Key %s NOT FOUND!\n", tagName);
    savestate_errors++;
}

void saveGwenesisStateSetBuffer(SaveState* state, const char* tagName, void* buffer, int length)
{
    // TO DO: seek the file to find if the key already exists. It's possible it could be written twice.
    svar_t var = {{0}, length};
    strncpy(var.key, tagName, sizeof(var.key) - 1);
    fwrite(&var, sizeof(var), 1, savestate_fp);
    fwrite(buffer, length, 1, savestate_fp);
    RG_LOGI("Saved key '%s'\n", tagName);
}

void gwenesis_io_get_buttons()
{
}


static rg_gui_event_t yfm_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        yfm_enabled = !yfm_enabled;
        rg_settings_set_number(NS_APP, SETTING_YFM_EMULATION, yfm_enabled);
    }
    strcpy(option->value, yfm_enabled ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t sn76489_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        sn76489_enabled = !sn76489_enabled;
        rg_settings_set_number(NS_APP, SETTING_SN76489_EMULATION, sn76489_enabled);
    }
    strcpy(option->value, sn76489_enabled ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t z80_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        z80_enabled = !z80_enabled;
        rg_settings_set_number(NS_APP, SETTING_Z80_EMULATION, z80_enabled);
    }
    strcpy(option->value, z80_enabled ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    if ((savestate_fp = fopen(filename, "wb")))
    {
        savestate_errors = 0;
        gwenesis_save_state();
        fclose(savestate_fp);
        return savestate_errors == 0;
    }
    return false;
}

static bool load_state_handler(const char *filename)
{
    ym2612_worker_flush();
    if ((savestate_fp = fopen(filename, "rb")))
    {
        savestate_errors = 0;
        gwenesis_load_state();
        fclose(savestate_fp);
        if (savestate_errors == 0)
            return true;
    }
    reset_emulation();
    return false;
}

static bool reset_handler(bool hard)
{
    reset_emulation();
    return true;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(currentUpdate, 0);
    }
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("YM2612 audio "), "-", RG_DIALOG_FLAG_NORMAL, &yfm_update_cb};
    *dest++ = (rg_gui_option_t){0, _("SN76489 audio"), "-", RG_DIALOG_FLAG_NORMAL, &sn76489_update_cb};
    *dest++ = (rg_gui_option_t){0, _("Z80 emulation"), "-", RG_DIALOG_FLAG_NORMAL, &z80_update_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

/* Linear resampling of one frame of YM2612 output (26633 Hz) to OUTPUT_RATE.
 * The phase and the last input sample carry over, so frame edges are smooth. */
static void submit_resampled(const rg_audio_frame_t *in, int count)
{
    static rg_audio_frame_t out[AUDIO_BUFFER_LENGTH];
    static rg_audio_frame_t last;
    static int32_t phase = -65536; /* 16.16, position in "in"; -1 = last */
    const int32_t step = (int32_t)(((int64_t)(AUDIO_SAMPLE_RATE / 2) << 16) / OUTPUT_RATE);
    int n = 0;

    while ((phase >> 16) + 1 <= count - 1 && n < (int)RG_COUNT(out))
    {
        int i = phase >> 16;
        int32_t frac = phase & 0xFFFF;
        const rg_audio_frame_t *a = i < 0 ? &last : &in[i];
        const rg_audio_frame_t *b = &in[i + 1];
        /* 15-bit fraction: a 16-bit difference times it stays within int32 */
        out[n].left = a->left + (((b->left - a->left) * (frac >> 1)) >> 15);
        out[n].right = a->right + (((b->right - a->right) * (frac >> 1)) >> 15);
        n++;
        phase += step;
    }
    phase -= count << 16;
    last = in[count - 1];
    rg_audio_submit(out, n);
}

void app_main(void)
{
    const rg_handlers_t handlers = {
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
        .options = &options_handler,
    };

    app = rg_system_init(OUTPUT_RATE, &handlers, NULL);

    yfm_enabled = rg_settings_get_number(NS_APP, SETTING_YFM_EMULATION, 1);
    sn76489_enabled = rg_settings_get_number(NS_APP, SETTING_SN76489_EMULATION, 0);
    z80_enabled = rg_settings_get_number(NS_APP, SETTING_Z80_EMULATION, 1);

    updates[0] = rg_surface_create(320, 241, RG_PIXEL_PAL565_BE, MEM_FAST);
    // updates[1] = rg_surface_create(320, 241, RG_PIXEL_PAL565_BE, MEM_FAST);
    currentUpdate = updates[0];

    // This is a hack because our new surface format doesn't yet support overdraw space easily
    updates[0]->data += 160;
    updates[0]->height = 240;
    // updates[1]->data += 160;
    // updates[1]->height = 240;

    VRAM = rg_alloc(VRAM_MAX_SIZE, MEM_FAST);

    RG_LOGI("Genesis start\n");

    size_t rom_size;
    void *rom_data;

    if (rg_extension_match(app->romPath, "zip"))
    {
        if (!rg_storage_unzip_file(app->romPath, NULL, &rom_data, &rom_size, RG_FILE_ALIGN_64KB))
            RG_PANIC("ROM file unzipping failed!");
    }
    else if (!rg_storage_read_file(app->romPath, &rom_data, &rom_size, RG_FILE_ALIGN_64KB))
    {
        RG_PANIC("ROM load failed!");
    }

    RG_LOGI("load_cartridge(%p, %d)\n", rom_data, rom_size);
    load_cartridge(rom_data, rom_size);
    // free(rom_data); // load_cartridge takes ownership

    RG_LOGI("power_on()\n");
    power_on();

    RG_LOGI("reset_emulation()\n");
    reset_emulation();
    ym2612_worker_start(); /* FM synthesis on core 1 (components/gwenesis/src/sound/ym2612.c) */

    if (app->bootFlags & RG_BOOT_RESUME)
    {
        rg_emu_load_state(app->saveSlot);
    }

    rg_system_set_tick_rate(60);
    app->frameskip = 3;

    extern unsigned char gwenesis_vdp_regs[0x20];
    extern unsigned int gwenesis_vdp_status;
    extern unsigned short CRAM565[256];
    extern unsigned int screen_width, screen_height;
    extern int hint_pending;

    uint32_t keymap[8] = {RG_KEY_UP, RG_KEY_DOWN, RG_KEY_LEFT, RG_KEY_RIGHT, RG_KEY_A, RG_KEY_B, RG_KEY_SELECT, RG_KEY_START};
    uint32_t joystick = 0, joystick_old;

    int skipFrames = 0;

    RG_LOGI("emulation loop\n");
    while (true)
    {
        joystick_old = joystick;
        joystick = rg_input_read_gamepad();

        if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
        {
            if (joystick & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
        }
        else if (joystick != joystick_old)
        {
            for (int i = 0; i < 8; i++)
            {
                if ((joystick & keymap[i]) == keymap[i])
                    gwenesis_io_pad_press_button(0, i);
                else
                    gwenesis_io_pad_release_button(0, i);
            }
        }

        int64_t startTime = rg_system_timer();
        bool drawFrame = skipFrames == 0;
        bool slowFrame = false;

        int lines_per_frame = REG1_PAL ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
        int hint_counter = gwenesis_vdp_regs[10];

        screen_width = REG12_MODE_H40 ? 320 : 256;
        screen_height = REG1_PAL ? 240 : 224;

        gwenesis_vdp_set_buffer(currentUpdate->data);
        gwenesis_vdp_render_config();

        /* Reset the difference clocks and audio index */
        system_clock = 0;
        zclk = z80_enabled ? 0 : 0x1000000;

        /* ym2612_clock / ym2612_index belong to the core-1 worker now */

        sn76489_clock = sn76489_enabled ? 0 : 0x1000000;
        sn76489_index = 0;

        scan_line = 0;

        while (scan_line < lines_per_frame)
        {
            GEN_PROF_T0(t0);
            m68k_run(system_clock + VDP_CYCLES_PER_LINE);
            GEN_PROF_ACC(m68k, t0);
            z80_run(system_clock + VDP_CYCLES_PER_LINE);
            GEN_PROF_ACC(z80, t0);

            /* Audio */
            /*  GWENESIS_AUDIO_ACCURATE:
            *    =1 : cycle accurate mode. audio is refreshed when CPUs are performing a R/W access
            *    =0 : line  accurate mode. audio is refreshed every lines.
            */
            if (GWENESIS_AUDIO_ACCURATE == 0) {
                gwenesis_SN76489_run(system_clock + VDP_CYCLES_PER_LINE);
                GEN_PROF_ACC(sn, t0);
                ym2612_run(system_clock + VDP_CYCLES_PER_LINE);
                GEN_PROF_ACC(ym, t0);
            }

            /* Video */
            if (drawFrame && scan_line < screen_height)
                gwenesis_vdp_render_line(scan_line); /* render scan_line */
            GEN_PROF_ACC(vdp, t0);

            // On these lines, the line counter interrupt is reloaded
            if ((scan_line == 0) || (scan_line > screen_height)) {
                //  if (REG0_LINE_INTERRUPT != 0)
                //    printf("HINTERRUPT counter reloaded: (scan_line: %d, new
                //    counter: %d)\n", scan_line, REG10_LINE_COUNTER);
                hint_counter = REG10_LINE_COUNTER;
            }

            // interrupt line counter
            if (--hint_counter < 0) {
                if ((REG0_LINE_INTERRUPT != 0) && (scan_line <= screen_height)) {
                    hint_pending = 1;
                    // printf("Line int pending %d\n",scan_line);
                    if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
                    m68k_update_irq(4);
                }
                hint_counter = REG10_LINE_COUNTER;
            }

            scan_line++;

            // vblank begin at the end of last rendered line
            if (scan_line == screen_height) {
                if (REG1_VBLANK_INTERRUPT != 0) {
                    gwenesis_vdp_status |= STATUS_VIRQPENDING;
                    m68k_set_irq(6);
                }
                z80_irq_line(1);
            }
            if (scan_line == (screen_height + 1)) {
                z80_irq_line(0);
            }

            system_clock += VDP_CYCLES_PER_LINE;
        }

        /* Audio
        * synchronize YM2612 and SN76489 to system_clock
        * it completes the missing audio sample for accurate audio mode
        */
        int16_t *ym_frame = NULL;
        if (GWENESIS_AUDIO_ACCURATE == 1) {
            gwenesis_SN76489_run(system_clock);
            ym_frame = ym2612_frame_end(system_clock, yfm_enabled); /* previous frame's audio */
        }

        // reset m68k cycles to the begin of next frame cycle
        m68k.cycles -= system_clock;

        if (drawFrame)
        {
            for (int i = 0; i < 256; ++i)
                currentUpdate->palette[i] = (CRAM565[i] << 8) | (CRAM565[i] >> 8);
            slowFrame = !rg_display_sync(false);
            currentUpdate->width = screen_width;
            currentUpdate->height = screen_height;
            rg_display_submit(currentUpdate, 0);
        }

        rg_system_tick(rg_system_timer() - startTime);
#if GEN_PROF
        {
            static int64_t last_print = 0;
            static int frames = 0, drawn = 0;
            frames++; drawn += drawFrame;
            gen_prof.frame += rg_system_timer() - startTime;
            if (rg_system_timer() - last_print >= 1000000)
            {
                int n = frames ? frames : 1;
                extern int64_t gen_prof_ym_us; extern int gen_prof_ym_calls, gen_prof_ym_samples;
                RG_LOGI("GENPROF n=%d drawn=%d | us/frame: frame=%d m68k=%d z80=%d ym2612=%d sn76489=%d vdp=%d (vdp per drawn=%d) | YM2612Update inside CPUs: %d us, %d calls, %d samples per frame\n",
                        frames, drawn, (int)(gen_prof.frame / n), (int)(gen_prof.m68k / n), (int)(gen_prof.z80 / n),
                        (int)(gen_prof.ym / n), (int)(gen_prof.sn / n), (int)(gen_prof.vdp / n),
                        drawn ? (int)(gen_prof.vdp / drawn) : 0,
                        (int)(gen_prof_ym_us / n), gen_prof_ym_calls / n, gen_prof_ym_samples / n);
                char hud[96];
                snprintf(hud, sizeof(hud), "68K%5.1f\nZ80%5.1f\nVDP%5.1f\nYM %5.1f", gen_prof.m68k / 1000.0f / n,
                         gen_prof.z80 / 1000.0f / n, drawn ? gen_prof.vdp / 1000.0f / drawn : 0.0f, gen_prof_ym_us / 1000.0f / n);
                rg_system_set_hud_text(hud);
                gen_prof_ym_us = 0; gen_prof_ym_calls = gen_prof_ym_samples = 0;
                memset(&gen_prof, 0, sizeof(gen_prof));
                frames = drawn = 0;
                last_print = rg_system_timer();
            }
        }
#endif

        if (yfm_enabled || z80_enabled) {
            // TODO: Mix in gwenesis_sn76489_buffer
            submit_resampled((const rg_audio_frame_t *)(ym_frame ? ym_frame : gwenesis_ym2612_buffer), AUDIO_BUFFER_LENGTH >> 1);
        }

        if (skipFrames == 0)
        {
            int elapsed = rg_system_timer() - startTime;
            if (app->frameskip > 0)
                skipFrames = app->frameskip;
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
