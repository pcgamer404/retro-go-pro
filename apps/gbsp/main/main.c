#include <rg_system.h>
#include <rg_display.h>
#include <rg_storage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
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
extern u32 flash_device_id;
extern u32 gamepak_buffer_count;

/* The battery save file: written in chunks with a yield between them (the card
   is also the ROM's paging store), replaced atomically, and recovered from the
   .tmp if a power cut lands between the remove and the rename. */
#define SRAM_FILE_LOGE(...) RG_LOGE(__VA_ARGS__)
#define SRAM_FILE_LOGI(...) RG_LOGI(__VA_ARGS__)
/* 16 KB was one long hold of the SPI bus the LCD also lives on: a card that
   takes 100 ms to program a block left the display task waiting for over a
   second per update (perf: lcd 4 @ 1430 ms). 4 KB per turn, and wait for the
   display to finish the frame it is on, keeps the save smooth. */
#define SRAM_FILE_CHUNK (4 * 1024)
static inline void gp_save_yield(void)
{
    for (int i = 0; i < 100 && rg_display_is_busy(); i++)
        vTaskDelay(pdMS_TO_TICKS(1));
    vTaskDelay(pdMS_TO_TICKS(6));
}
#define SRAM_FILE_YIELD() gp_save_yield()
#include "sram_file.h"

#ifdef HAVE_DYNAREC
#include "xjit_exec.h"
#endif

/* ---- Pokemon GBSP Directories (/sd/roms/pokegbsp/) ----------------------- */
#define POKE_BASE_DIR   RG_BASE_PATH_ROMS "/pokegbsp"
#define POKE_BIOS_DIR   POKE_BASE_DIR "/bios"
#define POKE_SAVES_DIR  POKE_BASE_DIR "/saves"
#define POKE_CACHE_DIR  POKE_BASE_DIR "/cache"

/* ---- Xtensa dynarec glue (xtensa-68000-dynarec, pjcau) -------------------
 * HAVE_DYNAREC / XTENSA_ARCH / SMALL_TRANSLATION_CACHE must be defined for THIS
 * file as well (main/CMakeLists.txt): gbsp_memory_t changes size with them.   */
extern u8 gamepak_backup_dirty;           /* gba_memory.c: set on every backup write */
void gbsp_render_start(void);             /* video.cpp: core-1 line renderer          */
void gbsp_render_wait(void);              /* video.cpp: core 1 has drawn every queued line */
extern int gbsp_render_core1;             /* video.cpp: 1 once the renderer task runs */
void gbsp_display_poll(void);             /* video.cpp calls periodically during rendering */
#ifdef HAVE_DYNAREC
int dynarec_enable = 0;                   /* 1 = translated code, 0 = interpreter     */
extern int xt_give_up;                    /* xtensa_stub.c: SMC storm, use interpreter*/
extern u32 xt_exec_delta;
extern u32 xt_rom_translations, xt_rom_flushes;
extern u8 *rom_translation_cache, *ram_translation_cache;
extern u8 *rom_translation_ptr, *ram_translation_ptr;
void gbsp_rvram_alloc(void);              /* video.cpp: renderer's VRAM copy          */
void xt_rblk_alloc(void);                 /* cpu_threaded.c: per-block SMC records    */
static xj_exec_t jit_mem;
#define JIT_GUARD (16 * 1024)
static u8 *jit_guard[2];
static char jit_flag_path[320];           /* "<rom>.jit": see jit_flag_read()         */
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

#define NUM_UPDATES 3   /* one displayed, one pending, one being rendered */
static rg_surface_t *updates[NUM_UPDATES];
static int updateCount;
static rg_surface_t *currentUpdate;   /* being rendered by the emulator/scanline task */
static rg_surface_t *shownUpdate;     /* most recently submitted (redraw, screenshot) */
static rg_surface_t *displaying;      /* current display transfer source; never draw into it */
static rg_surface_t *pending;         /* newest completed frame waiting for display */
static rg_app_t *app;

void gbsp_display_poll(void)
{
    if (pending && !rg_display_is_busy())
    {
        rg_surface_t *next = pending;
        pending = NULL;
        rg_display_submit(next, 0);
        displaying = next;
        shownUpdate = next;
    }
}

static const char *SETTING_SOUND_EMULATION = "sound";
static const char *SETTING_FRAMESKIP = "frameskip";
static const char *SETTING_RENDER = "render";
static int render_opt = 1;          /* 1 = core 1, 0 = synchronous on core 0 */
static bool render_task_ok;

/* ---- Frameskip -----------------------------------------------------------
 * The app used to force skip 0 whenever the core-1 renderer was running. That
 * is what the "Raised frameskip to 1" line in Retro-Go's monitor came from:
 * it raised the level once a second and the app wrote 0 back on the next
 * frame, so the level never stuck and the log repeated for ever.
 *
 * A skipped frame is not free but it is much cheaper: update_scanline()
 * returns before it snapshots the line, wakes core 1, hashes, copies or
 * submits, so core 1 stays idle and does not compete for the shared
 * instruction cache. pjcau measured this on the same board and core
 * (2026-10-10, Mario Kart Super Circuit): a drawn frame costs 19.1 ms of
 * core 0 and a skipped one 12.5 ms, so drawing every other frame averages
 * about 16 ms -- full speed at 60 Hz.
 *
 * Auto is not based on SPEED/BUSY: at skip 1 Mario Kart reads SPEED 100% and
 * BUSY 96% while skip 0 would cost 18.3 ms, so a rule built on those would
 * drop to 0, come back slow and climb again once a second. It predicts the
 * cost from the frames already timed (execute time with core 1 drawing vs
 * with core 1 idle) and only drops to a level the game has actually carried
 * before. This is pjcau's controller, with the clock read in microseconds
 * (rg_system_timer) instead of CPU cycles so it stays correct when the CPU
 * is overclocked.
 */
