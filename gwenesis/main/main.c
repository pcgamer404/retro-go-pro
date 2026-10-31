#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>

#include <gwenesis.h>
#include <xtensa/hal.h>

#define AUDIO_SAMPLE_RATE_NTSC (53267)
#define AUDIO_SAMPLE_RATE_PAL  (52781)
/* Large enough for a PAL frame (about 1061 mono samples per frame). */
#define AUDIO_BUFFER_LENGTH 1088
#define OUTPUT_RATE 32000

#if RG_SCREEN_PIXEL_FORMAT == 0
#define FB_PIXEL_FORMAT RG_PIXEL_PAL565_BE
#else
#define FB_PIXEL_FORMAT RG_PIXEL_PAL565_LE
#endif

extern unsigned char* VRAM;
extern unsigned char gwenesis_vdp_regs[];
extern int zclk;
extern unsigned short gwenesis_vdp_status;
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
static bool sound_enabled = true;
static bool swap_ac = false;
static int audio_sample_rate = AUDIO_SAMPLE_RATE_NTSC;

/* Keep VRAM in internal SRAM: the 68K DMA and the VDP renderer hammer it. */
static unsigned char vram_internal[VRAM_MAX_SIZE] __attribute__((aligned(16)));

/* The display task runs on the other core and converts the surface it was
   handed into SPI buffers over the next few milliseconds.  Rendering the next
   frame into that same surface lets the two cores race over the same bytes:
   lines get converted half-old/half-new, their checksums come out wrong, and
   the panel keeps a stale line here and there.  That is visible as horizontal
   tearing and as horizontal streaks that shimmer across dithered areas.
   Three buffers are rotated instead (one being displayed, one queued, one being
   rendered) so a submitted frame is never overwritten while it is transferred.
   They live in PSRAM because a 320x240 PAL8 frame does not fit in the ~38 KB of
   internal RAM that is left. */
#define NUM_DISPLAY_BUFFERS 3

static rg_surface_t *updates[NUM_DISPLAY_BUFFERS];
static rg_surface_t *currentUpdate; /* being rendered by the VDP */
static rg_surface_t *displaying;    /* handed to the display task, never draw into it */
static rg_surface_t *pending;       /* finished frame waiting for the display task */
static int renderIndex;             /* next buffer to render into */
static rg_app_t *app;

/* Defined by the VDP, updated by the main loop before every frame. */
extern int screen_width, screen_height;

static void display_poll(void)
{
    if (pending && !rg_display_is_busy())
    {
        displaying = pending;
        pending = NULL;
        rg_display_submit(displaying, 0);
    }
}

/* Pick the next buffer that neither the display task nor the pending frame
   uses.  Rotating in order (rather than always taking the first free one) keeps
   a buffer out of use for NUM_DISPLAY_BUFFERS frames, which is what gives the
   display task time to finish with it. */
static rg_surface_t *acquire_render_buffer(void)
{
    for (int n = 0; n < NUM_DISPLAY_BUFFERS; ++n)
    {
        int i = (renderIndex + n) % NUM_DISPLAY_BUFFERS;
        if (updates[i] != displaying && updates[i] != pending)
        {
            renderIndex = (i + 1) % NUM_DISPLAY_BUFFERS;
            return updates[i];
        }
    }

    /* The display is two frames behind: the queued frame is already stale. */
    pending = NULL;
    for (int n = 0; n < NUM_DISPLAY_BUFFERS; ++n)
    {
        int i = (renderIndex + n) % NUM_DISPLAY_BUFFERS;
        if (updates[i] != displaying)
        {
            renderIndex = (i + 1) % NUM_DISPLAY_BUFFERS;
            return updates[i];
        }
    }

    return updates[0];
}

static const char *SETTING_SOUND = "sound_enable";
static const char *SETTING_SWAP_AC = "swap_ac";

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


