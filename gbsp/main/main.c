#include <rg_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../components/gbsp-libretro/common.h"
#include "../components/gbsp-libretro/memmap.h"
#include "../components/gbsp-libretro/sound.h"
#include "../components/gbsp-libretro/gba_memory.h"
#include "../components/gbsp-libretro/gba_cc_lut.h"

/* gba_memory.h exposes backup/eeprom state but not the Flash bank count. */
extern u32 flash_bank_cnt;

#include <esp_heap_caps.h>
#ifdef HAVE_DYNAREC
#include "xjit_exec.h"
#endif

/* ---- Xtensa dynarec glue (xtensa-68000-dynarec, pjcau) -------------------
 * HAVE_DYNAREC / XTENSA_ARCH / SMALL_TRANSLATION_CACHE must be defined for THIS
 * file as well (main/CMakeLists.txt): gbsp_memory_t changes size with them.   */
extern u8 gamepak_backup_dirty;           /* gba_memory.c: set on every backup write */
void gbsp_render_start(void);             /* video.cpp: core-1 line renderer          */
void gbsp_render_wait(void);              /* video.cpp: core 1 has drawn every queued line */
extern int gbsp_render_core1;             /* video.cpp: 1 once the renderer task runs */
void gbsp_display_poll(void) {}           /* video.cpp calls it; v1: nothing to do    */
#ifdef HAVE_DYNAREC
int dynarec_enable = 0;                   /* 1 = translated code, 0 = interpreter     */
extern int xt_give_up;                    /* xtensa_stub.c: SMC storm, use interpreter*/
extern u32 xt_exec_delta;
extern u8 *rom_translation_cache, *ram_translation_cache;
extern u8 *rom_translation_ptr, *ram_translation_ptr;
void gbsp_rvram_alloc(void);              /* video.cpp: renderer's VRAM copy          */
static xj_exec_t jit_mem;
/* [ROM cache][guard][RAM cache][guard]: gpSP's cache-full check runs after each instruction and
   only leaves TRANSLATION_CACHE_LIMIT_THRESHOLD for that instruction plus the block's tail (cold
   code, exits). A block that ends past it used to write into whatever follows the cache - the start
   of the RAM cache (live IWRAM code) or the heap. The guards catch such an overrun (and log it). */
#define JIT_GUARD (16 * 1024)
static u8 *jit_guard[2];
static uint32_t jit_guard_frame;
static bool jit_guard_hit;
static char jit_flag_path[320];           /* "<rom>.jit": 'T' = trying, '0' = off     */
static bool jit_trying, jit_restart;
static int64_t jit_t0;
#endif

#define AUDIO_SAMPLE_RATE (GBA_SOUND_FREQUENCY)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 60 + 1)

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;
boot_mode selected_boot_mode = boot_game;

u32 skip_next_frame = 0;
int sprite_limit = 1;

gbsp_memory_t *gbsp_memory;

#define NUM_UPDATES 3   /* emulation never draws into a buffer the LCD still reads */
static rg_surface_t *updates[NUM_UPDATES];
static rg_surface_t *currentUpdate;   /* being drawn */
static rg_surface_t *shownUpdate;     /* last one submitted (redraw, screenshot) */
static int updateIndex;
static rg_app_t *app;

static const char *SETTING_SOUND_EMULATION = "sound";

/* Cartridge battery-save persistence.
 * gpSP keeps SRAM/Flash/EEPROM contents in gamepak_backup, but the original
 * Retro-Go frontend only implemented emulator save states.  Persist the
 * backup buffer on the SD card so battery saves survive a restart.
 */
#define GP_SAVE_BUF_SIZE (1024 * 128)
#define GP_SAVE_CHECK_MS 3000

static char gp_save_path[320];
static uint32_t gp_save_sum_flushed;
static uint32_t gp_save_sum_prev;
static bool gp_save_prev_differs;
static TickType_t gp_save_last_check;
static bool gp_save_enabled;
static bool gp_save_pending;

