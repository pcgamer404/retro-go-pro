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
#include "../../src/vm/dynarec.h"
#include "../../src/pal/pal.h"
#include "../../src/pal/alloc.h"
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
void pal_set_action_key_mode(int mode);
void pal_set_button_x_mode(int mode);
void pal_set_dpad_mode(int mode);

/* Per-game display settings live separately from Retro-Go's global app settings. */
static char s_game_cfg_path[256];
static int s_game_canvas_profile;
static int s_game_rotation = -1;
static int s_game_centered = 1;
static int s_game_sleep_mode = 2;  /* index into s_sleep_modes[] */
static int s_game_diag_ms;         /* 0 = diagonal keys off */
static int s_game_action_key = 0;  /* 0=FIRE (-5), 1=Keypad 5 ('5'), 2=Both */
static int s_game_dpad = 1;       /* 0=Arrows, 1=Arrows + keypad 2/4/6/8, 2=Keypad 2/4/6/8 only */
static int s_game_button_x = 0;    /* 0=Soft Right (-7), 1=Keypad # ('#'), 2=Soft Left (-6) */

static bool s_is_mjar = false;
static int s_mjar_startup_menu = 2; /* 0 = Ask every time, 1 = Last saved choice, 2 = Auto-start (5s) */
static int s_mjar_crashed = 0;      /* 1 = previous launch crashed */
static char s_mjar_last_variant[64] = "";
static char s_active_variant_name[64] = "";

static bool is_mjar_file(const char *path)
{
    if (!path) return false;
    size_t len = strlen(path);
    return (len > 5 && strcasecmp(path + len - 5, ".mjar") == 0);
}

static bool str_contains_nocase(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return false;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (strncasecmp(haystack + i, needle, nlen) == 0)
            return true;
    }
    return false;
}


/* Java heap sizing: an upper bound so a device with plenty of PSRAM does not
 * hand all of it to the VM (and the 2 MB that the clamp below leaves to
 * everything else). */
#define NJ_HEAP_MAX           (6u << 20)

/* Diagonals as number keys ("Keypad diagonals"). Games like Prince of Persia,
 * Assassin's Creed, Splinter Cell, etc. require 1/3/7/9 for diagonal jump and roll.
 * "Gameplay": instantaneous/debounced D-pad diagonals (1/3/7/9) and L/R shoulders (1/3).
 * "Hold 0.5 s" / "Hold 1.0 s": holding a diagonal presses 1/3/7/9 for menu screens. */
static const int s_diag_choices[] = { 0, 30, 500, 1000 };
#define NJ_DIAG_COUNT     ((int)(sizeof(s_diag_choices) / sizeof(s_diag_choices[0])))
#define NJ_DIAG_DEFAULT   0

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
    {   0, 1 },   /* Turbo: every sleep becomes pure yield (0 ms) */
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
    if (n > 5 && strcasecmp(base + n - 5, ".mjar") == 0) n -= 5;
    else if (n > 4 && (strcasecmp(base + n - 4, ".jar") == 0 || strcasecmp(base + n - 4, ".zip") == 0)) n -= 4;
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

typedef enum {
    NJ_GENRE_STANDARD = 0,
    NJ_GENRE_RACING,      /* 3D polygon racers: Asphalt, NFS, Ridge Racer, Ferrari, etc. */
    NJ_GENRE_OPEN_WORLD,  /* Open world streaming: Gangstar, Saints Row, GTA, etc. */
    NJ_GENRE_PLATFORMER,  /* Precision platformers: Prince of Persia, Assassin's Creed, etc. */
    NJ_GENRE_RPG          /* Menu/turn-based RPGs: Final Fantasy, Doom RPG, Mafia, etc. */
} nj_game_genre_t;

static nj_game_genre_t s_game_genre = NJ_GENRE_STANDARD;
static const char *s_genre_names[] = { "Standard", "Racing (3D)", "Open World", "Platformer", "RPG" };

