/* Nano Java app entry point for Retro-Go. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>

#include <rg_system.h>
#include <rg_storage.h>
#include <rg_display.h>
#include <rg_input.h>
#include <rg_gui.h>
#include <rg_settings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "../../src/vm/vm.h"
#include "../../src/pal/pal.h"
#include "../../src/midp/midp.h"
#include "../../src/util/zip.h"

extern const uint8_t _binary_nanojava_classlib_jar_start[];
extern const uint8_t _binary_nanojava_classlib_jar_end[];

void heap_set_stack_base(void *base);
void pal_set_canvas_size(int w, int h);
void pal_set_telemetry_enabled(bool enabled);
void pal_set_display_rotation(int mode);
void pal_set_centered_output(bool enabled);
void pal_set_diagonal_ms(int ms);

/* Per-game display settings live separately from Retro-Go's global app settings. */
static char s_game_cfg_path[256];
static int s_game_canvas_profile;
static int s_game_rotation = -1;
static int s_game_centered = 1;
static int s_game_sleep_mode = 2;  /* index into s_sleep_modes[] */
static int s_game_diag_ms;         /* 0 = diagonal keys off */

/* Diagonals as number keys ("Keypad diagonals"). Several Nokia ports only
 * accept 1/3/7/9 as the "continue" key on a screen and this handheld has no
 * numeric keypad, so holding a diagonal for this long presses the matching
 * digit. Off by default: a game that uses diagonals for movement would
 * otherwise also get a stray number key while the player holds one. */
static const int s_diag_choices[] = { 0, 500, 750, 1000 };
#define NJ_DIAG_COUNT     ((int)(sizeof(s_diag_choices) / sizeof(s_diag_choices[0])))
#define NJ_DIAG_DEFAULT   0

/* Java heap sizing: an upper bound so a device with plenty of PSRAM does not
 * hand all of it to the VM (and the 2 MB that the clamp below leaves to
 * everything else). */
#define NJ_HEAP_MAX           (6u << 20)

/* Thread.sleep() padding. Games use sleeps both to pace frames and to poll for
 * progress, and on a handheld this much slower than the phone the game was
 * written for that padding is often the largest single cost of a game that
 * feels slow or spends minutes on a loading screen. "Fast" shortens every
 * sleep of 2 ms or more, "Balanced" only frame-limiter sleeps (16 ms+),
 * "Accurate" honours sleeps exactly and "Turbo" turns every sleep of 2 ms or
 * more into 1 ms. See thread_sleep() in the VM. */
static const struct { int pct, min_ms; } s_sleep_modes[] = {
    { 100, 2 },   /* Accurate */
    {  25, 16 },  /* Balanced */
    {  25, 2 },   /* Fast (default) */
    {   0, 2 },   /* Turbo: every sleep of 2 ms+ becomes 1 ms */
};
#define NJ_SLEEP_MODE_COUNT   ((int)(sizeof(s_sleep_modes) / sizeof(s_sleep_modes[0])))
#define NJ_SLEEP_MODE_DEFAULT 2

static void apply_sleep_mode(void)
{
    int i = s_game_sleep_mode;
    if (i < 0 || i >= NJ_SLEEP_MODE_COUNT)
        i = NJ_SLEEP_MODE_DEFAULT;
    nj_sleep_pct = s_sleep_modes[i].pct;
    nj_sleep_min_ms = s_sleep_modes[i].min_ms;
}

/* Canvas profiles, in menu order (width, then height).  Every offered profile
 * fits the 320x240 panel without scaling, so the list deliberately stops at a
 * height of 240.  The three retired tall profiles stay in the table below the
 * offered ones: a saved config that names one keeps its canvas, it just is not
 * offered any more.  Index 0 follows the size in the JAR filename. */