static uint32_t gp_save_checksum(void)
{
    const uint32_t *p = (const uint32_t *)gamepak_backup;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < GP_SAVE_BUF_SIZE / 4; ++i)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static void gp_save_make_path(const char *rom_path)
{
    // Keep the battery save beside the ROM on the SD card.  This matches the
    // usual Retro-Go/ROM workflow and avoids relying on a separate save mount.
    snprintf(gp_save_path, sizeof(gp_save_path), "%s", rom_path);
    char *dot = strrchr(gp_save_path, '.');
    if (dot)
        *dot = '\0';
    strncat(gp_save_path, ".sav", sizeof(gp_save_path) - strlen(gp_save_path) - 1);
}

/* loading percentage (gba_memory.c reads the ROM in quarter-blocks). The retro-go function is
   optional: without it the callback does nothing. */
extern void (*gamepak_load_progress)(int percent);
extern void rg_gui_draw_loading(int percent) __attribute__((weak));
static void load_progress_cb(int percent)
{
    if (rg_gui_draw_loading)
        rg_gui_draw_loading(percent);
}

static void gp_save_init(const char *rom_path)
{
    gp_save_make_path(rom_path);
    gp_save_enabled = false;

    FILE *f = fopen(gp_save_path, "rb");
    if (f)
    {
        memset(gamepak_backup, 0xff, GP_SAVE_BUF_SIZE);
        size_t got = fread(gamepak_backup, 1, GP_SAVE_BUF_SIZE, f);
        fclose(f);
        RG_LOGI("gpSP save: loaded %u bytes from %s", (unsigned)got, gp_save_path);
    }
    else
    {
        RG_LOGI("gpSP save: no existing save at %s", gp_save_path);
    }

    gp_save_enabled = true;
    gp_save_sum_flushed = gp_save_checksum();
    gp_save_sum_prev = gp_save_sum_flushed;
    gp_save_prev_differs = false;
    gp_save_last_check = xTaskGetTickCount();
}

static size_t gp_save_size(void)
{
    switch (backup_type)
    {
        case BACKUP_SRAM: return 0x8000;
        case BACKUP_FLASH: return (flash_bank_cnt == FLASH_SIZE_64KB) ? 0x10000 : 0x20000;
        case BACKUP_EEPROM: return (eeprom_size == EEPROM_512_BYTE) ? 0x200 : 0x2000;
        default: return GP_SAVE_BUF_SIZE;
    }
}

static bool gp_save_flush(void)
{
    if (!gp_save_enabled)
        return false;

    char tmp[336];
    snprintf(tmp, sizeof(tmp), "%s.tmp", gp_save_path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
    {
        RG_LOGE("gpSP save: cannot open %s", tmp);
        return false;
    }

    size_t save_size = gp_save_size();
    size_t put = fwrite(gamepak_backup, 1, save_size, f);
    fflush(f);
    fclose(f);
    if (put != save_size)
    {
        remove(tmp);
        RG_LOGE("gpSP save: short write %u/%u", (unsigned)put, (unsigned)save_size);
        return false;
    }

    remove(gp_save_path);
    if (rename(tmp, gp_save_path) != 0)
    {
        remove(tmp);
        RG_LOGE("gpSP save: rename failed for %s", gp_save_path);
        return false;
    }

    gp_save_sum_flushed = gp_save_checksum();
    gp_save_sum_prev = gp_save_sum_flushed;
    gp_save_prev_differs = false;
    gp_save_pending = false;
    RG_LOGI("gpSP save: flushed %u bytes to %s", (unsigned)save_size, gp_save_path);
    return true;
}

static void gp_save_tick(void)
{
    if (!gp_save_enabled)
        return;

    TickType_t now = xTaskGetTickCount();
    if ((now - gp_save_last_check) * portTICK_PERIOD_MS < GP_SAVE_CHECK_MS)
        return;
    gp_save_last_check = now;

    /* gamepak_backup_dirty is set by every backup write: hash only after one */
    if (gamepak_backup_dirty)
    {
        gamepak_backup_dirty = 0;
        gp_save_pending = true;
    }
    if (!gp_save_pending)
        return;

    uint32_t sum = gp_save_checksum();
    bool differs = sum != gp_save_sum_flushed;
    /* Require the checksum to remain unchanged across two checks. This avoids
     * writing while a game is still in the middle of a multi-frame save. */
    if (differs && gp_save_prev_differs && sum == gp_save_sum_prev)
        gp_save_flush();
    else
    {
        gp_save_sum_prev = sum;
        gp_save_prev_differs = differs;
        if (!differs)
            gp_save_pending = false;
    }
}

void netpacket_poll_receive()
{
}

void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(shownUpdate, filename, width, height);
}