static rg_gui_event_t sound_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        sound_enabled = !sound_enabled;
        yfm_enabled = sound_enabled;
        sn76489_enabled = sound_enabled;
        z80_enabled = sound_enabled;
        rg_settings_set_number(NS_APP, SETTING_SOUND, sound_enabled);
        memset(gwenesis_sn76489_buffer, 0, sizeof(gwenesis_sn76489_buffer));
        memset(gwenesis_ym2612_buffer, 0, sizeof(gwenesis_ym2612_buffer));
    }
    strcpy(option->value, sound_enabled ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t swap_ac_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        swap_ac = !swap_ac;
        rg_settings_set_number(NS_APP, SETTING_SWAP_AC, swap_ac);
    }
    strcpy(option->value, swap_ac ? _("On") : _("Off"));

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
        {
            /* Older save states may not carry the console region bit. */
            if (gwenesis_region_pal)
                gwenesis_vdp_status |= STATUS_PAL;
            else
                gwenesis_vdp_status &= ~STATUS_PAL;
            return true;
        }
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
        /* The GUI asked for the current frame again; emulation is paused here,
           so the buffer being rendered into holds a complete frame. */
        pending = NULL;
        displaying = currentUpdate;
        rg_display_submit(currentUpdate, 0);
    }
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Sound"), "-", RG_DIALOG_FLAG_NORMAL, &sound_update_cb};
    *dest++ = (rg_gui_option_t){0, _("Swap A/C"), "-", RG_DIALOG_FLAG_NORMAL, &swap_ac_update_cb};

    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

/* --- Scanline renderer on core 1 -----------------------------------------
   The renderer costs 11-14 ms per drawn frame, the 68000+Z80 13-18 ms.  Run
   back to back they cost the sum, which is why frames have to be skipped.
   The renderer only needs line N's emulated state, so core 0 publishes "line
   N is emulated" and core 1 draws it while core 0 emulates line N+1.  The
   only coupling is the per-line handshake below.

   Core 1 also runs the display task (priority 6) and the YM2612 worker
   (priority 5), so the renderer sits at priority 5: the display and input
   tasks still preempt it.  It spins between lines (there is nothing else for
   it to do) and sleeps between frames.

   Known approximation: core 1 may observe a VRAM/register write issued
   during line N+1 while drawing line N, i.e. up to one line early.  The
   renderer already draws each line after that line's CPU work, so this is the
   same order of approximation, one line later.

   If the worker cannot keep up or dies, the frame loop notices and falls back
   to rendering inline for the rest of the session. */
#define VDP_CORE1 1

#if VDP_CORE1 && defined(ESP_PLATFORM)
#define VDP_PIPELINE 1
#else
#define VDP_PIPELINE 0
#endif

#if VDP_PIPELINE
static rg_task_t *vdp_task;
static volatile int vdp_frame_active;  /* a drawn frame is in flight */
static volatile int vdp_lines_ready;   /* lines whose CPU work is done */
static volatile int vdp_lines_done;    /* lines the worker has drawn */
static volatile int vdp_lines_total;   /* lines to draw this frame */
static volatile uint32_t vdp_work_cycles; /* render cycles, for the PERF line */
static volatile int vdp_pipeline_ok = 1;
static volatile int vdp_worker_idle = 1;

/* Runs on core 1.  Deliberately uses only rg_task_* so that this file never
   has to include the FreeRTOS headers, whose one-argument BIT() macro clashes
   with the two-argument BIT() of gwenesis_vdp.h.

   The worker blocks on its task queue between frames, so the frame-start
   message wakes it immediately.  Polling with rg_task_delay(1) instead cost
   up to two ticks of start-up lag per frame, and because the renderer is the
   slower of the two sides that lag was never recovered before frame end. */
static void vdp_render_worker(void *arg)
{
    rg_task_msg_t msg;
    (void)arg;

    while (rg_task_receive(&msg, -1))
    {
        if (msg.type == RG_TASK_MSG_STOP)
            break;

        vdp_worker_idle = 0;
        while (vdp_pipeline_ok && vdp_frame_active)
        {
            int done = vdp_lines_done;
            if (done >= vdp_lines_total)
                break;
            if (done < vdp_lines_ready)
            {
                uint32_t c0 = xthal_get_ccount();
                gwenesis_vdp_render_line(done);
                vdp_work_cycles += xthal_get_ccount() - c0;
                __sync_synchronize();
                vdp_lines_done = done + 1;
            }
            /* else: core 0 is still executing this line; spin briefly.  This is
               the whole point of the worker, so a busy wait is correct here. */
        }
        vdp_worker_idle = 1;
    }
}