static const struct { uint16_t w, h; bool offered; } s_canvas_profiles[] = {
    {   0,   0, true  },  /* 0: Auto (JAR name) */
    { 128, 128, true  },  /* 1 */
    { 128, 160, true  },  /* 2 */
    { 176, 208, true  },  /* 3 */
    { 176, 220, true  },  /* 4 */
    { 208, 208, true  },  /* 5 */
    { 320, 240, true  },  /* 6 */
    { 240, 320, false },  /* 7: retired (config version 1 only) */
    { 240, 400, false },  /* 8: retired */
    { 360, 640, false },  /* 9: retired */
};
#define NJ_CANVAS_COUNT ((int)(sizeof(s_canvas_profiles) / sizeof(s_canvas_profiles[0])))

/* config version 1 numbering -> current index */
static const int s_canvas_legacy_map[] = { 0, 6, 7, 3, 4, 8, 2, 9, 1 };

static void make_game_cfg_path(const char *rom_path)
{
    (void)mkdir("/sd/retro-go", 0775);
    (void)mkdir("/sd/retro-go/config", 0775);
    (void)mkdir("/sd/retro-go/config/j2me", 0775);
    (void)mkdir("/sd/retro-go/config/j2me/games", 0775);
    const char *base = rom_path ? strrchr(rom_path, '/') : NULL;
    base = base ? base + 1 : (rom_path ? rom_path : "unknown.jar");
    size_t n = strlen(base);
    if (n > 4 && strcasecmp(base + n - 4, ".jar") == 0) n -= 4;
    char slug[112]; size_t j = 0;
    for (size_t i = 0; i < n && j < sizeof(slug) - 1; i++) {
        unsigned char c = (unsigned char)base[i];
        slug[j++] = (isalnum(c) || c == '-' || c == '_') ? (char)c : '_';
    }
    slug[j] = '\0';
    if (!slug[0]) strcpy(slug, "unknown_game");
    snprintf(s_game_cfg_path, sizeof(s_game_cfg_path),
             "/sd/retro-go/config/j2me/games/%s.cfg", slug);
}

static void load_game_settings(const char *rom_path)
{
    make_game_cfg_path(rom_path);
    s_game_canvas_profile = 0;
    s_game_rotation = -1;
    s_game_centered = 1;
    s_game_sleep_mode = NJ_SLEEP_MODE_DEFAULT;
    s_game_diag_ms = s_diag_choices[NJ_DIAG_DEFAULT];
    int cfg_ver = 1;
    FILE *f = fopen(s_game_cfg_path, "r");
    if (f) {
        char key[48]; int value;
        while (fscanf(f, "%47[^=]=%d\n", key, &value) == 2) {
            if (!strcmp(key, "cfg_ver")) cfg_ver = value;
            else if (!strcmp(key, "canvas_profile") && value >= 0 && value < NJ_CANVAS_COUNT) s_game_canvas_profile = value;
            else if (!strcmp(key, "rotation") && value >= -1 && value <= 3) s_game_rotation = value;
            else if (!strcmp(key, "centered") && (value == 0 || value == 1)) s_game_centered = value;
            else if (!strcmp(key, "sleep_mode") && value >= 0 && value < NJ_SLEEP_MODE_COUNT) s_game_sleep_mode = value;
            else if (!strcmp(key, "diag_ms")) {
                for (int i = 0; i < NJ_DIAG_COUNT; i++)
                    if (value == s_diag_choices[i]) s_game_diag_ms = value;
            }
        }
        fclose(f);
    }
    /* Version 1 wrote the old canvas numbering (unsorted, with the three
     * tall profiles).  Translate it so a game keeps exactly the canvas the
     * player picked, even though the menu now lists the sizes in order. */
    if (cfg_ver < 2 && s_game_canvas_profile > 0 &&
        s_game_canvas_profile < (int)(sizeof(s_canvas_legacy_map) / sizeof(s_canvas_legacy_map[0])))
        s_game_canvas_profile = s_canvas_legacy_map[s_game_canvas_profile];
    pal_set_display_rotation(s_game_rotation);
    pal_set_centered_output(s_game_centered != 0);
    pal_set_diagonal_ms(s_game_diag_ms);
    apply_sleep_mode();
    RG_LOGI("J2ME per-game config: %s (canvas=%d rotation=%d centered=%d sleep_mode=%d diag_ms=%d)",
            s_game_cfg_path, s_game_canvas_profile, s_game_rotation, s_game_centered,
            s_game_sleep_mode, s_game_diag_ms);
}

