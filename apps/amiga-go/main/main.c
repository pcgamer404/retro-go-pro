/*
 * Amiga for Retro-Go.
 *
 * Retro-Go glue around the platform-independent Amiga core from
 * rlucencic/esp32ami (components/amiga).
 */
#include <rg_system.h>
#include <amiga.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUTPUT_WIDTH  320
#define OUTPUT_HEIGHT 240
#define AUDIO_CHUNK   512

/* The Amiga PAL picture is 720x568 (284 beam lines, each doubled). The
 * standard 256-line display sits 15 lines into that window; it is scaled
 * down to 240 lines. The 720 hires pixels are halved to 360 lores pixels
 * and centre-cropped to 320.
 */
#define SRC_TOP_LINE   15
#define SRC_LINES      256
#define SRC_CROP_X     40

static rg_app_t *app;
static rg_surface_t *surfaces[2];
static int surface_idx = 0;
static amiga_config_t machine;
static uint8_t *kickstart_image;
static uint32_t kickstart_size;
static char kickstart_path[RG_PATH_MAX + 1];
static uint8_t *extension_image;
static bool initialized;
static char g_scan_dir[RG_PATH_MAX + 1];
static int disk_pick_drive = -1;
static bool virtual_keyboard_requested;
static char s_extracted_disk[2][RG_PATH_MAX + 1];
static uint32_t last_buttons;
#define KEY_QUEUE_CAPACITY 8
typedef struct {
    uint8_t code;
    int frames;
} pending_key_t;
static pending_key_t s_key_queue[KEY_QUEUE_CAPACITY];
static int s_key_count;

static const char *SETTING_MODEL = "model";
static const char *SETTING_CTRL_MODE = "ctrl_mode";
static const char *SETTING_MOUSE_SPEED = "mouse_speed";
static const char *SETTING_FLOPPY_SPEED = "floppy_speed";
static const char *SETTING_FRAMESKIP = "frameskip";
static const char *SETTING_BLITTER = "blitter";
static const char *SETTING_BUS_TIMING = "bus_timing";
static const char *SETTING_SCREEN_FIT = "screen_fit";
static const char *SETTING_VERT_OFFSET = "vert_offset";

static const char *const model_names[] = {
    "A500", "A2000", "A600", "A1200", NULL
};

static const char *const ctrl_mode_names[] = {
    "Joystick", "Mouse", "Both", NULL
};

static const char *const mouse_speed_names[] = {
    "1x (Slow)", "2x (Normal)", "3x (Fast)", "5x (Turbo)", NULL
};

static const int mouse_speed_values[] = { 1, 2, 3, 5 };

static const char *const floppy_speed_names[] = {
    "1x (Authentic)", "2x (Quick)", "4x (Fast)", "8x (Turbo)", NULL
};

static const int floppy_speed_values[] = { 1, 2, 4, 8 };

static const char *const frameskip_names[] = {
    "Auto", "Off (0)", "1", "2", NULL
};

static const int frameskip_values[] = { -1, 0, 1, 2 };

static const char *const blitter_names[] = {
    "Authentic", "Fast / Immediate", NULL
};

static const char *const bus_timing_names[] = {
    "Fast / Turbo", "Authentic", NULL
};

static const char *const screen_fit_names[] = {
    "Auto (Detect)",
    "Fit 200 (Full/Lotus)",
    "Fit 216 (PAL Std)",
    "Fit 240 (1:1 Native)",
    "Fit 256 (Full PAL)",
    "Crop 200 (Centered)",
    NULL
};

static const char *const vert_offset_names[] = {
    "-12", "-8", "-6", "-4", "-2", "0 (Default)", "+2", "+4", "+6", "+8", "+12", NULL
};
static const int vert_offset_values[] = {
    -12, -8, -6, -4, -2, 0, 2, 4, 6, 8, 12
};