static bool vdp_pipeline_start(void)
{
    /* Priority 4: below the display task (6) and the YM2612 worker (5), so
       neither is delayed by the renderer.  At 5 the two would round-robin a
       whole tick away from each other every time a YM job was queued. */
    vdp_task = rg_task_create("vdp_render", &vdp_render_worker, NULL, 4 * 1024, 1,
                              RG_TASK_PRIORITY_4, 1);
    if (!vdp_task)
    {
        vdp_pipeline_ok = 0;
        RG_LOGW("VDP renderer: core 1 task failed, rendering inline");
        return false;
    }
    RG_LOGI("VDP scanline renderer on core 1");
    return true;
}

/* Ask the worker to start on the frame main has just prepared. */
static bool vdp_pipeline_kick(void)
{
    rg_task_msg_t msg = { .type = 1 };
    if (!rg_task_send(vdp_task, &msg, 0))
    {
        RG_LOGW("VDP renderer: worker not listening, rendering inline");
        vdp_pipeline_ok = 0;
        vdp_frame_active = 0;
        return false;
    }
    return true;
}

/* Returns true once every line of the frame has been drawn by the worker. */
static bool vdp_pipeline_wait(void)
{
    /* Bounded: if the worker ever stalls, give up on it rather than hang. */
    uint32_t start = xthal_get_ccount();
    while (vdp_lines_done < vdp_lines_total)
    {
        if ((uint32_t)(xthal_get_ccount() - start) > 24000000u) /* 100 ms */
        {
            RG_LOGE("VDP renderer: worker stalled, falling back to inline");
            vdp_pipeline_ok = 0;
            vdp_frame_active = 0;
            /* Wait for it to leave the renderer before we touch the buffer. */
            start = xthal_get_ccount();
            while (!vdp_worker_idle &&
                   (uint32_t)(xthal_get_ccount() - start) < 2400000u)
                rg_task_yield();
            return false;
        }
        /* The worker is on the other core; yielding here only lets this core's
           idle task run, and keeps the watchdog happy. */
        rg_task_yield();
    }
    vdp_frame_active = 0;
    return true;
}
#endif /* VDP_PIPELINE */

/* Linear resampling of one frame of mono YM2612/PSG output to OUTPUT_RATE (phase carries over). */
static void submit_resampled(const int16_t *in, int count)
{
    static rg_audio_frame_t out[AUDIO_BUFFER_LENGTH];
    static int16_t last;
    static int32_t phase = -65536; /* 16.16, position in "in"; -1 = last */
    const int32_t step = (int32_t)(((int64_t)audio_sample_rate << 16) / OUTPUT_RATE);
    int n = 0;

    while ((phase >> 16) + 1 <= count - 1 && n < (int)RG_COUNT(out))
    {
        int i = phase >> 16;
        int32_t frac = phase & 0xFFFF;
        int16_t a = i < 0 ? last : in[i];
        int16_t b = in[i + 1];
        int32_t sample = a + (((int32_t)(b - a) * (frac >> 1)) >> 15);
        out[n].left = out[n].right = (int16_t)sample;
        n++;
        phase += step;
    }
    phase -= count << 16;
    if (count > 0)
        last = in[count - 1];
    if (n)
        rg_audio_submit(out, n);
}