static void save_game_settings(void)
{
    if (!s_game_cfg_path[0]) return;
    FILE *f = fopen(s_game_cfg_path, "w");
    if (!f) { RG_LOGW("Cannot save per-game J2ME settings: %s", s_game_cfg_path); return; }
    fprintf(f, "cfg_ver=2\ncanvas_profile=%d\nrotation=%d\ncentered=%d\nsleep_mode=%d\ndiag_ms=%d\n",
            s_game_canvas_profile, s_game_rotation, s_game_centered, s_game_sleep_mode,
            s_game_diag_ms);
    fclose(f);
}

static size_t file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz > 0 ? (size_t)sz : 0;
}

static void select_canvas_size(const char *path)
{
    int profile = s_game_canvas_profile;
    if (profile < 0 || profile >= NJ_CANVAS_COUNT) profile = 0;
    int w = s_canvas_profiles[profile].w;
    int h = s_canvas_profiles[profile].h;
    if (profile == 0) {
        w = 240; h = 320; /* most common MIDP canvas when the name has no size */
        if (path) {
            if (strstr(path, "320x240")) { w = 320; h = 240; }
            else if (strstr(path, "240x320")) { w = 240; h = 320; }
            else if (strstr(path, "176x208")) { w = 176; h = 208; }
            else if (strstr(path, "176x220")) { w = 176; h = 220; }
            else if (strstr(path, "208x208")) { w = 208; h = 208; }
            else if (strstr(path, "240x400")) { w = 240; h = 400; }
            else if (strstr(path, "128x160")) { w = 128; h = 160; }
            else if (strstr(path, "128x128")) { w = 128; h = 128; }
            else if (strstr(path, "360x640")) { w = 360; h = 640; }
        }
    }
    pal_set_canvas_size(w, h);
    RG_LOGI("Nano Java canvas profile: %dx%d (setting=%d)", w, h, profile);
}