static void *psram_alloc(size_t size)
{
    void *ptr = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr)
        ptr = heap_caps_calloc(1, size, MALLOC_CAP_8BIT);
    if (!ptr) {
        RG_LOGE("psram_alloc failed for %u bytes! (largest free SPIRAM: %u, total free: %u)",
                (unsigned)size,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    return ptr;
}

static bool file_exists(const char *path)
{
    return rg_storage_exists(path);
}

static bool load_valid_kickstart(const char *path)
{
    void *data = NULL;
    size_t size = 0;
    int major = 0, minor = 0;

    if (!rg_storage_read_file(path, &data, &size, 0) || !data || !size)
        return false;

    if (!amiga_kickstart_version((const uint8_t *)data, (uint32_t)size, &major, &minor)) {
        free(data);
        return false;
    }

    free(kickstart_image);
    kickstart_image = data;
    kickstart_size = (uint32_t)size;
    snprintf(kickstart_path, sizeof(kickstart_path), "%s", path);
    RG_LOGI("Kickstart %d.%d from %s (%u bytes)", major, minor, path,
            (unsigned)kickstart_size);
    return true;
}

typedef struct {
    char path[2 * RG_PATH_MAX + 1];
    int score;
} rom_candidate_t;

static int rom_scan_cb(const rg_scandir_t *file, void *arg)
{
    rom_candidate_t *best = arg;
    const char *name = file->basename;

    if (!file->is_file || !name)
        return RG_SCANDIR_CONTINUE;
    if (!rg_extension_match(name, "rom bin"))
        return RG_SCANDIR_CONTINUE;
    if (strstr(name, "-ext") || strstr(name, "ext.rom"))
        return RG_SCANDIR_CONTINUE;

    int score = 0;
    if (strstr(name, "kickstart")) score += 4;
    if (strstr(name, "kick13")) score += 4;
    if (strstr(name, "amiga-os-130")) score += 5;
    if (strstr(name, "amiga-os")) score += 3;
    if (strstr(name, "aros")) score += 2;
    if (!score)
        return RG_SCANDIR_CONTINUE;

    if (score > best->score) {
        snprintf(best->path, sizeof(best->path), "%s/%s", g_scan_dir, name);
        best->score = score;
    }
    return RG_SCANDIR_CONTINUE;
}

static bool find_kickstart_in(const char *dir)
{
    static const char *const candidates[] = {
        "kickstart.rom", "kick13.rom", "kickstart13.rom",
        "amiga-os-120.rom", "amiga-os-130.rom", "amiga-os-204.rom",
        "amiga-os-205-a600.rom", "amiga-os-300-a1200.rom",
        "amiga-os-310-a600.rom", "amiga-os-310-a1200.rom",
        "aros.rom", "aros-20250422.rom", NULL
    };
    char path[RG_PATH_MAX + 1];

    for (int i = 0; candidates[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, candidates[i]);
        if (file_exists(path) && load_valid_kickstart(path))
            return true;
    }

    rom_candidate_t best = {0};
    snprintf(g_scan_dir, sizeof(g_scan_dir), "%s", dir);
    rg_storage_scandir(dir, rom_scan_cb, &best, RG_SCANDIR_FILES);
    if (best.path[0] && load_valid_kickstart(best.path))
        return true;

    return false;
}

static bool find_kickstart(void)
{
    if (find_kickstart_in(RG_BASE_PATH_BIOS))
        return true;

    char dir[RG_PATH_MAX + 1];
    if (app && app->romPath) {
        snprintf(dir, sizeof(dir), "%s", rg_dirname(app->romPath));
        if (find_kickstart_in(dir))
            return true;
    } else {
        snprintf(dir, sizeof(dir), "%s", RG_BASE_PATH_ROMS "/amiga");
        if (find_kickstart_in(dir))
            return true;
    }

    return false;
}

/* AROS needs its extension ROM ($E00000-$E7FFFF). Derive it from the
 * Kickstart's name when present; other Kickstarts do not use one.
 */
static bool find_extension(const char *kickstart_path)
{
    char path[RG_PATH_MAX + 1];
    const char *dot = strrchr(kickstart_path, '.');

    if (!dot || !strstr(kickstart_path, "aros"))
        return false;

    snprintf(path, sizeof(path), "%.*s-ext.rom", (int)(dot - kickstart_path), kickstart_path);
    if (!file_exists(path)) {
        snprintf(path, sizeof(path), "%s/aros-20250422-ext.rom", RG_BASE_PATH_BIOS);
        if (!file_exists(path))
            return false;
    }

    void *data = NULL;
    size_t size = 0;
    if (!rg_storage_read_file(path, &data, &size, 0) || size != 512 * 1024) {
        free(data);
        RG_LOGW("AROS extension ROM unusable: %s", path);
        return false;
    }
    extension_image = data;
    RG_LOGI("AROS extension ROM from %s", path);
    return true;
}

static void apply_model(int model)
{
    if (model < AMIGA_A500) model = AMIGA_A500;
    if (model > AMIGA_A1200) model = AMIGA_A1200;

    machine.model = (amiga_model_t)model;
    machine.ntsc = false;
    machine.ecs = false;
    machine.chip_size = 512 * 1024;
    machine.slow_size = 512 * 1024;
    machine.fast_size = 0;

    switch (machine.model) {
    case AMIGA_A2000:
        machine.slow_size = 0;
        machine.fast_size = 1024 * 1024;
        break;
    case AMIGA_A600:
        machine.ecs = true;
        machine.chip_size = 1024 * 1024;
        machine.slow_size = 0;
        break;
    case AMIGA_A1200:
        machine.ecs = true;
        machine.chip_size = 2048 * 1024;
        machine.slow_size = 0;
        break;
    default:
        break;
    }
}

static bool machine_start(void)
{
    if (!find_kickstart()) {
        rg_gui_alert("Amiga", "No Kickstart or AROS ROM found in\n" RG_BASE_PATH_BIOS);
        return false;
    }

    find_extension(kickstart_path);

    memset(&machine, 0, sizeof(machine));
    apply_model(rg_settings_get_number(NS_APP, SETTING_MODEL, AMIGA_A500));

    machine.chip = psram_alloc(machine.chip_size);
    machine.slow = machine.slow_size ? psram_alloc(machine.slow_size) : NULL;
    machine.fast = machine.fast_size ? psram_alloc(machine.fast_size) : NULL;
    machine.rom = psram_alloc(kickstart_size);
    machine.kickstart = kickstart_image;
    machine.kickstart_size = kickstart_size;
    machine.extension = extension_image;
    machine.ext_rom = extension_image ? psram_alloc(512 * 1024) : NULL;
    machine.picture = psram_alloc(amiga_picture_size());

    if (!machine.chip || (machine.slow_size && !machine.slow) ||
        (machine.fast_size && !machine.fast) || !machine.rom || !machine.picture ||
        (extension_image && !machine.ext_rom)) {
        RG_LOGE("Memory allocation failed: chip=%p, slow=%p, fast=%p, rom=%p, picture=%p, ext_rom=%p",
                machine.chip, machine.slow, machine.fast, machine.rom, machine.picture, machine.ext_rom);
        rg_gui_alert("Amiga", "Not enough memory to start the Amiga.");
        return false;
    }

    RG_LOGI("Amiga memory: chip=%u, slow=%u, fast=%u, rom=%u, pic=%u. Free SPIRAM: %u",
            (unsigned)machine.chip_size, (unsigned)machine.slow_size,
            (unsigned)machine.fast_size, (unsigned)kickstart_size,
            (unsigned)amiga_picture_size(),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (!amiga_init(&machine)) {
        rg_gui_alert("Amiga", "The Kickstart/AROS ROM was rejected.");
        return false;
    }

    free(kickstart_image);
    kickstart_image = NULL;
    machine.kickstart = NULL;
    free(extension_image);
    extension_image = NULL;
    machine.extension = NULL;

    initialized = true;
    return true;
}

static void machine_stop(void)
{
    if (initialized) {
        amiga_floppy_flush();
        initialized = false;
    }
    free(machine.chip);
    free(machine.slow);
    free(machine.fast);
    free(machine.rom);
    free(machine.ext_rom);
    free(machine.picture);
    memset(&machine, 0, sizeof(machine));
    free(kickstart_image);
    kickstart_image = NULL;
    free(extension_image);
    extension_image = NULL;
}

#define AUDIO_BUFFER_FRAMES 1024
static rg_audio_frame_t s_audio_buffer[AUDIO_BUFFER_FRAMES];

static void render_surface(rg_surface_t *dest_surface)
{
    amiga_screen_signature();

    const int width = amiga_screen_width();
    const int step = width >= 1440 ? 4 : 2;
    const int crop = SRC_CROP_X * (step / 2);

    int fit_mode = rg_settings_get_number(NS_APP, SETTING_SCREEN_FIT, 0); // 0 = Auto
    int vo_idx = rg_settings_get_number(NS_APP, SETTING_VERT_OFFSET, 5);  // 5 = 0 lines
    if (vo_idx < 0 || vo_idx > 10) vo_idx = 5;
    int v_offset = vert_offset_values[vo_idx];

    int src_top = 15 + v_offset;
    int src_lines = 200;
    bool centered_aspect = false;

    if (fit_mode == 0) { // Auto (Detect active DIW window)
        int win_top = 0, win_lines = 0;
        amiga_get_display_window(&win_top, &win_lines);
        if (win_lines >= 180 && win_lines <= 284 && win_top >= -10 && win_top <= 60) {
            src_top = win_top + v_offset;
            src_lines = win_lines;
        } else {
            src_lines = 200;
        }
    } else if (fit_mode == 1) { // Fit 200 (Full / Lotus 2)
        src_lines = 200;
    } else if (fit_mode == 2) { // Fit 216 (PAL Std)
        src_lines = 216;
    } else if (fit_mode == 3) { // Fit 240 (1:1 Native)
        src_lines = 240;
    } else if (fit_mode == 4) { // Fit 256 (Full PAL)
        src_lines = 256;
    } else if (fit_mode == 5) { // Crop 200 (Centered with black borders)
        src_lines = 200;
        centered_aspect = true;
    }

    if (src_top < 0) src_top = 0;
    if (src_top + src_lines > 284) src_lines = 284 - src_top;

    if (centered_aspect) {
        // 200 lines centered on 240 screen: 20 black lines top and bottom
        const int bar_lines = (OUTPUT_HEIGHT - src_lines) / 2;
        for (int y = 0; y < bar_lines; y++) {
            uint16_t *dst = (uint16_t *)((uint8_t *)dest_surface->data + y * dest_surface->stride);
            memset(dst, 0, OUTPUT_WIDTH * sizeof(uint16_t));
        }
        for (int y = 0; y < src_lines; y++) {
            int out_y = bar_lines + y;
            if (out_y >= OUTPUT_HEIGHT) break;
            const int src_row = src_top + y;
            uint16_t *dst = (uint16_t *)((uint8_t *)dest_surface->data + out_y * dest_surface->stride);
            const uint16_t *line = amiga_get_line_rgb565(src_row * 2);
            if (step == 2 && (crop + OUTPUT_WIDTH * 2) <= width) {
                const uint16_t *src = line + crop;
                uint32_t *dst32 = (uint32_t *)dst;
                for (int x = 0; x < OUTPUT_WIDTH / 2; x++) {
                    dst32[x] = src[x * 4] | (src[x * 4 + 2] << 16);
                }
            } else {
                for (int x = 0; x < OUTPUT_WIDTH; x++) {
                    int sx = crop + x * step;
                    dst[x] = (sx < width) ? line[sx] : 0;
                }
            }
        }
        for (int y = bar_lines + src_lines; y < OUTPUT_HEIGHT; y++) {
            uint16_t *dst = (uint16_t *)((uint8_t *)dest_surface->data + y * dest_surface->stride);
            memset(dst, 0, OUTPUT_WIDTH * sizeof(uint16_t));
        }
    } else {
        // Stretch src_lines across OUTPUT_HEIGHT (240 lines)
        int prev_src_row = -1;
        uint16_t *prev_dst = NULL;

        for (int y = 0; y < OUTPUT_HEIGHT; y++) {
            const int src_row = src_top + (y * src_lines) / OUTPUT_HEIGHT;
            uint16_t *dst = (uint16_t *)((uint8_t *)dest_surface->data + y * dest_surface->stride);

            if (src_row == prev_src_row && prev_dst) {
                // Duplicate line (e.g. 200 -> 240 scaling repeats ~1 line in 6): fast memcpy!
                memcpy(dst, prev_dst, OUTPUT_WIDTH * sizeof(uint16_t));
                continue;
            }

            const uint16_t *line = amiga_get_line_rgb565(src_row * 2);

            if (step == 2 && (crop + OUTPUT_WIDTH * 2) <= width) {
                const uint16_t *src = line + crop;
                uint32_t *dst32 = (uint32_t *)dst;
                for (int x = 0; x < OUTPUT_WIDTH / 2; x++) {
                    dst32[x] = src[x * 4] | (src[x * 4 + 2] << 16);
                }
            } else {
                for (int x = 0; x < OUTPUT_WIDTH; x++) {
                    int sx = crop + x * step;
                    dst[x] = (sx < width) ? line[sx] : 0;
                }
            }
            prev_src_row = src_row;
            prev_dst = dst;
        }
    }
}

static void audio_drain(void)
{
    // Submit the frame's audio in ONE single chunk to prevent driver sleep stalls
    int frames = amiga_audio_render((int16_t *)s_audio_buffer, AUDIO_BUFFER_FRAMES);
    if (frames > 0)
        rg_audio_submit(s_audio_buffer, (size_t)frames);

    // If excessive backlog exists (e.g. from pause/menu), discard extra samples
    while (amiga_audio_pending() > AUDIO_BUFFER_FRAMES)
        amiga_audio_render((int16_t *)s_audio_buffer, AUDIO_BUFFER_FRAMES);
}

static void audio_discard(void)
{
    while (amiga_audio_render((int16_t *)s_audio_buffer, AUDIO_BUFFER_FRAMES) > 0)
        ;
}

static const rg_keyboard_layout_t amiga_keyboard = {
    .layout = "1234567890"
              "qwertyuiop"
              "asdfghjkl "
              "zxcvbnm,./",
    .columns = 10,
    .rows = 4,
    .label = "Amiga",
};

static void deliver_typed_text(const char *text)
{
    amiga_type(text);
    for (int i = 0; i < 2000 && amiga_typing(); i++) {
        amiga_run_frame();
        audio_discard();
    }
    for (int i = 0; i < 2; i++) {
        amiga_run_frame();
        audio_discard();
    }

    while (rg_display_is_busy())
        rg_task_yield();
    render_surface(surfaces[surface_idx]);
    rg_display_submit(surfaces[surface_idx], 0);
    surface_idx ^= 1;
}

static void virtual_keyboard_session(void)
{
    const int count = (int)(amiga_keyboard.columns * amiga_keyboard.rows);
    int cursor = 0;

    rg_audio_set_mute(true);

    while (true) {
        rg_gui_draw_virtual_keyboard(RG_GUI_CENTER, RG_GUI_BOTTOM,
                                     &amiga_keyboard, cursor, false);
        rg_input_wait_for_key(RG_KEY_ALL, false, 1000);

        const uint32_t buttons = rg_input_read_gamepad();
        int next = cursor;

        if (buttons & (RG_KEY_B | RG_KEY_MENU | RG_KEY_OPTION))
            break;
        if (buttons & (RG_KEY_X | RG_KEY_Y)) {
            deliver_typed_text("\b");
            rg_task_delay(80);
            continue;
        }
        if (buttons & RG_KEY_A) {
            char text[2] = {amiga_keyboard.layout[cursor], 0};
            deliver_typed_text(text);
            rg_task_delay(60);
            continue;
        }
        if (buttons & RG_KEY_START) {
            deliver_typed_text("\n");
            rg_task_delay(60);
            continue;
        }
        if (buttons & RG_KEY_SELECT) {
            deliver_typed_text(" ");
            rg_task_delay(60);
            continue;
        }

        if (buttons & RG_KEY_LEFT) --next;
        else if (buttons & RG_KEY_RIGHT) ++next;
        else if (buttons & RG_KEY_UP) next -= (int)amiga_keyboard.columns;
        else if (buttons & RG_KEY_DOWN) next += (int)amiga_keyboard.columns;

        if (next >= 0 && next < count)
            cursor = next;

        rg_system_tick(0);
    }

    audio_discard();
    rg_audio_set_mute(false);
    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
}

static bool load_state_handler(const char *filename)
{
    RG_LOGW("Save states are not supported yet: %s", filename);
    return false;
}

static bool save_state_handler(const char *filename)
{
    RG_LOGW("Save states are not supported yet: %s", filename);
    return false;
}

static bool reset_handler(bool hard)
{
    if (!initialized)
        return false;
    amiga_reset();
    return true;
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    rg_surface_t *s = surfaces[surface_idx ^ 1];
    if (!s) s = surfaces[0];
    if (!s)
        return false;
    while (rg_display_is_busy())
        rg_task_yield();
    return rg_surface_save_image_file(s, filename, width, height);
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW) {
        rg_surface_t *s = surfaces[surface_idx ^ 1];
        if (!s) s = surfaces[0];
        if (s)
            rg_display_submit(s, 0);
    } else if (event == RG_EVENT_SHUTDOWN) {
        rg_audio_set_mute(true);
        machine_stop();
    }
}

static rg_gui_event_t model_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int model = rg_settings_get_number(NS_APP, SETTING_MODEL, AMIGA_A500);

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        model += (event == RG_DIALOG_NEXT) ? 1 : -1;
        if (model < AMIGA_A500) model = AMIGA_A1200;
        if (model > AMIGA_A1200) model = AMIGA_A500;
        rg_settings_set_number(NS_APP, SETTING_MODEL, model);
    }
    if (model < AMIGA_A500) model = AMIGA_A500;
    if (model > AMIGA_A1200) model = AMIGA_A1200;
    snprintf(option->value, 32, "%s", model_names[model]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t ctrl_mode_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int mode = rg_settings_get_number(NS_APP, SETTING_CTRL_MODE, 0);

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        mode += (event == RG_DIALOG_NEXT) ? 1 : -1;
        if (mode < 0) mode = 2;
        if (mode > 2) mode = 0;
        rg_settings_set_number(NS_APP, SETTING_CTRL_MODE, mode);
    }
    if (mode < 0 || mode > 2) mode = 0;
    snprintf(option->value, 32, "%s", ctrl_mode_names[mode]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t mouse_speed_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int speed_idx = rg_settings_get_number(NS_APP, SETTING_MOUSE_SPEED, 1);

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        speed_idx += (event == RG_DIALOG_NEXT) ? 1 : -1;
        if (speed_idx < 0) speed_idx = 3;
        if (speed_idx > 3) speed_idx = 0;
        rg_settings_set_number(NS_APP, SETTING_MOUSE_SPEED, speed_idx);
    }
    if (speed_idx < 0 || speed_idx > 3) speed_idx = 1;
    snprintf(option->value, 32, "%s", mouse_speed_names[speed_idx]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t floppy_speed_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int speed_idx = rg_settings_get_number(NS_APP, SETTING_FLOPPY_SPEED, 3);

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        speed_idx += (event == RG_DIALOG_NEXT) ? 1 : -1;
        if (speed_idx < 0) speed_idx = 3;
        if (speed_idx > 3) speed_idx = 0;
        rg_settings_set_number(NS_APP, SETTING_FLOPPY_SPEED, speed_idx);
        amiga_floppy_set_speed(floppy_speed_values[speed_idx]);
    }
    if (speed_idx < 0 || speed_idx > 3) speed_idx = 3;
    snprintf(option->value, 32, "%s", floppy_speed_names[speed_idx]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t disk0_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
        disk_pick_drive = 0;

    const char *path = amiga_floppy_path(0);
    snprintf(option->value, 32, "%s", path ? rg_basename(path) : "(empty)");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t eject0_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
        amiga_floppy_eject(0);
    snprintf(option->value, 32, "%s", amiga_floppy_path(0) ? "Inserted" : "(empty)");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t disk1_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
        disk_pick_drive = 1;

    const char *path = amiga_floppy_path(1);
    snprintf(option->value, 32, "%s", path ? rg_basename(path) : "(empty)");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t eject1_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
        amiga_floppy_eject(1);
    snprintf(option->value, 32, "%s", amiga_floppy_path(1) ? "Inserted" : "(empty)");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t vk_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER) {
        virtual_keyboard_requested = true;
        return RG_DIALOG_CANCEL;
    }
    snprintf(option->value, 32, "Open");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t frameskip_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int fs_idx = rg_settings_get_number(NS_APP, SETTING_FRAMESKIP, 0);

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        fs_idx += (event == RG_DIALOG_NEXT) ? 1 : -1;
        if (fs_idx < 0) fs_idx = 3;
        if (fs_idx > 3) fs_idx = 0;
        rg_settings_set_number(NS_APP, SETTING_FRAMESKIP, fs_idx);
    }
    if (fs_idx < 0 || fs_idx > 3) fs_idx = 0;
    snprintf(option->value, 32, "%s", frameskip_names[fs_idx]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t blitter_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int blit_idx = rg_settings_get_number(NS_APP, SETTING_BLITTER, 1);

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        blit_idx = !blit_idx;
        rg_settings_set_number(NS_APP, SETTING_BLITTER, blit_idx);
        amiga_blitter_set_fast(blit_idx != 0);
    }
    if (blit_idx < 0 || blit_idx > 1) blit_idx = 1;
    snprintf(option->value, 32, "%s", blitter_names[blit_idx]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t bus_timing_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int bus_idx = rg_settings_get_number(NS_APP, SETTING_BUS_TIMING, 0);

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        bus_idx = !bus_idx;
        rg_settings_set_number(NS_APP, SETTING_BUS_TIMING, bus_idx);
        amiga_mem_set_fast_bus(bus_idx == 0);
    }
    if (bus_idx < 0 || bus_idx > 1) bus_idx = 0;
    snprintf(option->value, 32, "%s", bus_timing_names[bus_idx]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t screen_fit_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int fit_idx = rg_settings_get_number(NS_APP, SETTING_SCREEN_FIT, 0);

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        fit_idx += (event == RG_DIALOG_NEXT) ? 1 : -1;
        if (fit_idx < 0) fit_idx = 5;
        if (fit_idx > 5) fit_idx = 0;
        rg_settings_set_number(NS_APP, SETTING_SCREEN_FIT, fit_idx);
    }
    if (fit_idx < 0 || fit_idx > 5) fit_idx = 0;
    snprintf(option->value, 32, "%s", screen_fit_names[fit_idx]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t vert_offset_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int vo_idx = rg_settings_get_number(NS_APP, SETTING_VERT_OFFSET, 5); // default 5 = "0"

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        vo_idx += (event == RG_DIALOG_NEXT) ? 1 : -1;
        if (vo_idx < 0) vo_idx = 10;
        if (vo_idx > 10) vo_idx = 0;
        rg_settings_set_number(NS_APP, SETTING_VERT_OFFSET, vo_idx);
    }
    if (vo_idx < 0 || vo_idx > 10) vo_idx = 5;
    snprintf(option->value, 32, "%s", vert_offset_names[vo_idx]);
    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, "Control Mode", "-",
                                RG_DIALOG_FLAG_NORMAL, &ctrl_mode_update_cb};
    *dest++ = (rg_gui_option_t){0, "Mouse Speed", "-",
                                RG_DIALOG_FLAG_NORMAL, &mouse_speed_update_cb};
    *dest++ = (rg_gui_option_t){0, "Floppy Speed", "-",
                                RG_DIALOG_FLAG_NORMAL, &floppy_speed_update_cb};
    *dest++ = (rg_gui_option_t){0, "Frameskip", "-",
                                RG_DIALOG_FLAG_NORMAL, &frameskip_update_cb};
    *dest++ = (rg_gui_option_t){0, "Blitter", "-",
                                RG_DIALOG_FLAG_NORMAL, &blitter_update_cb};
    *dest++ = (rg_gui_option_t){0, "Bus Timing", "-",
                                RG_DIALOG_FLAG_NORMAL, &bus_timing_update_cb};
    *dest++ = (rg_gui_option_t){0, "Screen Fit", "-",
                                RG_DIALOG_FLAG_NORMAL, &screen_fit_update_cb};
    *dest++ = (rg_gui_option_t){0, "Vertical Offset", "-",
                                RG_DIALOG_FLAG_NORMAL, &vert_offset_update_cb};
    *dest++ = (rg_gui_option_t){0, "Virtual Keyboard", "-",
                                RG_DIALOG_FLAG_NORMAL, &vk_update_cb};
    *dest++ = (rg_gui_option_t){0, "Model (restart)", "-",
                                RG_DIALOG_FLAG_NORMAL, &model_update_cb};
    *dest++ = (rg_gui_option_t){0, "Disk 1 (DF0)", "-",
                                RG_DIALOG_FLAG_NORMAL, &disk0_update_cb};
    *dest++ = (rg_gui_option_t){0, "Eject DF0", "-",
                                RG_DIALOG_FLAG_NORMAL, &eject0_update_cb};
    *dest++ = (rg_gui_option_t){0, "Disk 2 (DF1)", "-",
                                RG_DIALOG_FLAG_NORMAL, &disk1_update_cb};
    *dest++ = (rg_gui_option_t){0, "Eject DF1", "-",
                                RG_DIALOG_FLAG_NORMAL, &eject1_update_cb};
    *dest = (rg_gui_option_t)RG_DIALOG_END;
}