#define FS_HOLD_MIN    3       /* seconds the prediction must hold before a drop */
#define FS_HOLD_MAX    30      /* ... after the backoff below has doubled it */
#define FS_RAISE_SOON  3       /* a raise this soon after a drop means it was wrong */
#define FS_CALM        30      /* seconds with no raise that clear the backoff */
#define FS_DROP_US     13000   /* the fallback gate, when a level is unmeasured */
#define FS_PROVEN_US   16200   /* a level that cost more than this could not hold */
#define FS_LIGHTER_US  2000    /* ... so only retry it once the scene is this much lighter */
#define FS_FAST_US     12000   /* a drawn frame already this cheap: drop without the hold */
#define FS_STALL_US    100000  /* a frame this long is a stall, not a cost */

static int frameskip_opt = -1;      /* -1 = Auto, 0..2 = pinned */
static int frameskip_level;         /* what the frame loop uses now */
static uint64_t fs_drawn_us, fs_skip_us;
static uint32_t fs_drawn_n, fs_skip_n;
static int fs_good, fs_hold = FS_HOLD_MIN, fs_since_drop = FS_CALM, fs_since_raise = FS_CALM;
static uint32_t fs_at[3], fs_drawn_total, fs_seconds;   /* what the board reports */
static uint32_t fs_max_us;          /* the worst frame this second: a stall shows here */
static uint32_t fs_meas[3];         /* what a frame really cost while at that level */
static uint32_t fs_drawn_ref[3];    /* the drawn time just after arriving at it */

/* In the frame path: one add and one increment, no divide, no log. */
static void frameskip_auto_frame(uint32_t us, bool drawn)
{
    if (frameskip_opt >= 0)
        return;              /* pinned: nothing to decide */
    if (us > fs_max_us) fs_max_us = us;
    if (drawn) { fs_drawn_us += us; fs_drawn_n++; }
    else { fs_skip_us += us; fs_skip_n++; }
}

/* May we go down a level? Read what the level below actually cost last time:
 * never measured: fall back to the prediction; held: go; could not hold: only
 * go once this scene is measurably lighter than when we arrived here. */
static bool fs_may_drop(int k, uint32_t drawn, uint32_t predicted)
{
    const uint32_t below = fs_meas[k - 1];

    /* Do not drop into a level whose average frame would already miss 60 Hz.
     * The level below may have been cheap in a lighter scene, but this one is
     * not that scene: at skip 2 the log showed a drop to skip 1 with a
     * predicted 23.4 ms, followed by a raise one second later. That is the
     * 1 <-> 2 judder the player sees. */
    if (predicted > 17500)
        return false;

    if (!below)
        return predicted && predicted < FS_DROP_US;
    if (below <= FS_PROVEN_US)
        return true;
    return fs_drawn_ref[k] && drawn && drawn + FS_LIGHTER_US <= fs_drawn_ref[k];
}

/* once a second, Auto only */
static void frameskip_auto_second(void)
{
    const uint32_t drawn = fs_drawn_n ? (uint32_t)(fs_drawn_us / fs_drawn_n) : 0;
    const uint32_t skipped = fs_skip_n ? (uint32_t)(fs_skip_us / fs_skip_n) : 0;
    const uint64_t drawn_us_s = fs_drawn_us, skip_us_s = fs_skip_us;
    const uint32_t n_frames = fs_drawn_n + fs_skip_n;
    const int k = frameskip_level;
    const rg_stats_t st = rg_system_get_stats();
    uint32_t predicted = 0;

    if (k >= 0 && k < 3)
        fs_at[k]++;
    fs_drawn_total += fs_drawn_n;
    fs_seconds++;
    fs_drawn_us = fs_skip_us = 0;
    fs_drawn_n = fs_skip_n = 0;
    if (fs_since_drop < FS_CALM) fs_since_drop++;
    if (fs_since_raise < FS_CALM) fs_since_raise++;
    if (fs_since_raise >= FS_CALM)
        fs_hold = FS_HOLD_MIN;   /* a calm stretch forgets the backoff */

    /* what a frame would average one level down */
    if (k > 0)
        predicted = (drawn + (uint32_t)(k - 1) * skipped) / (uint32_t)k;

    /* What this level really costs a frame -- but not from a second that
     * contains a stall (a state load is one 138 ms frame, and remembering it
     * as this level's cost would block the way back down for the session). */
    if (n_frames && fs_max_us < FS_STALL_US)
    {
        fs_meas[k] = (uint32_t)((drawn_us_s + skip_us_s) / n_frames);
        if (!fs_drawn_ref[k] && drawn)
            fs_drawn_ref[k] = drawn;
    }
    fs_max_us = 0;

    /* Work on the average frame, not only on a drawn one: at skip 1 a drawn
     * frame may still cost 19 ms and the skipped one 12 ms, which is 15.5 ms
     * a frame -- enough for 60 Hz. Raising again there would make the screen
     * judder for no reason. */
    const uint32_t frame_us = n_frames ? (uint32_t)((drawn_us_s + skip_us_s) / n_frames) : 0;

    /* The speed/busy pair is only a backstop: it once said 90% at a 15.06 ms
     * average (66 Hz) and raised to skip 2, cutting the drawn rate for no
     * reason. Raise on the measured frame first; demand a really stuck core
     * before trusting the percentage. */
    if (((frame_us > 17500) ||
         (frame_us > 16500 && st.speedPercent < 85.f && st.busyPercent > 95.f)) && k < 2)
    {
        /* Raising is never delayed. A raise straight after a drop says the
         * drop was wrong, so the next one has to earn twice the patience. */
        if (fs_since_drop <= FS_RAISE_SOON && fs_hold < FS_HOLD_MAX)
            fs_hold = fs_hold * 2 > FS_HOLD_MAX ? FS_HOLD_MAX : fs_hold * 2;
        frameskip_level = k + 1;
        fs_drawn_ref[k + 1] = 0;
        fs_since_raise = 0;
        fs_good = 0;
        RG_LOGI("frameskip %d -> %d: frame %u.%02u ms, speed %d%%, drawn %u.%02u ms, skipped %u.%02u ms, hold now %ds",
                k, k + 1, (unsigned)(frame_us / 1000), (unsigned)((frame_us % 1000) / 10),
                (int)st.speedPercent, (unsigned)(drawn / 1000), (unsigned)((drawn % 1000) / 10),
                (unsigned)(skipped / 1000), (unsigned)((skipped % 1000) / 10), fs_hold);
    }
    else if (k > 0 && fs_may_drop(k, drawn, predicted))
    {
        /* A scene can get much lighter all at once: a race ends, a menu opens.
         * A drawn frame is what a frame costs one level down, so when one is
         * already well inside the 60 Hz budget the level below cannot be
         * wrong, and making it wait the hold is what made those menus feel
         * slow. */
        const bool light = drawn && drawn <= FS_FAST_US;
        const int hold = light ? 0 : fs_hold;

        if (light || ++fs_good >= fs_hold)
        {
            frameskip_level = k - 1;
            fs_drawn_ref[k - 1] = 0;
            fs_since_drop = 0;
            fs_good = 0;
            RG_LOGI("frameskip %d -> %d: skip%d last cost %u.%02u ms, drawn %u.%02u "
                    "(ref %u.%02u), skipped %u.%02u, predicted %u.%02u, held %ds%s",
                    k, k - 1, k - 1,
                    (unsigned)(fs_meas[k - 1] / 1000), (unsigned)((fs_meas[k - 1] % 1000) / 10),
                    (unsigned)(drawn / 1000), (unsigned)((drawn % 1000) / 10),
                    (unsigned)(fs_drawn_ref[k] / 1000), (unsigned)((fs_drawn_ref[k] % 1000) / 10),
                    (unsigned)(skipped / 1000), (unsigned)((skipped % 1000) / 10),
                    (unsigned)(predicted / 1000), (unsigned)((predicted % 1000) / 10),
                    hold, light ? " (fast)" : "");
        }
    }
    else
        fs_good = 0;

    if (fs_seconds % 10 == 0)
        RG_LOGI("frameskip residency: skip0 %us, skip1 %us, skip2 %us of %us, %u.%u drawn fps, "
                "measured %u.%02u / %u.%02u / %u.%02u ms, hold %ds, speed %d%%, busy %d%%",
                (unsigned)fs_at[0], (unsigned)fs_at[1], (unsigned)fs_at[2], (unsigned)fs_seconds,
                (unsigned)(fs_drawn_total / fs_seconds),
                (unsigned)((fs_drawn_total * 10 / fs_seconds) % 10),
                (unsigned)(fs_meas[0] / 1000), (unsigned)((fs_meas[0] % 1000) / 10),
                (unsigned)(fs_meas[1] / 1000), (unsigned)((fs_meas[1] % 1000) / 10),
                (unsigned)(fs_meas[2] / 1000), (unsigned)((fs_meas[2] % 1000) / 10), fs_hold,
                (int)st.speedPercent, (int)st.busyPercent);
}