/* the state buffer: malloc, or a block lent by the ROM cache (gba_memory.c) */
static void *state_buffer_get(bool *borrowed)
{
    void *buffer = malloc(GBA_STATE_MEM_SIZE);
    *borrowed = false;
    if (!buffer)
    {
        buffer = gamepak_borrow_block();
        *borrowed = buffer != NULL;
    }
    return buffer;
}

static void state_buffer_put(void *buffer, bool borrowed)
{
    if (borrowed)
        gamepak_return_block();
    else
        free(buffer);
}

static bool save_state_handler(const char *filename)
{
    size_t buffer_len = GBA_STATE_MEM_SIZE;
    bool borrowed;
    void *buffer = state_buffer_get(&borrowed);
    if (!buffer)
        return false;
    gba_save_state(buffer);
    bool success = rg_storage_write_file(filename, buffer, buffer_len, 0);
    state_buffer_put(buffer, borrowed);
    return success;
}

static bool load_state_handler(const char *filename)
{
    size_t buffer_len = GBA_STATE_MEM_SIZE;
    bool borrowed;
    void *buffer = state_buffer_get(&borrowed);
    if (!buffer)
        return false;
    bool success = rg_storage_read_file(filename, &buffer, &buffer_len, RG_FILE_USER_BUFFER)
                    && gba_load_state(buffer);
    state_buffer_put(buffer, borrowed);
    gp_save_pending = true;   /* the state carries its own backup contents */
    return success;
}

static bool reset_handler(bool hard)
{
    reset_gba();
#ifdef HAVE_DYNAREC
    if (dynarec_enable)
        flush_dynarec_caches();
#endif
    return true;
}

#ifdef HAVE_DYNAREC
static void jit_flag_write(int c);
#endif

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_SHUTDOWN)
    {
        /* power-off / quit from the launcher: write a pending battery save at once
           (the 3 s x 2 checksum poll in gp_save_tick would lose it) and treat the
           run as clean for the dynarec crash detector */
        if (gp_save_enabled && (gamepak_backup_dirty || gp_save_pending ||
                                gp_save_checksum() != gp_save_sum_flushed))
            gp_save_flush();
#ifdef HAVE_DYNAREC
        if (jit_trying)
        {
            jit_flag_write(0);
            jit_trying = false;
        }
#endif
    }
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(shownUpdate, 0);
    }
}