static bool disk_validator(const char *path)
{
    return rg_extension_match(path, "adf rp9 zip");
}

static const char *prepare_floppy_path(int drive, const char *path)
{
    if (!path || !path[0])
        return NULL;

    if (rg_extension_match(path, "zip")) {
        void *content = NULL;
        size_t content_size = 0;
        if (!rg_storage_unzip_file(path, NULL, &content, &content_size, 0) || !content || !content_size) {
            RG_LOGE("Failed to extract disk from ZIP: %s", path);
            free(content);
            return NULL;
        }

        rg_storage_mkdir(RG_BASE_PATH_CACHE);
        snprintf(s_extracted_disk[drive], sizeof(s_extracted_disk[drive]),
                 "%s/amiga_df%d.adf", RG_BASE_PATH_CACHE, drive);
        bool ok = rg_storage_write_file(s_extracted_disk[drive], content, content_size, RG_FILE_ATOMIC_WRITE);
        free(content);
        if (!ok) {
            RG_LOGE("Failed to write extracted disk cache: %s", s_extracted_disk[drive]);
            return NULL;
        }
        RG_LOGI("Extracted %u bytes from ZIP to %s", (unsigned)content_size, s_extracted_disk[drive]);
        return s_extracted_disk[drive];
    }

    return path;
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

static void inspect_bootblock(int drive, const char *path)
{
    if (!path) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t bb[1024];
    size_t n = fread(bb, 1, sizeof(bb), f);
    fclose(f);
    if (n < sizeof(bb)) return;

    uint32_t sum = 0;
    for (int i = 0; i < 256; i++) {
        uint32_t v = ((uint32_t)bb[i * 4] << 24) | ((uint32_t)bb[i * 4 + 1] << 16) |
                     ((uint32_t)bb[i * 4 + 2] << 8) | (uint32_t)bb[i * 4 + 3];
        if (UINT32_MAX - sum < v)
            sum++;
        sum += v;
    }

    bool is_dos = (memcmp(bb, "DOS", 3) == 0);
    bool valid_boot = (sum == 0xFFFFFFFF);
    const char *base = rg_basename(path);

    RG_LOGI("Floppy DF%d: '%s' [%.3s], boot-checksum=0x%08X (%s)",
            drive, base, is_dos ? (const char *)bb : "NDOS/Raw",
            (unsigned)sum, valid_boot ? "Bootable" : "Non-bootable data/addon");

    if (drive == 0 && (str_contains_nocase(base, "data disk") ||
                       str_contains_nocase(base, "data_disk") ||
                       str_contains_nocase(base, "scenery") ||
                       str_contains_nocase(base, "course") ||
                       str_contains_nocase(base, "disk 2") ||
                       str_contains_nocase(base, "disk2") ||
                       str_contains_nocase(base, "disk b") ||
                       (!valid_boot && sum == 0))) {
        RG_LOGW("DF0 disk appears to be a non-bootable data/expansion disk. Kickstart 1.3 will wait for bootable Disk 1!");
    }
}

static void disk_picker(int drive)
{
    if (drive < 0 || drive >= AMIGA_FLOPPIES)
        return;

    char start_path[RG_PATH_MAX + 1];
    const char *current = amiga_floppy_path(drive);

    if (current) {
        snprintf(start_path, sizeof(start_path), "%s", rg_dirname(current));
    } else if (app->romPath) {
        snprintf(start_path, sizeof(start_path), "%s", rg_dirname(app->romPath));
    } else {
        snprintf(start_path, sizeof(start_path), "%s/amiga", RG_BASE_PATH_ROMS);
    }

    char *path = rg_gui_file_picker("Insert disk", start_path, &disk_validator, false, false);
    if (path) {
        const char *ready_path = prepare_floppy_path(drive, path);
        if (!ready_path || !amiga_floppy_insert(drive, ready_path)) {
            rg_gui_alert("Amiga", "Unable to insert this floppy image.");
        } else {
            inspect_bootblock(drive, ready_path);
        }
        free(path);
    }
}

static void queue_key(uint8_t code)
{
    for (int i = 0; i < s_key_count; i++) {
        if (s_key_queue[i].code == code) {
            s_key_queue[i].frames = 5;
            return;
        }
    }
    if (s_key_count < KEY_QUEUE_CAPACITY) {
        s_key_queue[s_key_count].code = code;
        s_key_queue[s_key_count].frames = 5;
        amiga_key(code, true);
        s_key_count++;
    }
}

static void release_all_queued_keys(void)
{
    for (int i = 0; i < s_key_count; i++) {
        amiga_key(s_key_queue[i].code, false);
    }
    s_key_count = 0;
}

static void process_queued_keys(void)
{
    for (int i = 0; i < s_key_count; i++) {
        if (--s_key_queue[i].frames <= 0) {
            amiga_key(s_key_queue[i].code, false);
            for (int j = i; j < s_key_count - 1; j++) {
                s_key_queue[j] = s_key_queue[j + 1];
            }
            s_key_count--;
            i--;
        }
    }
}

static uint8_t gamepad_to_joystick(uint32_t buttons)
{
    uint8_t joystick = 0;

    if (buttons & RG_KEY_UP)    joystick |= AMIGA_JOY_UP;
    if (buttons & RG_KEY_DOWN)  joystick |= AMIGA_JOY_DOWN;
    if (buttons & RG_KEY_LEFT)  joystick |= AMIGA_JOY_LEFT;
    if (buttons & RG_KEY_RIGHT) joystick |= AMIGA_JOY_RIGHT;
    if (buttons & (RG_KEY_A | RG_KEY_B | RG_KEY_START)) joystick |= AMIGA_JOY_FIRE;
    if (buttons & (RG_KEY_X | RG_KEY_Y)) joystick |= AMIGA_JOY_FIRE2;

    return joystick;
}

static bool insert_content(const char *path)
{
    if (!path || !path[0])
        return true;

    if (rg_extension_match(path, "adf rp9 zip")) {
        const char *ready_path = prepare_floppy_path(0, path);
        if (!ready_path || !amiga_floppy_insert(0, ready_path)) {
            rg_gui_alert("Amiga", "Unable to insert this floppy image.");
            return false;
        }
        inspect_bootblock(0, ready_path);
        return true;
    }

    if (rg_extension_match(path, "hdf")) {
        if (!amiga_hd_insert(0, path)) {
            rg_gui_alert("Amiga", "Unable to insert this hard disk image.");
            return false;
        }
        return true;
    }

    rg_gui_alert("Amiga", "Unsupported content. Use ADF, RP9, HDF or ZIP.");
    return false;
}

void app_main(void)
{
    const rg_config_t system_config = {
        .sampleRate = AMIGA_AUDIO_HZ,
        .frameRate = 50,
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
        .mallocAlwaysInternal = 0,
    };

    app = rg_system_init(&system_config);
    app->frameskip = -1;

    if (!rg_settings_exists(NS_APP, "DispScaling"))
        rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);

    for (int i = 0; i < 2; i++) {
        surfaces[i] = rg_surface_create(OUTPUT_WIDTH, OUTPUT_HEIGHT, RG_PIXEL_565_LE, MEM_SLOW);
        if (!surfaces[i]) {
            rg_gui_alert("Amiga", "Unable to allocate the output surface.");
            rg_system_exit();
            return;
        }
    }

    if (!machine_start()) {
        rg_system_exit();
        return;
    }

    int f_speed_idx = rg_settings_get_number(NS_APP, SETTING_FLOPPY_SPEED, 3);
    if (f_speed_idx < 0 || f_speed_idx > 3) f_speed_idx = 3;
    amiga_floppy_set_speed(floppy_speed_values[f_speed_idx]);
    RG_LOGI("Floppy emulation speed initialized to %dx (%s)",
            floppy_speed_values[f_speed_idx], floppy_speed_names[f_speed_idx]);

    int blit_idx = rg_settings_get_number(NS_APP, SETTING_BLITTER, 1);
    if (blit_idx < 0 || blit_idx > 1) blit_idx = 1;
    amiga_blitter_set_fast(blit_idx != 0);
    RG_LOGI("Blitter mode initialized to: %s", blitter_names[blit_idx]);

    int bus_idx = rg_settings_get_number(NS_APP, SETTING_BUS_TIMING, 0); // default 0 = "Fast / Turbo"
    if (bus_idx < 0 || bus_idx > 1) bus_idx = 0;
    amiga_mem_set_fast_bus(bus_idx == 0);
    RG_LOGI("Bus timing mode initialized to: %s", bus_timing_names[bus_idx]);

    int fit_idx = rg_settings_get_number(NS_APP, SETTING_SCREEN_FIT, 0);
    if (fit_idx < 0 || fit_idx > 5) fit_idx = 0;
    RG_LOGI("Screen fit initialized to: %s", screen_fit_names[fit_idx]);

    if (!insert_content(app->romPath)) {
        rg_system_exit();
        return;
    }

    amiga_audio_enable(true);

    unsigned skip_frames = 0;
    int64_t prof_emu = 0, prof_render = 0, prof_disp = 0, prof_audio = 0, prof_total = 0;
    int prof_frames = 0, prof_drawn = 0;

    while (true) {
        const int64_t started_us = rg_system_timer();
        const uint32_t buttons = rg_input_read_gamepad();

        const uint32_t pressed = buttons & ~last_buttons;
        last_buttons = buttons;

        if (buttons & (RG_KEY_MENU | RG_KEY_OPTION)) {
            rg_audio_set_mute(true);
            if (buttons & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
            rg_audio_set_mute(false);
            if (virtual_keyboard_requested) {
                virtual_keyboard_requested = false;
                virtual_keyboard_session();
            }
            if (disk_pick_drive >= 0) {
                int d = disk_pick_drive;
                disk_pick_drive = -1;
                disk_picker(d);
            }
            last_buttons = rg_input_read_gamepad();
            while (rg_input_read_gamepad() & (RG_KEY_MENU | RG_KEY_OPTION))
                rg_task_delay(20);
            continue;
        }

        // 1. Chords with SELECT:
        static bool select_used_as_modifier = false;
        if (pressed & RG_KEY_SELECT) {
            select_used_as_modifier = false;
        }

        // SELECT + Y or SELECT + START: Open virtual keyboard
        if ((buttons & RG_KEY_SELECT) && (pressed & (RG_KEY_Y | RG_KEY_START))) {
            select_used_as_modifier = true;
            release_all_queued_keys();
            virtual_keyboard_session();
            last_buttons = rg_input_read_gamepad();
            continue;
        }

        // SELECT + A: Quick toggle Control Mode (Joystick <-> Mouse)
        if ((buttons & RG_KEY_SELECT) && (pressed & RG_KEY_A)) {
            select_used_as_modifier = true;
            int mode = rg_settings_get_number(NS_APP, SETTING_CTRL_MODE, 0);
            mode = (mode == 0) ? 1 : 0;
            rg_settings_set_number(NS_APP, SETTING_CTRL_MODE, mode);
            RG_LOGI("Control mode switched to: %s", ctrl_mode_names[mode]);
            last_buttons = rg_input_read_gamepad();
            continue;
        }

        // Quick Trainer shortcuts when holding SELECT:
        // SELECT + UP    = Key '1' (0x01)
        // SELECT + RIGHT = Key '2' (0x02)
        // SELECT + DOWN  = Key '3' (0x03)
        // SELECT + LEFT  = Key '4' (0x04)
        // SELECT + B     = Key 'Y' (0x15)
        // SELECT + X     = Key 'N' (0x36)
        if (buttons & RG_KEY_SELECT) {
            if (pressed & RG_KEY_UP)    { select_used_as_modifier = true; queue_key(0x01); }
            if (pressed & RG_KEY_RIGHT) { select_used_as_modifier = true; queue_key(0x02); }
            if (pressed & RG_KEY_DOWN)  { select_used_as_modifier = true; queue_key(0x03); }
            if (pressed & RG_KEY_LEFT)  { select_used_as_modifier = true; queue_key(0x04); }
            if (pressed & RG_KEY_B)     { select_used_as_modifier = true; queue_key(0x15); }
            if (pressed & RG_KEY_X)     { select_used_as_modifier = true; queue_key(0x36); }
        }

        // If SELECT was tapped and released alone without modifier: send Space
        if ((last_buttons & RG_KEY_SELECT) && !(buttons & RG_KEY_SELECT) && !select_used_as_modifier) {
            queue_key(0x40); /* Space */
        }

        // 2. Direct button presses (when SELECT is not held):
        if (!(buttons & RG_KEY_SELECT)) {
            // X = Space Bar (Key 0x40) - Essential for Amiga cracktros ("Press Space"), Turrican line/wheel, Battle Squadron bombs, etc.
            if (pressed & RG_KEY_X) {
                queue_key(0x40);
            }
            // Y = Return / Enter (Key 0x44)
            if (pressed & RG_KEY_Y) {
                queue_key(0x44);
            }
            // START = Universal Start / Skip (sends Return + Space, and pulses Left Mouse Button + Fire)
            if (pressed & RG_KEY_START) {
                queue_key(0x44); // Return
                queue_key(0x40); // Space
            }
        }

        process_queued_keys();

        // Handle controls according to selected mode
        int ctrl_mode = rg_settings_get_number(NS_APP, SETTING_CTRL_MODE, 0);
        int speed_idx = rg_settings_get_number(NS_APP, SETTING_MOUSE_SPEED, 1);
        if (speed_idx < 0 || speed_idx > 3) speed_idx = 1;
        int speed = mouse_speed_values[speed_idx];
        if (buttons & (RG_KEY_X | RG_KEY_Y)) speed *= 2; // Turbo boost

        uint8_t mouse_buttons = 0;
        // Universal button mapping:
        // Button A or START or L = Left Mouse Click (Port 1)!
        // Button B or R = Right Mouse Click (Port 1)!
        if (buttons & (RG_KEY_A | RG_KEY_START | RG_KEY_L))
            mouse_buttons |= 0x01;
        if (buttons & (RG_KEY_B | RG_KEY_R))
            mouse_buttons |= 0x02;

        if (ctrl_mode == 0) {
            // Joystick mode (Primary A fires joystick & mouse, D-pad directs joystick)
            amiga_joystick(gamepad_to_joystick(buttons));
            amiga_mouse(0, 0, mouse_buttons);
        } else if (ctrl_mode == 1) {
            // Mouse mode (D-pad moves mouse cursor)
            amiga_joystick(0);
            int mouse_dx = 0;
            int mouse_dy = 0;
            if (buttons & RG_KEY_UP)    mouse_dy -= speed;
            if (buttons & RG_KEY_DOWN)  mouse_dy += speed;
            if (buttons & RG_KEY_LEFT)  mouse_dx -= speed;
            if (buttons & RG_KEY_RIGHT) mouse_dx += speed;
            amiga_mouse(mouse_dx, mouse_dy, mouse_buttons);
        } else {
            // Both mode (D-pad moves both, buttons fire both)
            amiga_joystick(gamepad_to_joystick(buttons));
            int mouse_dx = 0;
            int mouse_dy = 0;
            if (buttons & RG_KEY_UP)    mouse_dy -= speed;
            if (buttons & RG_KEY_DOWN)  mouse_dy += speed;
            if (buttons & RG_KEY_LEFT)  mouse_dx -= speed;
            if (buttons & RG_KEY_RIGHT) mouse_dx += speed;
            amiga_mouse(mouse_dx, mouse_dy, mouse_buttons);
        }

        // Frameskip setting: 0 = Auto (-1), 1 = Off (0), 2 = 1, 3 = 2
        int fs_setting = rg_settings_get_number(NS_APP, SETTING_FRAMESKIP, 0);
        int target_skip = (fs_setting >= 0 && fs_setting <= 3) ? frameskip_values[fs_setting] : -1;

        // 1. Emulate frame
        const int64_t emu_start = rg_system_timer();
        amiga_run_frame();
        prof_emu += rg_system_timer() - emu_start;

        // 2. Render decision (check display busy after emulation finishes)
        bool render_video = (skip_frames == 0);
        if (render_video && rg_display_is_busy()) {
            render_video = false;
        }

        if (render_video) {
            rg_surface_t *cur_surface = surfaces[surface_idx];
            surface_idx ^= 1;

            const int64_t render_start = rg_system_timer();
            render_surface(cur_surface);
            prof_render += rg_system_timer() - render_start;

            const int64_t disp_start = rg_system_timer();
            rg_display_submit(cur_surface, 0);
            prof_disp += rg_system_timer() - disp_start;
            prof_drawn++;
        }

        // 3. Audio drain - single chunk submission to avoid driver sleep stalls
        const int64_t audio_start = rg_system_timer();
        audio_drain();
        prof_audio += rg_system_timer() - audio_start;

        const int64_t frame_total_us = rg_system_timer() - started_us;
        prof_total += frame_total_us;

        // Report active frame time to Retro-Go monitor
        rg_system_tick((int)frame_total_us);

        // 4. Frameskip management
        if (skip_frames > 0) {
            skip_frames--;
        } else {
            if (target_skip > 0) {
                skip_frames = target_skip;
            } else if (target_skip == -1) {
                // Auto: if frame took longer than 20ms (PAL 50Hz budget), skip 1 frame
                if (frame_total_us > 21500) {
                    skip_frames = 1;
                }
            }
        }

        if (++prof_frames >= 120) {
            RG_LOGI("PERF: emu=%d us, render=%d us, disp=%d us, audio=%d us, total=%d us | drawn=%d/120",
                    (int)(prof_emu / prof_frames),
                    (int)(prof_render / prof_frames),
                    (int)(prof_disp / prof_frames),
                    (int)(prof_audio / prof_frames),
                    (int)(prof_total / prof_frames),
                    prof_drawn);
            prof_emu = prof_render = prof_disp = prof_audio = prof_total = 0;
            prof_frames = 0;
            prof_drawn = 0;
        }
    }
}