static rg_gui_event_t renderer_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        render_opt = !render_opt;
        rg_settings_set_number(NS_APP, SETTING_RENDER, render_opt);
        /* Drain the queue before changing where lines are drawn. The game is
           paused in this dialog, so no line is outstanding in practice. */
        gbsp_render_wait();
        if (render_task_ok)
            gbsp_render_core1 = render_opt;
        RG_LOGI("line renderer: %s", gbsp_render_core1 ? "core 1" : "sync (core 0)");
    }

    strcpy(option->value, (render_opt && render_task_ok) ? _("Core 1") : _("Sync"));

    return RG_DIALOG_VOID;
}

static void frameskip_apply(void)
{
    fs_drawn_us = fs_skip_us = 0;
    fs_drawn_n = fs_skip_n = 0;
    fs_good = 0;
    fs_hold = FS_HOLD_MIN;
    fs_since_drop = fs_since_raise = FS_CALM;
    fs_max_us = 0;
    memset(fs_meas, 0, sizeof(fs_meas));
    memset(fs_drawn_ref, 0, sizeof(fs_drawn_ref));
    frameskip_level = frameskip_opt < 0 ? 0 : frameskip_opt;
}

static rg_gui_event_t frameskip_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV && frameskip_opt > -1)
        frameskip_opt--;
    else if (event == RG_DIALOG_NEXT && frameskip_opt < 2)
        frameskip_opt++;

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        rg_settings_set_number(NS_APP, SETTING_FRAMESKIP, frameskip_opt);
        frameskip_apply();
        app->frameskip = frameskip_level;   /* the dialog's status bars */
    }

    strcpy(option->value, frameskip_opt < 0 ? _("Auto") :
           frameskip_opt == 0 ? _("0 (off)") :
           frameskip_opt == 1 ? _("1 of 2") : _("2 of 3"));

    return RG_DIALOG_VOID;
}

/* ---- Battery saves ------------------------------------------------------
 * The game only sets gamepak_backup_dirty; nothing on the emulation thread
 * touches the SD card. Once the writes have stopped for GP_SAVE_IDLE_MS, the
 * save is copied to a PSRAM snapshot (microseconds) and a core-0 task writes it
 * out. Writing 128 KB straight from the frame loop used to cost a few hundred
 * milliseconds in one frame, visible as a hitch right after saving. */
#define GP_SAVE_IDLE_MS 1500
/* The SD card and the LCD share SPI2. A 4 KB card program can hold the bus
 * for 100-200 ms, and the display task then shows the frame being written as
 * a slow top-to-bottom wipe. Do not start a save while the screen is being
 * refreshed: the race load is exactly that window (perf: lcd 30 @ 178.92 ms).
 * The save then waits for a quiet screen (menu, pause, static dialog), and the
 * menu/exit path still flushes it synchronously. */
#define GP_SAVE_QUIET_MS 2000