static nj_game_genre_t detect_game_genre(const char *path)
{
    if (!path) return NJ_GENRE_STANDARD;
    if (str_contains_nocase(path, "asphalt") ||
        str_contains_nocase(path, "need for speed") || str_contains_nocase(path, "nfs") ||
        str_contains_nocase(path, "ferrari") || str_contains_nocase(path, "ridge racer") ||
        str_contains_nocase(path, "burnout") || str_contains_nocase(path, "driver") ||
        str_contains_nocase(path, "rally") || str_contains_nocase(path, "colin mcrae") ||
        str_contains_nocase(path, "moto gp") || str_contains_nocase(path, "fast & furious") ||
        str_contains_nocase(path, "fast and furious") || str_contains_nocase(path, "fast_furious")) {
        return NJ_GENRE_RACING;
    }
    if (str_contains_nocase(path, "gangstar") ||
        str_contains_nocase(path, "saints row") || str_contains_nocase(path, "saints_row") ||
        str_contains_nocase(path, "gta") || str_contains_nocase(path, "grand theft") ||
        str_contains_nocase(path, "true crime") || str_contains_nocase(path, "miami nights") ||
        str_contains_nocase(path, "spider-man") || str_contains_nocase(path, "spiderman")) {
        return NJ_GENRE_OPEN_WORLD;
    }
    if (str_contains_nocase(path, "prince of persia") || str_contains_nocase(path, "prince_of_persia") ||
        str_contains_nocase(path, "assassin") || str_contains_nocase(path, "splinter cell") ||
        str_contains_nocase(path, "splinter_cell") || str_contains_nocase(path, "castlevania") ||
        str_contains_nocase(path, "ninja prophecy") || str_contains_nocase(path, "soul of darkness") ||
        str_contains_nocase(path, "zombie infection") || str_contains_nocase(path, "persia") ||
        str_contains_nocase(path, "far cry") || str_contains_nocase(path, "far_cry")) {
        return NJ_GENRE_PLATFORMER;
    }
    if (str_contains_nocase(path, "final fantasy") || str_contains_nocase(path, "final_fantasy") ||
        str_contains_nocase(path, "doom rpg") || str_contains_nocase(path, "doom_rpg") ||
        str_contains_nocase(path, "wolfenstein") || str_contains_nocase(path, "heroes of lore") ||
        str_contains_nocase(path, "mafia") || str_contains_nocase(path, "sonic advance")) {
        return NJ_GENRE_RPG;
    }
    return NJ_GENRE_STANDARD;
}