static rg_gui_event_t canvas_profile_option(rg_gui_option_t *option, rg_gui_event_t event)
{
    int profile = s_game_canvas_profile;
    if (profile < 0 || profile >= NJ_CANVAS_COUNT) profile = 0;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        /* Skip the retired sizes: they are only reachable from old configs. */
        int step = (event == RG_DIALOG_NEXT) ? 1 : -1;
        int p = profile;
        do {
            p = (p + step + NJ_CANVAS_COUNT) % NJ_CANVAS_COUNT;
        } while (!s_canvas_profiles[p].offered && p != profile);
        profile = p;
        s_game_canvas_profile = profile;
        save_game_settings();
        return RG_DIALOG_REDRAW;
    }
    if (profile == 0)
        snprintf(option->value, 32, "Auto (JAR name)");
    else
        snprintf(option->value, 32, "%ux%u", s_canvas_profiles[profile].w,
                 s_canvas_profiles[profile].h);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t rotation_option(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char *const names[] = { "Auto", "Off", "90 CW", "180", "270 CW" };
    int index = s_game_rotation + 1;
    if (index < 0 || index >= (int)(sizeof(names) / sizeof(names[0]))) index = 0;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        int count = (int)(sizeof(names) / sizeof(names[0]));
        index = (index + (event == RG_DIALOG_NEXT ? 1 : count - 1)) % count;
        s_game_rotation = index - 1;
        pal_set_display_rotation(s_game_rotation);
        save_game_settings();
        return RG_DIALOG_REDRAW;
    }
    snprintf(option->value, 32, "%s", names[index]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t placement_option(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        s_game_centered = !s_game_centered;
        pal_set_centered_output(s_game_centered != 0);
        save_game_settings();
        return RG_DIALOG_REDRAW;
    }
    snprintf(option->value, 32, "%s", s_game_centered ? "Centered 1:1" : "Stretch to screen");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t telemetry_option(rg_gui_option_t *option, rg_gui_event_t event)
{
    int enabled = rg_settings_get_number(NS_APP, "telemetry", 1) != 0;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        enabled = !enabled;
        rg_settings_set_number(NS_APP, "telemetry", enabled);
        pal_set_telemetry_enabled(enabled != 0);
        return RG_DIALOG_REDRAW;
    }
    snprintf(option->value, 32, "%s", enabled ? "On" : "Off");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t sleep_mode_option(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char *const names[] = { "Accurate", "Balanced", "Fast", "Turbo" };
    int index = s_game_sleep_mode;
    if (index < 0 || index >= NJ_SLEEP_MODE_COUNT) index = NJ_SLEEP_MODE_DEFAULT;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        index = (index + (event == RG_DIALOG_NEXT ? 1 : NJ_SLEEP_MODE_COUNT - 1)) % NJ_SLEEP_MODE_COUNT;
        s_game_sleep_mode = index;
        apply_sleep_mode();
        save_game_settings();
        return RG_DIALOG_REDRAW;
    }
    snprintf(option->value, 32, "%s", names[index]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t diag_option(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char *const names[] = { "Off", "0.5 s", "0.75 s", "1 s" };
    int index = 0;
    for (int i = 0; i < NJ_DIAG_COUNT; i++)
        if (s_diag_choices[i] == s_game_diag_ms) index = i;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        index = (index + (event == RG_DIALOG_NEXT ? 1 : NJ_DIAG_COUNT - 1)) % NJ_DIAG_COUNT;
        s_game_diag_ms = s_diag_choices[index];
        pal_set_diagonal_ms(s_game_diag_ms);
        save_game_settings();
        return RG_DIALOG_REDRAW;
    }
    snprintf(option->value, 32, "%s", names[index]);
    return RG_DIALOG_VOID;
}

static bool save_state_handler(const char *filename)
{
    (void)filename;
    rg_gui_alert("Save State", "NanoJava does not support save states yet.");
    return false;
}

static bool load_state_handler(const char *filename)
{
    (void)filename;
    rg_gui_alert("Load State", "NanoJava does not support save states yet.");
    return false;
}

static bool reset_handler(bool hard)
{
    (void)hard;
    /* Follow the reset action used by the supplied OpenTyrian wrapper. */
    rg_system_restart();
    return true; /* rg_system_restart() is non-returning */
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    (void)filename;
    (void)width;
    (void)height;
    return false; /* A stable completed-frame surface is not exposed yet. */
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, "Canvas profile*", "-", RG_DIALOG_FLAG_NORMAL, &canvas_profile_option};
    *dest++ = (rg_gui_option_t){0, "Screen rotation", "-", RG_DIALOG_FLAG_NORMAL, &rotation_option};
    *dest++ = (rg_gui_option_t){0, "Screen placement", "-", RG_DIALOG_FLAG_NORMAL, &placement_option};
    *dest++ = (rg_gui_option_t){0, "Frame pacing", "-", RG_DIALOG_FLAG_NORMAL, &sleep_mode_option};
    *dest++ = (rg_gui_option_t){0, "Keypad diagonals", "-", RG_DIALOG_FLAG_NORMAL, &diag_option};
    *dest++ = (rg_gui_option_t){0, "Performance logging", "-", RG_DIALOG_FLAG_NORMAL, &telemetry_option};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

void app_main(void)
{
    int stack_base;
    heap_set_stack_base(&stack_base);

    const rg_config_t config = {
        .sampleRate = 32000,
        .frameRate = 0,
        .storageRequired = true,
        .romRequired = true,
        .isLauncher = false,
        .handlers = {
            .loadState = &load_state_handler,
            .saveState = &save_state_handler,
            .reset = &reset_handler,
            .screenshot = &screenshot_handler,
            .options = &options_handler,
        },
        .mallocAlwaysInternal = 1024,
    };
    rg_app_t *rg_app = rg_system_init(&config);
    if (!rg_app) {
        RG_LOGE("Retro-Go system initialization failed");
        return;
    }
    /* Match OpenTyrian: only choose FULL as the default; respect saved Retro-Go scaling. */
    if (!rg_settings_exists(NS_APP, "DispScaling"))
        rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);
    pal_set_telemetry_enabled(rg_settings_get_number(NS_APP, "telemetry", 1) != 0);

    RG_LOGI("Nano Java / Retro-Go starting; internal free=%u KB, PSRAM free=%u KB",
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 1024));

    const char *rom_path = rg_system_get_app()->romPath;
    if (!rom_path || !rom_path[0])
        rom_path = "/sd/roms/java/DiamondRush_320x240.jar";
    RG_LOGI("Java ME JAR: %s", rom_path);
    load_game_settings(rom_path);
    select_canvas_size(rom_path);

    size_t classlib_size = (size_t)(_binary_nanojava_classlib_jar_end -
                                    _binary_nanojava_classlib_jar_start);
    if (!classlib_size || classlib_size > UINT32_MAX) {
        RG_LOGE("Embedded Nano Java class library is missing or invalid");
        rg_system_exit();
        return;
    }

    /* Heap sizing, as in v1.7: it scales with the JAR (images are decoded into
     * the heap) but stays moderate, because a very large heap starves PSRAM for
     * the rest of the app and is slower (cold cache).
     *
     * An earlier build raised this to "largest free PSRAM block - 2 MB" on the
     * theory that the loading-screen failures were a lack of room.  They were
     * not: they came from a class-library buffer that grew without bound (see
     * PERFORMANCE_NOTES.md, change 12).  The extra 776 KB would only have
     * changed how much memory a game sees through Runtime.freeMemory(), which
     * some titles react to, so the v1.7 numbers are kept: every game gets the
     * heap it got before and nothing can start behaving differently just
     * because the heap moved.
     *
     * heap_log_oom() still reports every failure with the requested size, the
     * free bytes and the largest free chunk, so a genuinely tight heap shows up
     * in the first log that needs it. */
    size_t jar_size = file_size(rom_path);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t heap_size = (3u << 20) + 2 * jar_size;
    if (heap_size < (4u << 20)) heap_size = 4u << 20;
    if (heap_size > NJ_HEAP_MAX) heap_size = NJ_HEAP_MAX;
    if (largest > (3u << 20) && heap_size > largest - (2u << 20))
        heap_size = largest - (2u << 20);
    heap_size &= ~(size_t)0xFFF;
    RG_LOGI("Java heap: %u KB for %u KB JAR (largest PSRAM block %u KB)",
            (unsigned)(heap_size >> 10), (unsigned)(jar_size >> 10), (unsigned)(largest >> 10));
    VMOptions options = { heap_size, 4096, 256, false, true, false };
    if (!vm_init(&options)) {
        RG_LOGE("Nano Java VM initialization failed");
        rg_system_exit();
        return;
    }
    midp_init();

    Zip *classlib = zip_open_mem(_binary_nanojava_classlib_jar_start,
                                 (uint32_t)classlib_size);
    if (!classlib) {
        RG_LOGE("Cannot open embedded Nano Java class library");
        vm_shutdown();
        midp_shutdown();
        rg_system_exit();
        return;
    }
    res_set_system(classlib);

    Zip *app = zip_open_file(rom_path);
    if (!app) {
        RG_LOGE("Cannot open Java game JAR: %s", rom_path);
        vm_shutdown();
        midp_shutdown();
        rg_system_exit();
        return;
    }
    res_add_app_zip(app);

    const char *boot_args[] = { "-midlet", "" };
    if (!vm_boot(boot_args, 2)) {
        RG_LOGE("Nano Java failed to start the MIDlet");
        vm_shutdown();
        midp_shutdown();
        rg_system_exit();
        return;
    }

    vm_poll_hook = midp_poll;
    int result = vm_run();
    if (vm.uncaught)
        result = 1;
    RG_LOGI("Nano Java game finished (status %d)", result);

    vm_shutdown();
    midp_shutdown();
    rg_system_exit();
}