static char gp_save_path[280];        /* sram_file.h's .tmp needs 320 */
static u8 *gp_save_snap;              /* PSRAM, claimed before the ROM cache */
static size_t gp_save_snap_len;
static TaskHandle_t gp_save_task_h;
static SemaphoreHandle_t gp_save_lock;
static volatile bool gp_save_snap_pending;
static bool gp_save_enabled;
static bool gp_save_dirty;
static bool gp_save_quiet_logged;
static TickType_t gp_save_last_dirty;
static int32_t gp_save_lcd_seen;
static TickType_t gp_save_lcd_quiet;
static u32 gp_save_pages_seen;
static TickType_t gp_save_pages_quiet;

static size_t gp_save_size(void)
{
    /* The cartridge's real size: 32 KB SRAM, 64/128 KB flash, 512 B/8 KB
       EEPROM, or the whole 128 KB buffer when the type is not known yet. */
    return sram_backup_size(backup_type, flash_bank_cnt, eeprom_size, sizeof(gamepak_backup));
}

static void gp_save_make_path(const char *rom_path)
{
    const char *base = strrchr(rom_path, '/');
    base = base ? (base + 1) : rom_path;
    snprintf(gp_save_path, sizeof(gp_save_path), "%s/%s", POKE_SAVES_DIR, base);
    char *dot = strrchr(gp_save_path, '.');
    if (dot)
        *dot = '\0';
    strncat(gp_save_path, ".sav", sizeof(gp_save_path) - strlen(gp_save_path) - 1);
}

extern void (*gamepak_load_progress)(int percent);

/* rg_system_init() leaves its hourglass alone in the middle of a blank screen
   and filling the ROM cache begins right after it. A 4 MB game takes several
   seconds off the card, so that lone icon sat there for the whole load and read
   as an opaque white box that had taken over the display. Replace it with a
   loading screen: which game, and a bar that moves. These are the same
   direct-to-panel calls the hourglass itself uses, because there is no surface
   to submit yet. */
static void load_progress_cb(int percent)
{
    static bool screen_drawn;
    static int box_w = 200, bar_x, bar_y, bar_w, bar_h;
    char line[48];
    const char *name = (app && app->romPath) ? strrchr(app->romPath, '/') : NULL;
    name = name ? name + 1 : "";

    if (!screen_drawn)
    {
        const int panel_w = rg_display_get_width();
        const int panel_h = rg_display_get_height();
        /* A fixed box width keeps the text inside the panel whatever the font;
           rg_gui_draw_text() clips to it instead of running off the edge. */
        size_t cap;

        box_w = (panel_w > 64) ? panel_w - 24 : 40;
        cap = (size_t)(box_w / 8);      /* the font is at most 8 px a character */
        if (cap > 30)
            cap = 30;

        screen_drawn = true;
        rg_display_clear(C_BLACK);

        if (name[0])
        {
            if (strlen(name) > cap && cap > 3)
                snprintf(line, sizeof(line), "%.*s...", (int)(cap - 3), name);
            else
                snprintf(line, sizeof(line), "%.*s", (int)cap, name);
            rg_gui_draw_text(RG_GUI_CENTER, RG_GUI_TOP + 10, box_w, line,
                             C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
        }

        /* The bar takes the hourglass's place in the middle of the panel. */
        bar_w = (panel_w > 88) ? panel_w / 2 : panel_w - 40;
        bar_h = 12;
        bar_x = (panel_w - bar_w) / 2;
        bar_y = (panel_h - bar_h) / 2;
        rg_gui_draw_rect(bar_x, bar_y, bar_w, bar_h, 1, C_WHITE, C_BLACK);
    }

    snprintf(line, sizeof(line), "Loading %d%%", percent);
    rg_gui_draw_text(RG_GUI_CENTER, bar_y + bar_h + 10, box_w, line,
                     C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);

    if (percent > 0)
        rg_gui_draw_rect(bar_x + 1, bar_y + 1, (bar_w - 2) * percent / 100, bar_h - 2,
                         0, C_WHITE, C_WHITE);
}

/* Called before init_gamepak_buffer(): during a game PSRAM belongs to the JIT
   and the ROM cache, so the snapshot has to be claimed first. */
static void gp_save_alloc(void)
{
    gp_save_snap = heap_caps_malloc(sizeof(gamepak_backup), MALLOC_CAP_SPIRAM);
    gp_save_lock = xSemaphoreCreateMutex();
    if (gp_save_snap && gp_save_lock)
        RG_LOGI("battery save: %u KB snapshot claimed before the ROM cache",
                (unsigned)(sizeof(gamepak_backup) / 1024));
    else
        RG_LOGE("battery save: no snapshot buffer, writes stay in the frame loop");
}

static void gp_save_write_locked(const void *data, size_t len)
{
    if (gp_save_lock)
        xSemaphoreTake(gp_save_lock, portMAX_DELAY);
    sram_file_write(gp_save_path, data, len);
    if (gp_save_lock)
        xSemaphoreGive(gp_save_lock);
}

static void gp_save_task(void *arg)
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (gp_save_snap_pending)
        {
            gp_save_write_locked(gp_save_snap, gp_save_snap_len);
            gp_save_snap_pending = false;
        }
    }
}

static void gp_save_init(const char *rom_path)
{
    gp_save_make_path(rom_path);
    gp_save_enabled = false;
    gp_save_dirty = false;
    gp_save_snap_pending = false;

    size_t got = sram_file_read(gp_save_path, gamepak_backup, sizeof(gamepak_backup));
    if (got)
        RG_LOGI("battery save loaded: %s (%u bytes)", gp_save_path, (unsigned)got);
    else
        RG_LOGI("battery save: nothing at %s yet", gp_save_path);

    gp_save_enabled = true;
    gp_save_quiet_logged = false;
    gamepak_backup_dirty = 0;
    gp_save_lcd_seen = rg_display_get_counters().totalFrames;
    gp_save_lcd_quiet = xTaskGetTickCount();
    gp_save_pages_seen = gamepak_page_loads;
    gp_save_pages_quiet = gp_save_lcd_quiet;

    if (gp_save_snap && !gp_save_task_h &&
        xTaskCreatePinnedToCore(gp_save_task, "gba_save", 4096, NULL, 1, &gp_save_task_h, 0) != pdPASS)
    {
        gp_save_task_h = NULL;
        RG_LOGE("battery save: no task, writes stay in the frame loop");
    }
}