int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    // RG_LOGI("%u, %u, %u, %u", port, device, index, id);
    uint32_t joystick = rg_input_read_gamepad();
    int16_t val = 0;
    if (joystick & RG_KEY_DOWN) val |= (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (joystick & RG_KEY_UP) val |= (1 << RETRO_DEVICE_ID_JOYPAD_UP);
    if (joystick & RG_KEY_LEFT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (joystick & RG_KEY_RIGHT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (joystick & RG_KEY_START) val |= (1 << RETRO_DEVICE_ID_JOYPAD_START);
    if (joystick & RG_KEY_SELECT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_SELECT);
    /* GBA L/R are libretro JOYPAD_L/R (JOYPAD_X/Y are turbo A/B in gpSP). This handheld's config.h
       has no RG_KEY_L/R, only X and Y buttons: X = L, Y = R (swap the two lines if reversed) */
    if (joystick & (RG_KEY_L | RG_KEY_X)) val |= (1 << RETRO_DEVICE_ID_JOYPAD_L);
    if (joystick & (RG_KEY_R | RG_KEY_Y)) val |= (1 << RETRO_DEVICE_ID_JOYPAD_R);
    if (joystick & RG_KEY_B) val |= (1 << RETRO_DEVICE_ID_JOYPAD_B);
    if (joystick & RG_KEY_A) val |= (1 << RETRO_DEVICE_ID_JOYPAD_A);
    return val;
}

void set_fastforward_override(bool fastforward)
{
}

static rg_gui_event_t sound_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        sound_master_enable = !sound_master_enable;
        rg_settings_set_number(NS_APP, SETTING_SOUND_EMULATION, sound_master_enable);
    }

    strcpy(option->value, sound_master_enable ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

#ifdef HAVE_DYNAREC
/* Per-game engine state in "<rom>.jit" (plain file: survives a crash).
 *   no file : dynarec, never ran
 *   'T'     : dynarec launched and not yet proven (previous run crashed/hung if found at launch)
 *   '0'     : interpreter (user switched it off, crash, or self-modifying-code storm)   */
static int jit_flag_read(void)
{
    FILE *f = fopen(jit_flag_path, "rb");
    int c = f ? fgetc(f) : 0;
    if (f)
        fclose(f);
    return c == EOF ? 0 : c;
}

static void jit_flag_write(int c)
{
    if (!c)
    {
        remove(jit_flag_path);
        return;
    }
    FILE *f = fopen(jit_flag_path, "wb");
    if (f)
    {
        fputc(c, f);
        fclose(f);
    }
}

/* the dynarec has run long enough (or the user reached the menu): not a crash */
static void jit_mark_ok(void)
{
    if (jit_trying && rg_system_timer() - jit_t0 > 120 * 1000000)
    {
        jit_flag_write(0);
        jit_trying = false;
    }
}

static rg_gui_event_t jit_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        if (dynarec_enable)   /* off at once, same state, between frames */
        {
            dynarec_enable = 0;
            jit_trying = false;
            jit_flag_write('0');
        }
        else if (!jit_restart)   /* on from the next start */
        {
            jit_flag_write(0);
            jit_restart = true;
        }
    }
    strcpy(option->value, dynarec_enable ? _("On") : jit_restart ? _("On (restart)") : _("Off"));
    return RG_DIALOG_VOID;
}
#endif

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Audio enable"), "-", RG_DIALOG_FLAG_NORMAL, &sound_toggle_cb};
#ifdef HAVE_DYNAREC
    *dest++ = (rg_gui_option_t){0, _("Fast CPU (dynarec)"), "-", RG_DIALOG_FLAG_NORMAL, &jit_toggle_cb};
#endif
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

#ifdef HAVE_DYNAREC
/* The translator follows every unseen branch target of a block recursively
 * (translate_block_* -> block_lookup_translate_* -> translate_block_*), each level
 * with a 256-byte exit table plus register-window spills, and it runs inside helpers
 * called from translated code. That is deeper than the 8 KB main task stack:
 * a game that reaches new code (Mario Kart after a lap) overflows it. Same fix as the
 * dynarec author's app: the whole emulator on its own task, 20 KB, core 0. */