void app_main(void)
{
    const rg_config_t config = {
        .sampleRate = OUTPUT_RATE,
        .frameRate = 60,
        .storageRequired = true,
        .romRequired = true,
        .handlers.loadState = &load_state_handler,
        .handlers.saveState = &save_state_handler,
        .handlers.reset = &reset_handler,
        .handlers.screenshot = &screenshot_handler,
        .handlers.event = &event_handler,
        .handlers.options = &options_handler,
    };
    app = rg_system_init(&config);
    /* -1 disables the system monitor's auto frameskip.  The audio pacing wait
       counts as "busy" time, so BUSY reads 80-99% even when the CPU is idle and
       the monitor would push frameskip all the way to 5 (about 9 drawn FPS).
       We do our own adaptive skip at the end of the loop instead. */
    app->frameskip = -1;

    sound_enabled = rg_settings_get_number(NS_APP, SETTING_SOUND, 1);
    yfm_enabled = sn76489_enabled = z80_enabled = sound_enabled;
    swap_ac = rg_settings_get_number(NS_APP, SETTING_SWAP_AC, 0);

    /* Three rotating video buffers (see the comment at the top). */
    for (int i = 0; i < NUM_DISPLAY_BUFFERS; ++i)
    {
        updates[i] = rg_surface_create(320, 241, FB_PIXEL_FORMAT, MEM_SLOW);
        if (!updates[i])
            RG_PANIC("Could not allocate a video buffer.");
        /* This is a hack because our new surface format doesn't yet support
           overdraw space easily. */
        updates[i]->data += 160;
        updates[i]->height = 240;
    }
    currentUpdate = updates[0];

    VRAM = vram_internal;

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

    /* PAL games run at 50 Hz and produce more audio samples per frame. */
    rg_system_set_tick_rate(gwenesis_region_pal ? GWENESIS_REFRESH_RATE_PAL : GWENESIS_REFRESH_RATE_NTSC);
    audio_sample_rate = gwenesis_region_pal ? AUDIO_SAMPLE_RATE_PAL : AUDIO_SAMPLE_RATE_NTSC;

    ym2612_worker_start(); /* FM synthesis on core 1 */
#if VDP_PIPELINE
    vdp_pipeline_start();  /* scanline rendering on core 1 */
#endif

    if (app->bootFlags & RG_BOOT_RESUME)
    {
        rg_emu_load_state(app->saveSlot);
    }

    extern unsigned char gwenesis_vdp_regs[0x20];
    extern unsigned short CRAM565[256];
    extern int screen_width, screen_height;
    extern int hint_pending;

#if defined(RG_TARGET_GB300_P4)
    uint32_t keymap[8] = {RG_KEY_UP, RG_KEY_DOWN, RG_KEY_LEFT, RG_KEY_RIGHT, RG_KEY_B, RG_KEY_A, RG_KEY_Y, RG_KEY_START};
#elif defined(RG_TARGET_MY_HANDHELD)
    /* A/B/X = Genesis A/B/C (Y is handled as an alternate C below). */
    uint32_t keymap[8] = {RG_KEY_UP, RG_KEY_DOWN, RG_KEY_LEFT, RG_KEY_RIGHT, RG_KEY_B, RG_KEY_X, RG_KEY_A, RG_KEY_START};
#else
    uint32_t keymap[8] = {RG_KEY_UP, RG_KEY_DOWN, RG_KEY_LEFT, RG_KEY_RIGHT, RG_KEY_A, RG_KEY_B, RG_KEY_SELECT, RG_KEY_START};
#endif
    uint32_t joystick_old = -1;

    int skipFrames = 0;

    /* Per-frame profiling (averaged over 120 frames) */
    int64_t prof_emu = 0, prof_vdp = 0, prof_audio = 0, prof_core = 0, prof_wait = 0;
    int64_t prof_mix = 0, prof_disp = 0, prof_tick = 0, prof_total = 0;
    uint64_t prof_68k_cycles = 0, prof_z80_cycles = 0;
    int prof_frames = 0;
    int prof_drawn = 0;

    RG_LOGI("emulation loop");
    while (true)
    {
        const int64_t startTime = rg_system_timer();
        uint32_t joystick = rg_input_read_gamepad();
        bool drawFrame = skipFrames == 0;
        bool slowFrame = false;

        if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
        {
            if (joystick & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
            continue;
        }

        if (joystick != joystick_old)
        {
            for (int i = 0; i < 8; i++)
            {
                int button = i;
                /* Genesis A is bit 6, C is bit 5. */
                if (swap_ac)
                {
                    if (i == 5) button = 6;
                    else if (i == 6) button = 5;
                }
                if ((joystick & keymap[i]) == keymap[i])
                    gwenesis_io_pad_press_button(0, button);
                else
                    gwenesis_io_pad_release_button(0, button);
            }
#if defined(RG_TARGET_MY_HANDHELD)
            /* Y is an alternate Genesis C button, or A when A/C is swapped. */
            if (joystick & RG_KEY_Y)
                gwenesis_io_pad_press_button(0, swap_ac ? 6 : 5);
#endif
            joystick_old = joystick;
        }

        int lines_per_frame = gwenesis_region_pal ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
        int hint_counter = gwenesis_vdp_regs[10];

        screen_width = REG12_MODE_H40 ? 320 : 256;
        screen_height = REG1_PAL ? 240 : 224;

        currentUpdate = acquire_render_buffer();
        gwenesis_vdp_set_buffer(currentUpdate->data);
        gwenesis_vdp_render_config();

#if VDP_PIPELINE
        /* Hand this frame's lines to the renderer on core 1. */
        bool pipelined = drawFrame && vdp_pipeline_ok;
        if (pipelined)
        {
            /* Clear "ready" first: the worker must never see the new frame's
               done=0 paired with the previous frame's ready count. */
            vdp_lines_ready = 0;
            vdp_lines_done = 0;
            vdp_lines_total = screen_height;
            vdp_work_cycles = 0;
            __sync_synchronize();
            vdp_frame_active = 1;
            if (!vdp_pipeline_kick())
                pipelined = false;
        }
#endif

        /* Reset the difference clocks and audio index */
        system_clock = 0;
        zclk = z80_enabled ? 0 : 0x1000000;

        sn76489_clock = sn76489_enabled ? 0 : 0x1000000;
        sn76489_index = 0;

        scan_line = 0;

        const int64_t t_emu = rg_system_timer();
        while (scan_line < lines_per_frame)
        {
            uint32_t c0 = xthal_get_ccount();
            m68k_run(system_clock + VDP_CYCLES_PER_LINE);
            uint32_t c1 = xthal_get_ccount();
            z80_run(system_clock + VDP_CYCLES_PER_LINE);
            uint32_t c2 = xthal_get_ccount();
            prof_68k_cycles += (uint32_t)(c1 - c0);
            prof_z80_cycles += (uint32_t)(c2 - c1);

            /* Audio */
            /*  GWENESIS_AUDIO_ACCURATE:
            *    =1 : cycle accurate mode. audio is refreshed when CPUs are performing a R/W access
            *    =0 : line  accurate mode. audio is refreshed every lines.
            */
            if (GWENESIS_AUDIO_ACCURATE == 0) {
                gwenesis_SN76489_run(system_clock + VDP_CYCLES_PER_LINE);
                ym2612_run(system_clock + VDP_CYCLES_PER_LINE);
            }

            /* Video */
            if (drawFrame && scan_line < screen_height)
            {
#if VDP_PIPELINE
                if (pipelined)
                {
                    /* Release this line to the renderer.  Order matters: the
                       CPU work above must be visible before the flag. */
                    __sync_synchronize();
                    vdp_lines_ready = scan_line + 1;
                }
                else
#endif
                {
                    const int64_t t_vdp = rg_system_timer();
                    gwenesis_vdp_render_line(scan_line); /* render scan_line */
                    prof_vdp += rg_system_timer() - t_vdp;
                }
            }

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

        /* Time core 0 spent emulating, before waiting for the renderer. */
        const int64_t emu_core = rg_system_timer() - t_emu;
        prof_core += emu_core;

#if VDP_PIPELINE
        if (pipelined)
        {
            const int64_t t_wait = rg_system_timer();
            if (vdp_pipeline_wait())
            {
                /* Render time measured on core 1 (cycles -> us at 240 MHz). */
                prof_vdp += (int64_t)vdp_work_cycles / 240;
                prof_wait += rg_system_timer() - t_wait;
            }
            else
            {
                prof_wait += rg_system_timer() - t_wait;
                /* Don't show a half-rendered frame; skip it and let the
                   frameskip logic deal with the next one. */
                drawFrame = false;
                slowFrame = true;
            }
        }
#endif

        prof_emu += rg_system_timer() - t_emu;

        /* Audio
        * synchronize YM2612 and SN76489 to system_clock
        * it completes the missing audio sample for accurate audio mode
        */
        const int64_t t_audio = rg_system_timer();
        int audio_samples = system_clock / AUDIO_FREQ_DIVISOR;
        if (audio_samples > AUDIO_BUFFER_LENGTH)
            audio_samples = AUDIO_BUFFER_LENGTH;
        int16_t *ym_frame = NULL;
        if (GWENESIS_AUDIO_ACCURATE == 1) {
            gwenesis_SN76489_run(system_clock);
            ym_frame = ym2612_frame_end(system_clock, yfm_enabled); /* previous frame's audio */
        }
        if (!ym_frame)
            ym_frame = gwenesis_ym2612_buffer;

        // reset m68k cycles to the begin of next frame cycle
        m68k.cycles -= system_clock;
        prof_audio += rg_system_timer() - t_audio;

        const int64_t t_disp = rg_system_timer();
        if (drawFrame)
        {
            prof_drawn++;
            if (FB_PIXEL_FORMAT  == RG_PIXEL_PAL565_BE)
            {
                for (int i = 0; i < 256; ++i)
                    currentUpdate->palette[i] = (CRAM565[i] << 8) | (CRAM565[i] >> 8);
            }
            else
            {
                memcpy(currentUpdate->palette, CRAM565, 512);
            }
            slowFrame = rg_display_is_busy(); // Previous frame is still not done, hence slowFrame...
            currentUpdate->width = screen_width;
            currentUpdate->height = screen_height;
            pending = currentUpdate;
            display_poll();
        }
        else
        {
            /* A skipped frame still gives the display task a chance to pick up
               the frame that is waiting for it. */
            display_poll();
        }
        prof_disp += rg_system_timer() - t_disp;

        const int64_t t_tick = rg_system_timer();
        rg_system_tick(rg_system_timer() - startTime);
        prof_tick += rg_system_timer() - t_tick;

        // Mix in gwenesis_sn76489_buffer
        const int64_t t_mix = rg_system_timer();
        if (sn76489_enabled)
        {
            for (int i = 0; i < audio_samples; i++)
            {
                int32_t sample = ym_frame[i] + gwenesis_sn76489_buffer[i];
                if (sample > 32767) sample = 32767;
                else if (sample < -32768) sample = -32768;
                ym_frame[i] = (int16_t)sample;
            }
        }
        submit_resampled(ym_frame, audio_samples);
        prof_mix += rg_system_timer() - t_mix;
        prof_total += rg_system_timer() - startTime;

        if (++prof_frames >= 120)
        {
            RG_LOGI("PERF emu=%d vdp=%d vdpd=%d core=%d wait=%d cpu68=%d z80=%d audio=%d mix=%d disp=%d tick=%d total=%d draw=%d",
                (int)(prof_emu / prof_frames), (int)(prof_vdp / prof_frames),
                (int)(prof_drawn ? prof_vdp / prof_drawn : 0),
                (int)(prof_core / prof_frames),
                (int)(prof_wait / prof_frames),
                /* Cycles are accumulated per scanline; dividing by 240 lines
                   converts them to microseconds per frame at 240 MHz. */
                (int)(prof_68k_cycles / (prof_frames * 240u)),
                (int)(prof_z80_cycles / (prof_frames * 240u)),
                (int)(prof_audio / prof_frames),
                (int)(prof_mix / prof_frames), (int)(prof_disp / prof_frames),
                (int)(prof_tick / prof_frames), (int)(prof_total / prof_frames),
                (int)prof_drawn);
            prof_emu = prof_vdp = prof_audio = prof_mix = prof_disp = prof_tick = prof_total = prof_core = prof_wait = 0;
            prof_68k_cycles = prof_z80_cycles = 0;
            prof_frames = 0;
            prof_drawn = 0;
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