static void load_game_settings(const char *rom_path)
{
    s_is_mjar = is_mjar_file(rom_path);
    s_mjar_startup_menu = 2; /* Default: 2 = Auto-start (5s) */
    s_mjar_crashed = 0;
    s_mjar_last_variant[0] = '\0';

    make_game_cfg_path(rom_path);
    s_game_canvas_profile = 0;
    s_game_rotation = -1;
    s_game_centered = 1;
    s_game_sleep_mode = NJ_SLEEP_MODE_DEFAULT;
    s_game_diag_ms = s_diag_choices[NJ_DIAG_DEFAULT];
    s_game_action_key = 0;
    s_game_button_x = 0;
    s_game_dpad = 1;

    s_game_genre = detect_game_genre(rom_path);
    switch (s_game_genre) {
    case NJ_GENRE_RACING:
        s_game_sleep_mode = 3; /* Turbo: 0 ms sleep / pure yield eliminates 46ms timer stall */
        break;
    case NJ_GENRE_OPEN_WORLD:
        s_game_sleep_mode = 3; /* Turbo: 0 ms sleep / pure yield eliminates movement lag */
        s_game_diag_ms = 30;   /* Responsive 8-way movement */
        break;
    case NJ_GENRE_PLATFORMER:
        s_game_diag_ms = 30;   /* Gameplay keypad diagonals for jump/roll */
        break;
    case NJ_GENRE_RPG:
        s_game_action_key = 1; /* Keypad 5 default for menus */
        break;
    default:
        break;
    }

    int cfg_ver = 1;
    FILE *f = fopen(s_game_cfg_path, "r");
    if (f) {
        char line[160];
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = line;
            char *val = eq + 1;
            size_t vlen = strlen(val);
            while (vlen > 0 && (val[vlen - 1] == '\r' || val[vlen - 1] == '\n' || val[vlen - 1] == ' '))
                val[--vlen] = '\0';
            int value = atoi(val);

            if (!strcmp(key, "cfg_ver")) cfg_ver = value;
            else if (!strcmp(key, "canvas_profile") && value >= 0 && value < NJ_CANVAS_COUNT) s_game_canvas_profile = value;
            else if (!strcmp(key, "rotation") && value >= -1 && value <= 3) s_game_rotation = value;
            else if (!strcmp(key, "centered") && (value == 0 || value == 1)) s_game_centered = value;
            else if (!strcmp(key, "sleep_mode") && value >= 0 && value < NJ_SLEEP_MODE_COUNT) s_game_sleep_mode = value;
            else if (!strcmp(key, "diag_ms")) {
                for (int i = 0; i < NJ_DIAG_COUNT; i++)
                    if (value == s_diag_choices[i]) s_game_diag_ms = value;
                if (value == 750) s_game_diag_ms = 500;
            }
            else if (!strcmp(key, "action_key") && value >= 0 && value <= 2) s_game_action_key = value;
            else if (!strcmp(key, "dpad_mode") && value >= 0 && value <= 2) s_game_dpad = value;
            else if (!strcmp(key, "button_x") && value >= 0 && value <= 2) s_game_button_x = value;
            else if (!strcmp(key, "mjar_startup") && value >= 0 && value <= 2) s_mjar_startup_menu = value;
            else if (!strcmp(key, "mjar_crashed") && (value == 0 || value == 1)) s_mjar_crashed = value;
            else if (!strcmp(key, "mjar_variant") && val[0]) {
                strncpy(s_mjar_last_variant, val, sizeof(s_mjar_last_variant) - 1);
                s_mjar_last_variant[sizeof(s_mjar_last_variant) - 1] = '\0';
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

    /* Version < 3 upgrade: If this game was previously saved with default
     * sleep_mode=2 (Fast) and belongs to the Racing or Open World genre,
     * seamlessly upgrade it to Turbo (sleep_mode=3) so timer waits don't bottleneck FPS. */
    if (cfg_ver < 3) {
        if ((s_game_genre == NJ_GENRE_RACING || s_game_genre == NJ_GENRE_OPEN_WORLD) &&
            s_game_sleep_mode == NJ_SLEEP_MODE_DEFAULT) {
            s_game_sleep_mode = 3; /* Turbo */
        }
        if (s_game_genre == NJ_GENRE_OPEN_WORLD && s_game_diag_ms == 0) {
            s_game_diag_ms = 30;
        }
    }
    pal_set_display_rotation(s_game_rotation);
    pal_set_centered_output(s_game_centered != 0);
    pal_set_diagonal_ms(s_game_diag_ms);
    pal_set_action_key_mode(s_game_action_key);
    pal_set_button_x_mode(s_game_button_x);
    pal_set_dpad_mode(s_game_dpad);
    apply_sleep_mode();
    RG_LOGI("J2ME per-game config: %s (genre=%s, canvas=%d rotation=%d centered=%d sleep_mode=%d diag_ms=%d action_key=%d button_x=%d mjar_startup=%d mjar_crashed=%d mjar_variant=%s)",
            s_game_cfg_path, s_genre_names[s_game_genre], s_game_canvas_profile, s_game_rotation, s_game_centered,
            s_game_sleep_mode, s_game_diag_ms, s_game_action_key, s_game_button_x,
            s_mjar_startup_menu, s_mjar_crashed, s_mjar_last_variant);
}

static void save_game_settings(void)
{
    if (!s_game_cfg_path[0]) return;
    FILE *f = fopen(s_game_cfg_path, "w");
    if (!f) { RG_LOGW("Cannot save per-game J2ME settings: %s", s_game_cfg_path); return; }
    fprintf(f, "cfg_ver=3\ncanvas_profile=%d\nrotation=%d\ncentered=%d\nsleep_mode=%d\ndiag_ms=%d\naction_key=%d\nbutton_x=%d\ndpad_mode=%d\n",
            s_game_canvas_profile, s_game_rotation, s_game_centered, s_game_sleep_mode,
            s_game_diag_ms, s_game_action_key, s_game_button_x, s_game_dpad);
    if (s_is_mjar) {
        fprintf(f, "mjar_startup=%d\nmjar_crashed=%d\nmjar_variant=%s\n",
                s_mjar_startup_menu, s_mjar_crashed,
                s_mjar_last_variant[0] ? s_mjar_last_variant : s_active_variant_name);
    }
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

static uint8_t *s_variant_data = NULL;
static size_t s_variant_size = 0;

static const char *const s_auto_res_priority[] = {
    "320x240", "208x208", "176x220", "176x208", "128x160", "128x128", "240x320"
};

static intptr_t mjar_pick_dialog(const char *title, const rg_gui_option_t *options_const, int selected_index, int timeout_seconds)
{
    rg_gui_option_t *options = (rg_gui_option_t *)options_const;
    size_t options_count = 0;
    while (options_const[options_count].flags != 0)
        options_count++;

    if (options_count == 0) return RG_DIALOG_CANCELLED;

    int sel = RG_MIN(RG_MAX(0, selected_index), (int)options_count - 1);
    int sel_old = -1;

    char title_buf[64];
    int remaining = timeout_seconds;
    int64_t timer_start = esp_timer_get_time();

    if (remaining > 0)
        snprintf(title_buf, sizeof(title_buf), "%s (%ds)", title, remaining);
    else
        snprintf(title_buf, sizeof(title_buf), "%s", title);

    rg_gui_draw_status_bars();
    rg_gui_draw_dialog(title_buf, options, options_count, sel);
    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
    rg_task_delay(80);

    uint32_t joystick = 0, joystick_old;
    uint64_t joystick_last = 0;
    rg_gui_event_t event = RG_DIALOG_VOID;

    while (event != RG_DIALOG_SELECT && event != RG_DIALOG_CANCEL)
    {
        joystick_old = ((rg_system_timer() - joystick_last) > 300000) ? 0 : joystick;
        joystick = rg_input_read_gamepad();

        if (joystick ^ joystick_old)
        {
            if (joystick & (RG_KEY_UP | RG_KEY_DOWN | RG_KEY_LEFT | RG_KEY_RIGHT)) {
                /* User moved cursor: cancel countdown */
                if (remaining > 0) {
                    remaining = 0;
                    snprintf(title_buf, sizeof(title_buf), "%s", title);
                    rg_gui_draw_dialog(title_buf, options, options_count, sel);
                }
                if (joystick & RG_KEY_UP) {
                    if (--sel < 0) sel = (int)options_count - 1;
                } else if (joystick & RG_KEY_DOWN) {
                    if (++sel > (int)options_count - 1) sel = 0;
                }
            } else if (joystick & (RG_KEY_B | RG_KEY_OPTION | RG_KEY_MENU)) {
                event = RG_DIALOG_CANCEL;
            } else if (joystick & RG_KEY_A) {
                event = RG_DIALOG_SELECT;
            }
            joystick_last = rg_system_timer();
        }

        /* Countdown timer tick */
        if (remaining > 0) {
            int elapsed = (int)((esp_timer_get_time() - timer_start) / 1000000);
            int new_rem = timeout_seconds - elapsed;
            if (new_rem <= 0) {
                event = RG_DIALOG_SELECT;
            } else if (new_rem != remaining) {
                remaining = new_rem;
                snprintf(title_buf, sizeof(title_buf), "%s (%ds)", title, remaining);
                rg_gui_draw_dialog(title_buf, options, options_count, sel);
            }
        }

        if (sel_old != sel) {
            sel_old = sel;
            rg_gui_draw_dialog(title_buf, options, options_count, sel);
        }

        rg_task_delay(20);
        rg_system_tick(0);
    }

    rg_input_wait_for_key(joystick, false, 1000);
    rg_display_force_redraw();

    if (event == RG_DIALOG_CANCEL)
        return RG_DIALOG_CANCELLED;

    return options[sel].arg;
}

static Zip *load_app_zip(const char *rom_path, int profile)
{
    Zip *outer = zip_open_file(rom_path);
    if (!outer) return NULL;

    /* Standard standalone .jar file - cache in PSRAM only for small JARs (<= 512 KB)
     * so large titles (1 MB - 2 MB JARs like Asphalt 6, Spider-Man, Gangstar Rio)
     * preserve physical PSRAM for Java heap and the 1 MB JIT code cache. */
    if (!s_is_mjar) {
        s_active_variant_name[0] = '\0';
        s_variant_size = file_size(rom_path);
        if (s_variant_size > 0 && s_variant_size <= 512 * 1024) {
            FILE *f = fopen(rom_path, "rb");
            if (f) {
                uint8_t *mem = (uint8_t *)nj_pal_malloc(s_variant_size);
                if (mem) {
                    if (fread(mem, 1, s_variant_size, f) == s_variant_size) {
                        fclose(f);
                        zip_close(outer);
                        s_variant_data = mem;
                        Zip *in_mem_jar = zip_open_mem(mem, (uint32_t)s_variant_size);
                        if (in_mem_jar) {
                            RG_LOGI("Cached standalone .jar in PSRAM (%u KB)", (unsigned)(s_variant_size >> 10));
                            return in_mem_jar;
                        }
                    }
                    nj_pal_free(mem);
                    s_variant_data = NULL;
                }
                fclose(f);
            }
        }
        return outer;
    }

    uint32_t count = zip_count(outer);
    ZipEntry *jar_entries[32];
    int jar_count = 0;
    for (uint32_t i = 0; i < count && jar_count < 32; i++) {
        ZipEntry *e = zip_entry(outer, i);
        if (!e || !e->name) continue;
        size_t len = strlen(e->name);
        if (len > 4 && strcasecmp(e->name + len - 4, ".jar") == 0) {
            jar_entries[jar_count++] = e;
        }
    }

    /* Fallback if an .mjar contained no inner jars */
    if (jar_count == 0) {
        s_active_variant_name[0] = '\0';
        s_variant_size = file_size(rom_path);
        return outer;
    }

    ZipEntry *chosen = NULL;

    /* Single resolution in .mjar -> auto launch seamlessly without asking! */
    if (jar_count == 1) {
        chosen = jar_entries[0];
        const char *slash = strrchr(chosen->name, '/');
        const char *vname = slash ? slash + 1 : chosen->name;
        strncpy(s_active_variant_name, vname, sizeof(s_active_variant_name) - 1);
        s_active_variant_name[sizeof(s_active_variant_name) - 1] = '\0';
        strncpy(s_mjar_last_variant, vname, sizeof(s_mjar_last_variant) - 1);
        s_mjar_last_variant[sizeof(s_mjar_last_variant) - 1] = '\0';

        uint32_t inner_sz = 0;
        s_variant_data = zip_read(outer, chosen, &inner_sz);
        zip_close(outer);

        if (!s_variant_data || inner_sz == 0) {
            RG_LOGE("Multi-version: failed to extract '%s'", s_active_variant_name);
            if (s_variant_data) {
                nj_pal_free(s_variant_data);
                s_variant_data = NULL;
            }
            return NULL;
        }

        s_variant_size = (size_t)inner_sz;
        RG_LOGI("Single-variant .mjar container: auto-launching '%s' (%u KB)",
                s_active_variant_name, (unsigned)(s_variant_size >> 10));

        Zip *inner = zip_open_mem(s_variant_data, inner_sz);
        if (!inner) {
            RG_LOGE("Multi-version: failed to parse inner ZIP '%s'", s_active_variant_name);
            nj_pal_free(s_variant_data);
            s_variant_data = NULL;
            return NULL;
        }

        return inner;
    }

    /* Locate the index of the last selected resolution */
    int last_idx = -1;
    if (s_mjar_last_variant[0]) {
        for (int i = 0; i < jar_count; i++) {
            const char *slash = strrchr(jar_entries[i]->name, '/');
            const char *vname = slash ? slash + 1 : jar_entries[i]->name;
            if (strcasecmp(vname, s_mjar_last_variant) == 0) {
                last_idx = i;
                break;
            }
        }
    }
    if (last_idx == -1 && profile > 0 && profile < NJ_CANVAS_COUNT) {
        char target[16];
        snprintf(target, sizeof(target), "%ux%u", s_canvas_profiles[profile].w, s_canvas_profiles[profile].h);
        for (int i = 0; i < jar_count; i++) {
            if (str_contains_nocase(jar_entries[i]->name, target)) {
                last_idx = i;
                break;
            }
        }
    }
    if (last_idx == -1) {
        for (size_t p = 0; p < sizeof(s_auto_res_priority) / sizeof(s_auto_res_priority[0]); p++) {
            for (int i = 0; i < jar_count; i++) {
                if (str_contains_nocase(jar_entries[i]->name, s_auto_res_priority[p])) {
                    last_idx = i;
                    break;
                }
            }
            if (last_idx >= 0) break;
        }
    }
    if (last_idx == -1) last_idx = 0;

    /* Check prompt conditions:
     * - User held SELECT or Y on boot -> always prompt
     * - Previous launch crashed (s_mjar_crashed == 1) -> prompt crash recovery
     * - Startup Menu is 'Ask every time' (s_mjar_startup_menu == 0) -> prompt
     * - Startup Menu is 'Auto-start (5s)' (s_mjar_startup_menu == 2) -> prompt with 5s timer
     * - Startup Menu is 'Last saved choice' (s_mjar_startup_menu == 1) -> do NOT prompt!
     */
    uint32_t boot_keys = rg_input_read_gamepad();
    bool force_prompt = (boot_keys & (RG_KEY_SELECT | RG_KEY_Y)) || (s_mjar_crashed == 1);
    bool should_prompt = force_prompt || (s_mjar_startup_menu == 0) || (s_mjar_startup_menu == 2);

    if (should_prompt) {
        rg_gui_option_t opts[33];
        memset(opts, 0, sizeof(opts));
        int opt_cnt = 0;

        /* Move the last selected resolution to the very top (index 0) */
        const char *s_top = strrchr(jar_entries[last_idx]->name, '/');
        opts[opt_cnt].arg = last_idx;
        opts[opt_cnt].label = s_top ? s_top + 1 : jar_entries[last_idx]->name;
        opts[opt_cnt].flags = RG_DIALOG_FLAG_NORMAL;
        opt_cnt++;

        /* Place the remaining variants below */
        for (int i = 0; i < jar_count; i++) {
            if (i == last_idx) continue;
            const char *s = strrchr(jar_entries[i]->name, '/');
            opts[opt_cnt].arg = i;
            opts[opt_cnt].label = s ? s + 1 : jar_entries[i]->name;
            opts[opt_cnt].flags = RG_DIALOG_FLAG_NORMAL;
            opt_cnt++;
        }
        opts[opt_cnt] = (rg_gui_option_t)RG_DIALOG_END;

        int timeout = 0;
        if (s_mjar_startup_menu == 2 && !force_prompt) {
            timeout = 5; /* 5-second countdown */
        }

        const char *title = s_mjar_crashed ? "Crash Recovery: Pick Res" : "Select Resolution";
        intptr_t sel_idx = mjar_pick_dialog(title, opts, 0, timeout);
        if (sel_idx >= 0 && sel_idx < jar_count) {
            chosen = jar_entries[sel_idx];
        } else {
            chosen = jar_entries[last_idx];
        }

        s_mjar_crashed = 0;
    } else {
        /* Last saved choice mode: launch saved variant directly without prompt */
        chosen = jar_entries[last_idx];
    }

    const char *slash = strrchr(chosen->name, '/');
    const char *vname = slash ? slash + 1 : chosen->name;
    strncpy(s_active_variant_name, vname, sizeof(s_active_variant_name) - 1);
    s_active_variant_name[sizeof(s_active_variant_name) - 1] = '\0';
    strncpy(s_mjar_last_variant, vname, sizeof(s_mjar_last_variant) - 1);
    s_mjar_last_variant[sizeof(s_mjar_last_variant) - 1] = '\0';

    for (int p = 1; p < NJ_CANVAS_COUNT; p++) {
        char target[16];
        snprintf(target, sizeof(target), "%ux%u", s_canvas_profiles[p].w, s_canvas_profiles[p].h);
        if (str_contains_nocase(chosen->name, target)) {
            s_game_canvas_profile = p;
            break;
        }
    }
    save_game_settings();

    uint32_t inner_sz = 0;
    s_variant_data = zip_read(outer, chosen, &inner_sz);
    zip_close(outer);

    if (!s_variant_data || inner_sz == 0) {
        RG_LOGE("Multi-version: failed to extract '%s'", s_active_variant_name);
        if (s_variant_data) {
            nj_pal_free(s_variant_data);
            s_variant_data = NULL;
        }
        return NULL;
    }

    s_variant_size = (size_t)inner_sz;
    RG_LOGI("Multi-version container: loaded '%s' (%u KB)",
            s_active_variant_name, (unsigned)(s_variant_size >> 10));

    Zip *inner = zip_open_mem(s_variant_data, inner_sz);
    if (!inner) {
        RG_LOGE("Multi-version: failed to parse inner ZIP '%s'", s_active_variant_name);
        nj_pal_free(s_variant_data);
        s_variant_data = NULL;
        return NULL;
    }

    return inner;
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
    if (profile == 0) {
        if (s_active_variant_name[0]) {
            const char *res = NULL;
            for (size_t i = 0; i < sizeof(s_auto_res_priority) / sizeof(s_auto_res_priority[0]); i++) {
                if (strstr(s_active_variant_name, s_auto_res_priority[i])) {
                    res = s_auto_res_priority[i];
                    break;
                }
            }
            if (res)
                snprintf(option->value, 32, "Auto (%s)", res);
            else
                snprintf(option->value, 32, "Auto (%.18s)", s_active_variant_name);
        } else {
            snprintf(option->value, 32, "Auto (JAR name)");
        }
    } else {
        snprintf(option->value, 32, "%ux%u", s_canvas_profiles[profile].w,
                 s_canvas_profiles[profile].h);
    }
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
    int enabled = rg_settings_get_number(NS_APP, "telemetry", 0) != 0;
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

static rg_gui_event_t button_x_option(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char *const names[] = { "Soft Right (-7)", "Keypad # ('#')", "Soft Left (-6)" };
    int index = s_game_button_x;
    if (index < 0 || index >= 3) index = 0;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        index = (index + (event == RG_DIALOG_NEXT ? 1 : 2)) % 3;
        s_game_button_x = index;
        pal_set_button_x_mode(s_game_button_x);
        save_game_settings();
        return RG_DIALOG_REDRAW;
    }
    snprintf(option->value, 32, "%s", names[index]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t dpad_option(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char *const names[] = { "Arrows", "Arrows + 2/4/6/8", "Keypad 2/4/6/8" };
    int index = s_game_dpad;
    if (index < 0 || index > 2) index = 0;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        index = (index + (event == RG_DIALOG_NEXT ? 1 : 2)) % 3;
        s_game_dpad = index;
        pal_set_dpad_mode(s_game_dpad);
        save_game_settings();
        return RG_DIALOG_REDRAW;
    }
    snprintf(option->value, 32, "%s", names[index]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t mjar_startup_option(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char *const names[] = { "Ask every time", "Last saved choice", "Auto-start (5s)" };
    int index = s_mjar_startup_menu;
    if (index < 0 || index > 2) index = 2;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        index = (index + (event == RG_DIALOG_NEXT ? 1 : 2)) % 3;
        s_mjar_startup_menu = index;
        save_game_settings();
        return RG_DIALOG_REDRAW;
    }
    snprintf(option->value, 32, "%s", names[index]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t diag_option(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char *const names[] = { "Off", "Gameplay", "Hold 0.5 s", "Hold 1.0 s" };
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

static void options_handler(rg_gui_option_t *dest)
{
    if (s_is_mjar) {
        *dest++ = (rg_gui_option_t){0, "MJAR Variant*", "-", RG_DIALOG_FLAG_NORMAL, &canvas_profile_option};
        *dest++ = (rg_gui_option_t){0, "Startup Menu", "-", RG_DIALOG_FLAG_NORMAL, &mjar_startup_option};
    } else {
        *dest++ = (rg_gui_option_t){0, "Canvas profile*", "-", RG_DIALOG_FLAG_NORMAL, &canvas_profile_option};
    }
    *dest++ = (rg_gui_option_t){0, "Screen rotation", "-", RG_DIALOG_FLAG_NORMAL, &rotation_option};
    *dest++ = (rg_gui_option_t){0, "Screen placement", "-", RG_DIALOG_FLAG_NORMAL, &placement_option};
    *dest++ = (rg_gui_option_t){0, "D-pad keys", "-", RG_DIALOG_FLAG_NORMAL, &dpad_option};
    *dest++ = (rg_gui_option_t){0, "Keypad diagonals", "-", RG_DIALOG_FLAG_NORMAL, &diag_option};
    *dest++ = (rg_gui_option_t){0, "Button X mapping", "-", RG_DIALOG_FLAG_NORMAL, &button_x_option};
    *dest++ = (rg_gui_option_t){0, "Frame pacing", "-", RG_DIALOG_FLAG_NORMAL, &sleep_mode_option};
    *dest++ = (rg_gui_option_t){0, "Performance logging", "-", RG_DIALOG_FLAG_NORMAL, &telemetry_option};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

static void event_handler(int event, void *arg)
{
    (void)arg;
    if (event == RG_EVENT_SHUTDOWN) {
        pal_audio_shutdown();
        pal_present_shutdown();
    }
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
            .event = &event_handler,
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

    Zip *app = load_app_zip(rom_path, s_game_canvas_profile);
    if (!app) {
        RG_LOGE("Cannot open Java game JAR/archive: %s", rom_path);
        rg_system_exit();
        return;
    }
    select_canvas_size(s_active_variant_name[0] ? s_active_variant_name : rom_path);

    size_t classlib_size = (size_t)(_binary_nanojava_classlib_jar_end -
                                    _binary_nanojava_classlib_jar_start);
    if (!classlib_size || classlib_size > UINT32_MAX) {
        RG_LOGE("Embedded Nano Java class library is missing or invalid");
        if (s_variant_data) {
            nj_pal_free(s_variant_data);
            s_variant_data = NULL;
        }
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
    size_t jar_size = s_variant_size;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Native reserve from largest free PSRAM block:
     * - JIT executable code cache (JIT_CODE_CACHE_SIZE, up to 1024 KB)
     * - Dynarec compiler scratch buffers (~100 KB)
     * - Class/method/string descriptors, audio, and graphics buffers (~1300 KB)
     * - Safety headroom (~384 KB)
     * Total native reserve = JIT_CODE_CACHE_SIZE + 1792 KB.
     * Note: s_variant_data is ALREADY allocated in PSRAM before this point,
     * so `largest` ALREADY accounts for it. We do NOT subtract s_variant_size again. */
    size_t native_reserve = JIT_CODE_CACHE_SIZE + (1024u << 10);
    size_t target_heap = (3584u << 10) + (jar_size * 2);
    if (target_heap < (4096u << 10))
        target_heap = (4096u << 10);

    /* Genre-specific heap sizing on 8 MB PSRAM devices (largest >= 7500 KB):
     * 1. NJ_GENRE_PLATFORMER (Assassin's Creed, Prince of Persia):
     *    2D games with massive asset packages (e.g. 286 KB chunk).
     *    They have no 3D engine, no Z-buffer, and minimal native footprint (~850 KB).
     *    They require 6,144 KB (6.0 MB) Java heap to prevent loading stalls.
     * 2. NJ_GENRE_RACING (Asphalt Nitro, Asphalt 6):
     *    Full 3D games with software rasterizer (150 KB Z-buffer), M3G textures,
     *    and heavy native memory demands. Their Java heap is moderate (4608 KB).
     *    Capping at 4,608 KB preserves >2.9 MB of native PSRAM, preventing
     *    out-of-native-memory crashes.
     * 3. NJ_GENRE_OPEN_WORLD (Gangstar Rio):
     *    Large world + 300+ methods. 5,120 KB (5.0 MB) Java heap provides
     *    ample room for both Java and native allocations (>2.4 MB native headroom).
     * 4. Standard / RPG / other JARs:
     *    4,096 KB (or 4,480 KB for large JARs > 500 KB).
     */
    if (largest >= (7500u << 10)) {
        if (s_game_genre == NJ_GENRE_PLATFORMER) {
            target_heap = (6144u << 10);
        } else if (s_game_genre == NJ_GENRE_OPEN_WORLD) {
            target_heap = (5120u << 10);
        } else if (s_game_genre == NJ_GENRE_RACING) {
            target_heap = (4608u << 10);
        } else if (jar_size > (500u << 10)) {
            target_heap = (4480u << 10);
        } else {
            target_heap = (4096u << 10);
        }
    } else {
        if (target_heap > (4096u << 10))
            target_heap = (4096u << 10);
    }

    size_t heap_size = target_heap;
    if (largest > native_reserve && heap_size > largest - native_reserve)
        heap_size = largest - native_reserve;
    if (heap_size < (3072u << 10))
        heap_size = (3072u << 10);
    if (largest > (512u << 10) && heap_size > largest - (512u << 10))
        heap_size = largest - (512u << 10);
    heap_size &= ~(size_t)0xFFF;
    RG_LOGI("Profile: %s | Java heap: %u KB for %u KB JAR (largest PSRAM block %u KB)",
            s_genre_names[s_game_genre],
            (unsigned)(heap_size >> 10), (unsigned)(jar_size >> 10), (unsigned)(largest >> 10));
    VMOptions options = { heap_size, 4096, 256, false, true, false };
    if (!vm_init(&options)) {
        RG_LOGE("Nano Java VM initialization failed");
        if (s_variant_data) {
            nj_pal_free(s_variant_data);
            s_variant_data = NULL;
        }
        rg_system_exit();
        return;
    }
    midp_init();

    Zip *classlib = zip_open_mem(_binary_nanojava_classlib_jar_start,
                                 (uint32_t)classlib_size);
    if (!classlib) {
        RG_LOGE("Cannot open embedded Nano Java class library");
        pal_audio_shutdown();
        pal_present_shutdown();
        midp_shutdown();
        vm_shutdown();
        if (s_variant_data) {
            nj_pal_free(s_variant_data);
            s_variant_data = NULL;
        }
        rg_system_exit();
        return;
    }
    res_set_system(classlib);
    res_add_app_zip(app);

    /* Check for an accompanying JAD file */
    bool jad_loaded = false;
    char jad_path[RG_PATH_MAX];
    strncpy(jad_path, rom_path, sizeof(jad_path) - 1);
    jad_path[sizeof(jad_path) - 1] = '\0';
    char *dot = strrchr(jad_path, '.');
    if (dot && (strcasecmp(dot, ".jar") == 0 || strcasecmp(dot, ".mjar") == 0 || strcasecmp(dot, ".zip") == 0)) {
        strcpy(dot, ".jad");
        FILE *jf = fopen(jad_path, "rb");
        if (jf) {
            fseek(jf, 0, SEEK_END);
            long sz = ftell(jf);
            fseek(jf, 0, SEEK_SET);
            if (sz > 0 && sz < 65536) {
                uint8_t *jad_buf = malloc(sz);
                if (jad_buf) {
                    if (fread(jad_buf, 1, sz, jf) == (size_t)sz) {
                        RG_LOGI("Loaded JAD file: %s (%ld bytes)", jad_path, sz);
                        res_set_jad(jad_buf, sz);
                        jad_loaded = true;
                    }
                    free(jad_buf);
                }
            }
            fclose(jf);
        }
    }
    if (!jad_loaded) {
        /* Default Gameloft & common JAD properties if no .jad is present */
        static const char s_default_jad[] =
            "Blood-Censor: 0\r\n"
            "Cop-Censor: 0\r\n"
            "URL-TEMPLATE-GAME: \r\n"
            "URL-GLIVE: \r\n"
            "URL-PT: \r\n"
            "URL-PROMO: \r\n"
            "IGP-CATEGORIES: \r\n";
        res_set_jad((const uint8_t *)s_default_jad, strlen(s_default_jad));
    }

    if (s_is_mjar) {
        /* Set crash flag before boot. If boot or execution crashes,
         * the flag stays 1 so next boot will display the variant picker. */
        s_mjar_crashed = 1;
        save_game_settings();
    }

    const char *boot_args[] = { "-midlet", "" };
    if (!vm_boot(boot_args, 2)) {
        RG_LOGE("Nano Java failed to start the MIDlet");
        if (s_is_mjar && s_active_variant_name[0]) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Failed to boot %s\nSelect another variant on next launch.", s_active_variant_name);
            rg_gui_alert("Boot Failure", msg);
        }
        pal_audio_shutdown();
        pal_present_shutdown();
        midp_shutdown();
        vm_shutdown();
        if (s_variant_data) {
            nj_pal_free(s_variant_data);
            s_variant_data = NULL;
        }
        rg_system_exit();
        return;
    }

    vm_poll_hook = midp_poll;
    int result = vm_run();
    if (vm.uncaught)
        result = 1;
    RG_LOGI("Nano Java game finished (status %d)", result);

    if (result != 0) {
        if (s_is_mjar && s_active_variant_name[0]) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Variant failed: %s\nSelect another resolution on next launch.", s_active_variant_name);
            rg_gui_alert("Game Error", msg);
        }
    } else {
        if (s_is_mjar) {
            s_mjar_crashed = 0;
            save_game_settings();
        }
    }

    /* 1. Stop background audio and display tasks first while VM and memory are intact */
    pal_audio_shutdown();
    pal_present_shutdown();

    /* 2. Tear down MIDP and VM */
    midp_shutdown();
    vm_shutdown();
    if (s_variant_data) {
        nj_pal_free(s_variant_data);
        s_variant_data = NULL;
    }
    rg_system_exit();
}