/* Copy the battery to the snapshot and wake the writer. */
static void gp_save_schedule(void)
{
    if (!gp_save_enabled || !gp_save_dirty)
        return;
    if (!gp_save_snap || !gp_save_task_h)
    {
        gp_save_dirty = false;
        gp_save_write_locked(gamepak_backup, gp_save_size());
        return;
    }
    if (gp_save_snap_pending)
        return;
    gp_save_snap_len = gp_save_size();
    memcpy(gp_save_snap, gamepak_backup, gp_save_snap_len);
    gp_save_dirty = false;   /* writes landing after this set it again */
    gp_save_snap_pending = true;
    xTaskNotifyGive(gp_save_task_h);
}

/* The menu, a save-state load and a shutdown: nothing may be left unwritten. */
static void gp_save_flush_sync(void)
{
    if (!gp_save_enabled)
        return;
    /* the writer owns the snapshot until it is done; a card that stops
       answering must not hold the menu (or a shutdown) for ever */
    for (int i = 0; i < 300 && gp_save_snap_pending; i++)
        vTaskDelay(1);   /* 100 Hz: up to three seconds */
    if (gp_save_snap_pending)
    {
        RG_LOGE("battery save: the writer is stuck, leaving the save to the task");
        return;
    }
    if (gamepak_backup_dirty)
    {
        gamepak_backup_dirty = 0;
        gp_save_write_locked(gamepak_backup, gp_save_size());
    }
    gp_save_dirty = false;
}

static void gp_save_tick(void)
{
    if (!gp_save_enabled)
        return;

    TickType_t now = xTaskGetTickCount();
    const rg_display_counters_t dc = rg_display_get_counters();

    /* Track the two things that share SPI2 with the card. */
    if (dc.totalFrames != gp_save_lcd_seen)
    {
        gp_save_lcd_seen = dc.totalFrames;
        gp_save_lcd_quiet = now;
    }
    if (gamepak_page_loads != gp_save_pages_seen)
    {
        gp_save_pages_seen = gamepak_page_loads;
        gp_save_pages_quiet = now;
    }

    /* Fast path: the game did not write the save memory. */
    if (gamepak_backup_dirty)
    {
        gamepak_backup_dirty = 0;
        gp_save_dirty = true;
        gp_save_last_dirty = now;
        return;
    }
    if (!gp_save_dirty || gp_save_snap_pending)
        return;
    if ((now - gp_save_last_dirty) * portTICK_PERIOD_MS < GP_SAVE_IDLE_MS)
        return;

    /* A quiet screen and no ROM paging: otherwise the card holds the bus
       while the display is trying to draw the race's first frames. */
    if ((now - gp_save_lcd_quiet) * portTICK_PERIOD_MS < GP_SAVE_QUIET_MS ||
        (now - gp_save_pages_quiet) * portTICK_PERIOD_MS < GP_SAVE_QUIET_MS)
    {
        if (!gp_save_quiet_logged)
        {
            gp_save_quiet_logged = true;
            RG_LOGI("battery save: deferred until the screen is idle");
        }
        return;
    }

    gp_save_quiet_logged = false;
    gp_save_schedule();
}

void netpacket_poll_receive()
{
}

void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    if (!shownUpdate)
        return false;
    if (width <= 0)
        width = shownUpdate->width;
    if (height <= 0)
        height = shownUpdate->height;
    return rg_surface_save_image_file(shownUpdate, filename, width, height);
}

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
    if (success)
        gp_save_flush_sync();   /* the state carries its own battery contents */
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
static void jit_flag_write(int status, int fails);
#endif

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_SHUTDOWN)
    {
        gp_save_flush_sync();
#ifdef HAVE_DYNAREC
        if (jit_trying)
        {
            /* a clean exit proves the dynarec as well as two minutes do */
            jit_flag_write(0, 0);
            jit_trying = false;
        }
#endif
    }
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(shownUpdate ? shownUpdate : currentUpdate, 0);
    }
}