#define GBSP_TASK_STACK (20 * 1024)
static void gbsp_main(void);
static void gbsp_task(void *arg)
{
    gbsp_main();
}
void app_main(void)
{
    if (xTaskCreatePinnedToCore(gbsp_task, "gbsp", GBSP_TASK_STACK, NULL, uxTaskPriorityGet(NULL), NULL, 0) != pdPASS)
        gbsp_main();   /* no memory for the stack: try on the main task */
    vTaskDelete(NULL);
}
static void gbsp_main(void)
#else
void app_main(void)
#endif
{
    app = rg_system_init(&(const rg_config_t){
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameRate = 60,
        .storageRequired = true,
        .romRequired = true,
        .handlers = {
            .loadState = &load_state_handler,
            .saveState = &save_state_handler,
            .reset = &reset_handler,
            .screenshot = &screenshot_handler,
            .event = &event_handler,
            .options = &options_handler,
        },
    });
    // rg_system_set_overclock(2);

    sound_master_enable = rg_settings_get_number(NS_APP, SETTING_SOUND_EMULATION, true);

    /* MEM_SLOW = PSRAM, explicitly (the dynarec author's app does the same): MEM_ANY
       may be served from internal RAM, 3 x 77 KB of it, and the dynarec has none to spare
       (SD card, task stacks and file buffers need it; running out = random crashes). */
    for (int i = 0; i < NUM_UPDATES; i++)
    {
        updates[i] = rg_surface_create(GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT + 1, RG_PIXEL_565_LE, MEM_SLOW);
        updates[i]->height = GBA_SCREEN_HEIGHT;
    }
    currentUpdate = shownUpdate = updates[0];

    gba_screen_pixels = currentUpdate->data;

    gbsp_memory = rg_alloc(sizeof(*gbsp_memory), MEM_ANY);
    RG_LOGI("gbsp_memory=%p (%u KB)", gbsp_memory, (unsigned)(sizeof(*gbsp_memory) / 1024));

    libretro_supports_bitmasks = true;
    retro_set_input_state(input_cb);

    /* PSRAM is shared out in this order because init_gamepak_buffer() below takes
       everything that is left for the ROM cache:
       renderer buffers -> translation caches -> renderer VRAM copy -> ROM cache  */
    gbsp_render_start();   /* core-1 line renderer (video.cpp); also needed without the dynarec */
    RG_LOGI("line renderer on core 1: %s", gbsp_render_core1 ? "yes" : "no");
#ifdef HAVE_DYNAREC
    {
        snprintf(jit_flag_path, sizeof(jit_flag_path), "%s", app->romPath);
        char *dot = strrchr(jit_flag_path, '.');
        if (dot)
            *dot = '\0';
        strncat(jit_flag_path, ".jit", sizeof(jit_flag_path) - strlen(jit_flag_path) - 1);

        int flag = jit_flag_read();
        if (flag == 'T')   /* launched on the dynarec and never proved itself: crash, hang or power loss */
        {
            RG_LOGW("dynarec: previous run did not finish cleanly, using the interpreter for this game");
            jit_flag_write('0');
            flag = '0';
        }
        if (flag != '0' && xj_exec_alloc_psram(&jit_mem, ROM_TRANSLATION_CACHE_SIZE + JIT_GUARD + RAM_TRANSLATION_CACHE_SIZE + JIT_GUARD))
        {
            rom_translation_cache = jit_mem.data;
            jit_guard[0] = jit_mem.data + ROM_TRANSLATION_CACHE_SIZE;
            ram_translation_cache = jit_guard[0] + JIT_GUARD;
            jit_guard[1] = ram_translation_cache + RAM_TRANSLATION_CACHE_SIZE;
            memset(jit_guard[0], 0, JIT_GUARD);
            memset(jit_guard[1], 0, JIT_GUARD);
            rom_translation_ptr = rom_translation_cache;
            ram_translation_ptr = ram_translation_cache;
            xt_exec_delta = jit_mem.exec - (u32)(uintptr_t)jit_mem.data;
            dynarec_enable = 1;
            jit_flag_write('T');
            jit_trying = true;
            jit_t0 = rg_system_timer();
            RG_LOGI("dynarec: caches %u KB at %p (exec %08x)",
                    (unsigned)((ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE) / 1024),
                    jit_mem.data, (unsigned)jit_mem.exec);
        }
        else
        {
            RG_LOGW("dynarec: off (flag '%c'), interpreter", flag ? flag : '-');
        }
        gbsp_rvram_alloc();   /* the renderer's VRAM copy: after the caches, before the ROM cache */
    }
#endif
    init_gamepak_buffer();
    init_sound();
    // load_bios(RG_BASE_PATH_BIOS "/gba_bios.bin");

    memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
    gamepak_load_progress = load_progress_cb;
    if (load_gamepak(NULL, app->romPath, FEAT_DISABLE, FEAT_DISABLE, SERIAL_MODE_DISABLED) != 0)
    {
        RG_PANIC("Could not load the game file.");
    }
    gamepak_load_progress = NULL;

    gp_save_init(app->romPath);

    RG_LOGI("reset_gba");
    reset_gba();

    if (app->bootFlags & RG_BOOT_RESUME)
    {
        RG_LOGI("load_state");
        rg_emu_load_state(app->saveSlot);
    }

    RG_LOGI("free after setup: internal %u KB (keep > 20 KB: the SD card needs it), PSRAM %u KB",
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    RG_LOGI("emulation loop");

    rg_audio_sample_t mixbuffer[AUDIO_BUFFER_LENGTH] = {0};

    while (true)
    {
        // RG_TIMER_INIT();
        const int64_t startTime = rg_system_timer();
        uint32_t joystick = rg_input_read_gamepad();

        if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
        {
#ifdef HAVE_DYNAREC
            jit_mark_ok();
#endif
            gp_save_flush();
            if (joystick & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
            memset(&mixbuffer, 0, sizeof(mixbuffer));
            continue;
        }

        update_input();
        rumble_frame_reset();
        clear_gamepak_stickybits();
#ifdef HAVE_DYNAREC
        if (dynarec_enable)
        {
            execute_arm_translate(execute_cycles);
            if (xt_give_up)   /* self-modifying-code storm: the interpreter from the next frame on */
            {
                RG_LOGW("dynarec: giving up for this game (code rewritten continuously)");
                dynarec_enable = 0;
                jit_trying = false;
                jit_flag_write('0');
            }
            else
                jit_mark_ok();
        }
        else
#endif
            execute_arm(execute_cycles);
        // RG_TIMER_LAP("execute_arm");

        if (!skip_next_frame)
        {
            /* The core only syncs with core 1 before video-memory writes. The last lines of
               the frame (and more, if core 1 was starved) are still queued here: without this
               wait they are drawn into whatever gba_screen_pixels points at *then* (the next
               buffer), while the buffer being submitted is still written by core 1 and read
               by the display, and save states / reset / the menu run with lines outstanding.
               Same call as in the dynarec author's frame loop. */
            gbsp_render_wait();
            rg_display_submit(currentUpdate, 0);
            shownUpdate = currentUpdate;
            updateIndex = (updateIndex + 1) % NUM_UPDATES;
            currentUpdate = updates[updateIndex];
            gba_screen_pixels = currentUpdate->data;
        }

        gp_save_tick();
#ifdef HAVE_DYNAREC
        if (dynarec_enable && !jit_guard_hit && ++jit_guard_frame % 30 == 0)
        {
            for (int g = 0; g < 2; g++)
            {
                const uint32_t *w = (const uint32_t *)jit_guard[g];
                uint32_t acc = 0;
                for (int i = 0; i < 16; i++)
                    acc |= w[i];
                if (acc)
                {
                    int last = 0;
                    for (int i = 0; i < JIT_GUARD; i++)
                        if (jit_guard[g][i])
                            last = i;
                    RG_LOGE("TRANSLATION CACHE OVERRUN: code was written %d bytes past the end of the %s cache (ROM cache %u KB used, RAM cache %u KB used)",
                            last + 1, g ? "RAM" : "ROM",
                            (unsigned)((rom_translation_ptr - rom_translation_cache) / 1024),
                            (unsigned)((ram_translation_ptr - ram_translation_cache) / 1024));
                    jit_guard_hit = true;
                }
            }
        }
        {
            static uint32_t stack_log, stack_min = 0xFFFFFFFF;
            uint32_t hw = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
            if (hw < stack_min)
                stack_min = hw;
            if (++stack_log % 1800 == 0)   /* every 30 s */
                RG_LOGI("gbsp task stack: never less than %u bytes free of %u | internal RAM free %u KB, lowest ever %u KB, largest block %u KB | PSRAM free %u KB",
                        (unsigned)stack_min, (unsigned)GBSP_TASK_STACK,
                        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                        (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        }
#endif

        size_t frames_count = sound_read_samples((s16 *)mixbuffer, AUDIO_BUFFER_LENGTH);
        // RG_TIMER_LAP("sound_read_samples");

        rg_system_tick(rg_system_timer() - startTime);

        rg_audio_submit(mixbuffer, frames_count);
        // RG_TIMER_LAP("rg_audio_submit");

        /* lines are drawn on core 1: skipping a frame saves core 0 nothing, and
           retro-go's auto frameskip would only drop picture (the author's app does the same) */
        if (gbsp_render_core1)
            skip_next_frame = 0;
        else if (skip_next_frame == 0)
            skip_next_frame = app->frameskip;
        else if (skip_next_frame > 0)
            skip_next_frame--;
    }

    RG_PANIC("GBsP Ended");
}