int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    uint32_t joystick = rg_input_read_gamepad();
    int16_t val = 0;
    if (joystick & RG_KEY_DOWN) val |= (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (joystick & RG_KEY_UP) val |= (1 << RETRO_DEVICE_ID_JOYPAD_UP);
    if (joystick & RG_KEY_LEFT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (joystick & RG_KEY_RIGHT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (joystick & RG_KEY_START) val |= (1 << RETRO_DEVICE_ID_JOYPAD_START);
    if (joystick & RG_KEY_SELECT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_SELECT);
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
/* Per game, "<rom>.jit" in the cache directory:
 *   absent    never tried, or proven (two minutes of play, or a clean exit)
 *   T<n>      on probation; n = starts that crashed or hung on the dynarec
 *   O<n>      off: n = 3 after repeated crashes, 0 when chosen in the menu
 *   X<n>      the dynarec gave up (the game rewrites its own code)
 * A power cut or an app switch also leaves T behind, so the reset reason
 * decides whether it counts: only a panic or a watchdog is the dynarec's fault.
 * Without that, one battery cut inside the probation window disabled the
 * dynarec for a game for ever. */
#define JIT_FAILS_MAX 3

static bool jit_reset_was_a_crash(void)
{
    switch (esp_reset_reason())
    {
    case ESP_RST_PANIC:    return true;
    case ESP_RST_INT_WDT:  return true;
    case ESP_RST_TASK_WDT: return true;
    case ESP_RST_WDT:      return true;
    default:               return false;   /* POWERON, SW, EXT, BROWNOUT, ... */
    }
}

static int jit_flag_read(int *fails)
{
    *fails = 0;
    FILE *f = fopen(jit_flag_path, "rb");
    if (!f)
        return 0;
    int c = fgetc(f), n = 0, d;
    while ((d = fgetc(f)) >= '0' && d <= '9')
    {
        n = n * 10 + (d - '0');
        if (n > 99)
            break;
    }
    fclose(f);
    if (c == EOF)
        return 0;
    *fails = n;
    return c == '0' ? 'O' : c;   /* older builds wrote '0' for off */
}

static void jit_flag_write(int status, int fails)
{
    if (!status)
    {
        remove(jit_flag_path);
        return;
    }
    FILE *f = fopen(jit_flag_path, "wb");
    if (f)
    {
        fprintf(f, "%c%d", status, fails);
        fclose(f);
    }
}

static void jit_mark_ok(void)
{
    if (jit_trying && rg_system_timer() - jit_t0 > 120 * 1000000)
    {
        jit_flag_write(0, 0);
        jit_trying = false;
    }
}

static rg_gui_event_t jit_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        if (dynarec_enable)
        {
            dynarec_enable = 0;
            jit_trying = false;
            jit_flag_write('O', 0);
            RG_LOGI("dynarec: off for this game, interpreter");
        }
        else if (!jit_restart)
        {
            jit_flag_write(0, 0);
            jit_restart = true;   /* the caches are sized at boot: restart */
        }
    }
    strcpy(option->value, dynarec_enable ? _("On") : jit_restart ? _("On (restart)") : _("Off"));
    return RG_DIALOG_VOID;
}
#endif

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Audio enable"), "-", RG_DIALOG_FLAG_NORMAL, &sound_toggle_cb};
    *dest++ = (rg_gui_option_t){0, _("Frameskip"), "-", RG_DIALOG_FLAG_NORMAL, &frameskip_cb};
    *dest++ = (rg_gui_option_t){0, _("Renderer"), "-", RG_DIALOG_FLAG_NORMAL, &renderer_cb};
#ifdef HAVE_DYNAREC
    *dest++ = (rg_gui_option_t){0, _("Fast CPU (dynarec)"), "-", RG_DIALOG_FLAG_NORMAL, &jit_toggle_cb};
#endif
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

#ifdef HAVE_DYNAREC
#define GBSP_TASK_STACK (20 * 1024)
static void gbsp_main(void);
static void gbsp_task(void *arg)
{
    gbsp_main();
}
void app_main(void)
{
    if (xTaskCreatePinnedToCore(gbsp_task, "gbsp", GBSP_TASK_STACK, NULL, uxTaskPriorityGet(NULL), NULL, 0) != pdPASS)
        gbsp_main();
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

    rg_storage_mkdir(POKE_BASE_DIR);
    rg_storage_mkdir(POKE_BIOS_DIR);
    rg_storage_mkdir(POKE_SAVES_DIR);
    rg_storage_mkdir(POKE_CACHE_DIR);

    sound_master_enable = rg_settings_get_number(NS_APP, SETTING_SOUND_EMULATION, true);

    /* This app owns frameskip (see the controller above). -1 turns rg_system's
       own auto off, which otherwise raises the level once a second while this
       app lowers it again -- the "Raised frameskip to 1" loop in the log. */
    frameskip_opt = (int)rg_settings_get_number(NS_APP, SETTING_FRAMESKIP, -1);
    if (frameskip_opt < -1 || frameskip_opt > 2)
        frameskip_opt = -1;   /* a stored value from another build is not trusted */
    frameskip_apply();
    app->frameskip = -1;
    RG_LOGI("frameskip: %s", frameskip_opt < 0 ? "Auto" :
            frameskip_opt == 0 ? "pinned 0 (off)" :
            frameskip_opt == 1 ? "pinned 1 of 2" : "pinned 2 of 3");

    for (int i = 0; i < NUM_UPDATES; i++)
    {
        updates[i] = rg_surface_create(GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT + 1, RG_PIXEL_565_LE, MEM_SLOW);
        if (!updates[i])
            break;
        updates[i]->height = GBA_SCREEN_HEIGHT;
        updateCount++;
    }
    if (updateCount == 0)
        RG_PANIC("Could not allocate a GBA video buffer.");
    currentUpdate = shownUpdate = updates[0];

    gba_screen_pixels = currentUpdate->data;

    gbsp_memory = rg_alloc(sizeof(*gbsp_memory), MEM_ANY);
    RG_LOGI("gbsp_memory=%p (%u KB)", gbsp_memory, (unsigned)(sizeof(*gbsp_memory) / 1024));

    libretro_supports_bitmasks = true;
    retro_set_input_state(input_cb);

    gbsp_render_start();
    render_task_ok = gbsp_render_core1 != 0;
    render_opt = (int)rg_settings_get_number(NS_APP, SETTING_RENDER, 1);
    if (render_opt < 0 || render_opt > 1 || !render_task_ok)
        render_opt = render_task_ok ? 1 : 0;
    gbsp_render_core1 = render_opt;
    RG_LOGI("line renderer: %s", gbsp_render_core1 ? "core 1" : "sync (core 0)");
#ifdef HAVE_DYNAREC
    {
        const char *base = strrchr(app->romPath, '/');
        base = base ? (base + 1) : app->romPath;
        snprintf(jit_flag_path, sizeof(jit_flag_path), "%s/%s", POKE_CACHE_DIR, base);
        char *dot = strrchr(jit_flag_path, '.');
        if (dot)
            *dot = '\0';
        strncat(jit_flag_path, ".jit", sizeof(jit_flag_path) - strlen(jit_flag_path) - 1);

        int fails = 0;
        int flag = jit_flag_read(&fails);
        bool crashed = jit_reset_was_a_crash();

        if (flag == 'T')
        {
            if (crashed)
                fails++;
            if (fails >= JIT_FAILS_MAX)
            {
                jit_flag_write('O', fails);
                flag = 'O';
                RG_LOGW("dynarec: it crashed on this game %d times (reset reason %d); "
                        "using the interpreter from now on (Options > Fast CPU to retry)",
                        fails, (int)esp_reset_reason());
            }
            else
            {
                jit_flag_write('T', fails);
                RG_LOGW("dynarec: the last start %s (reset reason %d), attempt %d of %d",
                        crashed ? "crashed" : "did not finish but did not crash",
                        (int)esp_reset_reason(), fails + 1, JIT_FAILS_MAX);
            }
        }

        if ((flag == 0 || flag == 'T') &&
            xj_exec_alloc_psram(&jit_mem, ROM_TRANSLATION_CACHE_SIZE + JIT_GUARD + RAM_TRANSLATION_CACHE_SIZE + JIT_GUARD))
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
            jit_flag_write('T', fails);
            jit_trying = true;
            jit_t0 = rg_system_timer();
            RG_LOGI("dynarec: caches %u KB at %p (exec %08x)",
                    (unsigned)((ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE) / 1024),
                    jit_mem.data, (unsigned)jit_mem.exec);
        }
        else if (flag == 0 || flag == 'T')
        {
            RG_LOGW("dynarec: no memory for the translation caches, interpreter");
        }
        else
        {
            RG_LOGW("dynarec: off for this game (%s), interpreter",
                    flag == 'X' ? "gave up: the game rewrites its own code" : "disabled");
        }

        gbsp_rvram_alloc();   /* the renderer's VRAM copy, also before the ROM cache */
        if (dynarec_enable)
            xt_rblk_alloc();  /* per-block SMC records: a store only rewinds its own block */
    }
#endif
    gp_save_alloc();          /* before the ROM cache: during a game PSRAM is full */
    init_gamepak_buffer();
    RG_LOGI("ROM cache: %u blocks of 1 MB, cpu %d MHz, %u KB PSRAM free (%u KB largest)",
            (unsigned)gamepak_buffer_count, rg_system_get_cpu_speed(),
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
            (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    init_sound();

    /* Load optional official GBA BIOS if present in /sd/roms/pokegbsp/bios/gba_bios.bin */
    struct stat st;
    if (stat(POKE_BIOS_DIR "/gba_bios.bin", &st) == 0)
    {
        load_bios(POKE_BIOS_DIR "/gba_bios.bin");
    }

    memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
    gamepak_load_progress = load_progress_cb;
    load_progress_cb(0);   /* replace the system hourglass before the first read */
    /* RTC autodetect: the gpSP database enables the clock for the games that
       have one (Pokemon Ruby/Sapphire/Emerald among them), so berries, tides
       and the day/night cycle follow the handheld's clock. Rumble and the
       link port stay off: this build has no motor and no link partner. */
    if (load_gamepak(NULL, app->romPath, FEAT_AUTODETECT, FEAT_DISABLE, SERIAL_MODE_DISABLED) != 0)
    {
        RG_PANIC("Could not load the game file.");
    }
    gamepak_load_progress = NULL;
    RG_LOGI("ROM: %u KB, %s in the cache", (unsigned)(gamepak_size / 1024),
            (gamepak_size <= gamepak_buffer_count * 1024u * 1024u) ? "fits" : "does not fit");

    RG_LOGI("reset_gba");
    reset_gba();

    /* Pokemon ROM hacks often carry a modified header that keeps gpSP from
       knowing their save chip. Give flash 128 KB (two banks) up front: a hack
       that needs the second bank finds it, while the save *type* is still
       detected at run time, so plain SRAM and EEPROM games keep what they
       expect. This has to be after reset_gba(): init_memory() reloads the
       database's value there. */
    flash_device_id = FLASH_DEVICE_MACRONIX_128KB;
    flash_bank_cnt = FLASH_SIZE_128KB;

    gp_save_init(app->romPath);

    if (app->bootFlags & RG_BOOT_RESUME)
    {
        RG_LOGI("load_state");
        rg_emu_load_state(app->saveSlot);
    }

    rg_audio_sample_t mixbuffer[AUDIO_BUFFER_LENGTH] = {0};

    /* Where a frame goes, once every 5 s. Six timer reads a frame (~1 us each)
       cost well under 0.1% of an 18 ms frame. A report from the board can then
       name the slow part instead of leaving it to guesswork:
         exec   core 0 translating/emulating, with core 1 drawing
         wait   blocked in gbsp_render_wait() -- the renderer is the bottleneck
         disp   poll/submit/sync, i.e. the display transfer
         audio  mixing and submitting the frame's samples
         page   ROM pages read from the SD card (cache misses; 32 KB each) */
    int64_t perf_report = rg_system_timer();
    int64_t fs_last = rg_system_timer();
    uint32_t perf_frames = 0, perf_drawn = 0, perf_pages = gamepak_page_loads;
    uint32_t perf_trans = 0, perf_flush = 0;
    int32_t perf_lcd_frames = rg_display_get_counters().totalFrames;
    int64_t perf_lcd_busy = rg_display_get_counters().busyTime;
#ifdef HAVE_DYNAREC
    perf_trans = xt_rom_translations;
    perf_flush = xt_rom_flushes;
#endif
    uint64_t perf_exec = 0, perf_wait = 0, perf_disp = 0, perf_audio = 0;

    while (true)
    {
        const int64_t startTime = rg_system_timer();
        uint32_t joystick = rg_input_read_gamepad();
        const bool drawn = !skip_next_frame;

        if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
        {
#ifdef HAVE_DYNAREC
            jit_mark_ok();
#endif
            gp_save_flush_sync();
            /* The dialog's status bars read app->frameskip, which is -1 while
               playing so rg_system's own auto stays out of the decision. Show
               the real level for as long as the dialog is up. */
            app->frameskip = frameskip_level;
            if (joystick & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
            app->frameskip = -1;
            memset(&mixbuffer, 0, sizeof(mixbuffer));
#ifdef HAVE_DYNAREC
            if (jit_restart)
            {
                /* the user turned the dynarec back on: caches and probation are
                   set up at boot, so start the game again now */
                jit_restart = false;
                rg_system_restart();
            }
#endif
            fs_last = rg_system_timer();
            continue;
        }

        update_input();
        rumble_frame_reset();
        clear_gamepak_stickybits();

        const int64_t t_exec = rg_system_timer();
#ifdef HAVE_DYNAREC
        if (dynarec_enable)
        {
            execute_arm_translate(execute_cycles);
            if (xt_give_up)
            {
                RG_LOGW("dynarec: giving up for this game (code rewritten continuously)");
                dynarec_enable = 0;
                jit_trying = false;
                jit_flag_write('X', 0);
            }
            else
                jit_mark_ok();
        }
        else
#endif
            execute_arm(execute_cycles);
        const uint32_t exec_us = (uint32_t)(rg_system_timer() - t_exec);
        perf_exec += exec_us;

        if (!skip_next_frame)
        {
            const int64_t t_wait = rg_system_timer();
            gbsp_render_wait();
            perf_wait += (uint32_t)(rg_system_timer() - t_wait);

            const int64_t t_disp = rg_system_timer();
            if (updateCount >= 3)
            {
                rg_surface_t *finished = currentUpdate;
                pending = finished;
                gbsp_display_poll();

                rg_surface_t *nextUpdate = NULL;
                for (int i = 0; i < updateCount; i++)
                {
                    if (updates[i] != displaying && updates[i] != pending && updates[i] != finished)
                    {
                        nextUpdate = updates[i];
                        break;
                    }
                }
                if (!nextUpdate)
                    RG_PANIC("No safe GBA video buffer available.");
                currentUpdate = nextUpdate;
                gba_screen_pixels = currentUpdate->data;
            }
            else if (updateCount == 2)
            {
                rg_display_sync();
                rg_surface_t *finished = currentUpdate;
                rg_display_submit(finished, 0);
                displaying = shownUpdate = finished;
                currentUpdate = (finished == updates[0]) ? updates[1] : updates[0];
                gba_screen_pixels = currentUpdate->data;
            }
            else
            {
                rg_display_submit(currentUpdate, 0);
                displaying = shownUpdate = currentUpdate;
                while (rg_display_is_busy())
                    rg_task_yield();
                rg_display_sync();
                gba_screen_pixels = currentUpdate->data;
            }
            perf_disp += (uint32_t)(rg_system_timer() - t_disp);
        }

        const int64_t t_audio = rg_system_timer();
        gp_save_tick();
        size_t frames_count = sound_read_samples((s16 *)mixbuffer, AUDIO_BUFFER_LENGTH);
        const int64_t t_mix_end = rg_system_timer();

        rg_system_tick(t_mix_end - startTime);

        const int64_t t_submit = rg_system_timer();
        rg_audio_submit(mixbuffer, frames_count);
        perf_audio += (uint32_t)(t_mix_end - t_audio) + (uint32_t)(rg_system_timer() - t_submit);
        /* The audio sink can block for most of a frame. The display often
           frees up during that wait; submit then instead of losing the slot
           until the next emulated frame. */
        gbsp_display_poll();

        /* The controller measures core 0's execute time, the way it was tuned:
           a drawn frame is a frame spent with core 1 drawing, a skipped one a
           frame with core 1 idle. */
        frameskip_auto_frame(exec_us, drawn);

        if (skip_next_frame == 0)
            skip_next_frame = frameskip_level;
        else if (skip_next_frame > 0)
            skip_next_frame--;

        if (drawn)
            perf_drawn++;
        perf_frames++;

        const int64_t now = rg_system_timer();
        if (frameskip_opt < 0 && now - fs_last >= 1000000)
        {
            frameskip_auto_second();
            fs_last = now;
        }
        if (now - perf_report >= 5000000)
        {
            const uint32_t n = perf_frames ? perf_frames : 1;
            const uint32_t pages = gamepak_page_loads - perf_pages;
            const rg_display_counters_t dc = rg_display_get_counters();
            const uint32_t lcd_frames = (uint32_t)(dc.totalFrames - perf_lcd_frames);
            const uint32_t lcd_us = lcd_frames ? (uint32_t)((dc.busyTime - perf_lcd_busy) / lcd_frames) : 0;
            char jit[96] = "";
#ifdef HAVE_DYNAREC
            snprintf(jit, sizeof(jit), ", jit %d rend %d rcache %u/%u KB trans %u flush %u",
                     dynarec_enable, gbsp_render_core1,
                     (unsigned)((u32)(rom_translation_ptr - rom_translation_cache) / 1024),
                     (unsigned)(ROM_TRANSLATION_CACHE_SIZE / 1024),
                     (unsigned)(xt_rom_translations - perf_trans),
                     (unsigned)(xt_rom_flushes - perf_flush));
            perf_trans = xt_rom_translations;
            perf_flush = xt_rom_flushes;
#endif
            RG_LOGI("perf: %u frames (%u drawn), exec %u.%02u ms, wait %u.%02u, disp %u.%02u, "
                    "audio %u.%02u, %u page loads, skip %d, %u KB free, lcd %u @ %u.%02u ms%s",
                    (unsigned)perf_frames, (unsigned)perf_drawn,
                    (unsigned)(perf_exec / n / 1000), (unsigned)((perf_exec / n % 1000) / 10),
                    (unsigned)(perf_wait / n / 1000), (unsigned)((perf_wait / n % 1000) / 10),
                    (unsigned)(perf_disp / n / 1000), (unsigned)((perf_disp / n % 1000) / 10),
                    (unsigned)(perf_audio / n / 1000), (unsigned)((perf_audio / n % 1000) / 10),
                    (unsigned)pages, frameskip_level,
                    (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                    (unsigned)lcd_frames, (unsigned)(lcd_us / 1000), (unsigned)((lcd_us % 1000) / 10),
                    jit);
            perf_lcd_frames = dc.totalFrames;
            perf_lcd_busy = dc.busyTime;
            perf_frames = perf_drawn = 0;
            perf_exec = perf_wait = perf_disp = perf_audio = 0;
            perf_pages = gamepak_page_loads;
            perf_report = now;
        }
    }

    RG_PANIC("GBsP Ended");
}
