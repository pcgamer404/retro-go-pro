/*
 * main.c - Retro-Go frontend for Sinclair ZX Spectrum (zx-go)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <rg_system.h>
#include <rg_display.h>
#include <rg_audio.h>
#include <rg_input.h>
#include <rg_gui.h>
#include <rg_settings.h>

#include "spectemu.h"

#define AUDIO_SAMPLE_RATE 15600
#define PAL_FRAME_RATE    50

static rg_app_t *app = NULL;
static rg_surface_t *output = NULL;
static bool output_valid = false;
static zx_joy_type_t joystick_mode = ZX_JOY_KEMPSTON;

// Standard ZX Spectrum 40-key keyboard layout for Retro-Go virtual keyboard picker
static const rg_keyboard_layout_t ZX_KEYBOARD_LAYOUT = {
    .layout = "1234567890"
              "qwertyuiop"
              "asdfghjkl "
              "zxcvbnm\n ?",
    .columns = 10,
    .rows = 4,
    .label = "ZX Keyboard"
};

static const char *joy_names[ZX_JOY_COUNT] = {
    "Kempston",
    "Sinclair 1",
    "Sinclair 2",
    "Cursor",
    "QAOP Space"
};

static void virtual_keyboard_session(void)
{
    rg_audio_set_mute(true);
    while (true) {
        int key = rg_gui_input_char(&ZX_KEYBOARD_LAYOUT);
        if (key <= 0) break;

        spectemu_inject_key((char)key);
        rg_task_delay(100);
    }
    rg_audio_set_mute(false);
    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
}

typedef enum {
    ZX_SCREEN_FULL = 0,    // 256x192 Borderless (Retro-Go upscales to 320x240 full screen)
    ZX_SCREEN_BORDER = 1,  // 320x240 Classic Sinclair border
    ZX_SCREEN_COUNT
} zx_screen_mode_t;

static zx_screen_mode_t screen_mode = ZX_SCREEN_FULL;
static const char *screen_mode_names[ZX_SCREEN_COUNT] = {
    "Full Screen",
    "With Border"
};

static void apply_screen_mode(zx_screen_mode_t mode)
{
    screen_mode = mode;
    rg_settings_set_number(NS_APP, "screen_mode", screen_mode);

    int target_w = (screen_mode == ZX_SCREEN_FULL) ? 256 : 320;
    int target_h = (screen_mode == ZX_SCREEN_FULL) ? 192 : 240;

    if (output && output->width == target_w && output->height == target_h) {
        return;
    }

    while (rg_display_is_busy()) rg_task_yield();
    if (output) {
        rg_surface_free(output);
        output = NULL;
    }
    output = rg_surface_create(target_w, target_h, RG_PIXEL_565_LE, MEM_SLOW);
}

static rg_gui_event_t screen_mode_update(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        apply_screen_mode((zx_screen_mode_t)((screen_mode + 1) % ZX_SCREEN_COUNT));
    }
    snprintf(option->value, 16, "%s", screen_mode_names[screen_mode]);
    return RG_DIALOG_VOID;
}

static const char *s_quick_keys[] = {"1", "2", "3", "4", "5", "Space", "Enter", "Y", "N"};
static int s_quick_key_idx = 0;

static rg_gui_event_t quick_key_update(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV) {
        s_quick_key_idx = (s_quick_key_idx + 9 - 1) % 9;
    } else if (event == RG_DIALOG_NEXT) {
        s_quick_key_idx = (s_quick_key_idx + 1) % 9;
    } else if (event == RG_DIALOG_ENTER || event == RG_DIALOG_SELECT) {
        char k = s_quick_keys[s_quick_key_idx][0];
        if (strcmp(s_quick_keys[s_quick_key_idx], "Space") == 0) k = ' ';
        else if (strcmp(s_quick_keys[s_quick_key_idx], "Enter") == 0) k = '\n';
        spectemu_inject_key(k);
        return RG_DIALOG_SELECT;
    }
    snprintf(option->value, 16, "%s", s_quick_keys[s_quick_key_idx]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t joy_mode_update(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV) {
        joystick_mode = (zx_joy_type_t)((joystick_mode + ZX_JOY_COUNT - 1) % ZX_JOY_COUNT);
        rg_settings_set_number(NS_APP, "joy_mode", joystick_mode);
    } else if (event == RG_DIALOG_NEXT) {
        joystick_mode = (zx_joy_type_t)((joystick_mode + 1) % ZX_JOY_COUNT);
        rg_settings_set_number(NS_APP, "joy_mode", joystick_mode);
    }
    snprintf(option->value, 16, "%s", joy_names[joystick_mode]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t keyboard_trigger(rg_gui_option_t *option, rg_gui_event_t event)
{
    (void)option;
    if (event == RG_DIALOG_ENTER || event == RG_DIALOG_SELECT) {
        virtual_keyboard_session();
        return RG_DIALOG_SELECT;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t type_load_trigger(rg_gui_option_t *option, rg_gui_event_t event)
{
    (void)option;
    if (event == RG_DIALOG_ENTER || event == RG_DIALOG_SELECT) {
        spectemu_inject_string("J\"\"\n");
        return RG_DIALOG_SELECT;
    }
    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *options)
{
    *options++ = (rg_gui_option_t){
        0, "Screen Mode", "-", RG_DIALOG_FLAG_NORMAL, &screen_mode_update
    };
    *options++ = (rg_gui_option_t){
        1, "Joystick", "-", RG_DIALOG_FLAG_NORMAL, &joy_mode_update
    };
    *options++ = (rg_gui_option_t){
        2, "Type Key", "-", RG_DIALOG_FLAG_NORMAL, &quick_key_update
    };
    *options++ = (rg_gui_option_t){
        3, "Virtual Keyboard", "Open", RG_DIALOG_FLAG_NORMAL, &keyboard_trigger
    };
    *options++ = (rg_gui_option_t){
        4, "Type LOAD \"\"", "Run", RG_DIALOG_FLAG_NORMAL, &type_load_trigger
    };
    *options = (rg_gui_option_t)RG_DIALOG_END;
}

static const char *prepare_rom_path(const char *path)
{
    if (!path || !path[0])
        return NULL;

    if (rg_extension_match(path, "zip")) {
        char inner_ext[16] = ".tap"; // default fallback
        FILE *fp = fopen(path, "rb");
        if (fp) {
            uint8_t hdr[30];
            if (fread(hdr, 1, 30, fp) == 30 && hdr[0] == 'P' && hdr[1] == 'K' && hdr[2] == 3 && hdr[3] == 4) {
                uint16_t fn_len = hdr[26] | (hdr[27] << 8);
                if (fn_len > 0 && fn_len < 256) {
                    char fname[256];
                    if (fread(fname, 1, fn_len, fp) == fn_len) {
                        fname[fn_len] = '\0';
                        const char *dot = strrchr(fname, '.');
                        if (dot && strlen(dot) < 8) {
                            strncpy(inner_ext, dot, sizeof(inner_ext) - 1);
                        }
                    }
                }
            }
            fclose(fp);
        }

        // If inner_ext didn't identify extension, check double extension e.g. "game.tap.zip" or "game.tzx.zip"
        if (strcmp(inner_ext, ".tap") == 0) {
            const char *dot_zip = strrchr(path, '.');
            if (dot_zip && dot_zip > path) {
                const char *p = dot_zip - 1;
                while (p > path && *p != '.' && *p != '/' && *p != '\\') p--;
                if (*p == '.') {
                    char sub_ext[16] = {0};
                    size_t len = dot_zip - p;
                    if (len < sizeof(sub_ext)) {
                        strncpy(sub_ext, p, len);
                        if (rg_extension_match(sub_ext, "tap tzx z80 sna")) {
                            strncpy(inner_ext, sub_ext, sizeof(inner_ext) - 1);
                        }
                    }
                }
            }
        }

        void *content = NULL;
        size_t content_size = 0;
        if (!rg_storage_unzip_file(path, NULL, &content, &content_size, 0) || !content || !content_size) {
            RG_LOGE("Failed to extract ZX Spectrum file from ZIP: %s", path);
            free(content);
            return NULL;
        }

        rg_storage_mkdir(RG_BASE_PATH_CACHE);
        static char s_extracted_rom[256];
        snprintf(s_extracted_rom, sizeof(s_extracted_rom), "%s/zx_extracted%s",
                 RG_BASE_PATH_CACHE, inner_ext);
        bool ok = rg_storage_write_file(s_extracted_rom, content, content_size, RG_FILE_ATOMIC_WRITE);
        free(content);
        if (!ok) {
            RG_LOGE("Failed to write extracted ZX Spectrum file: %s", s_extracted_rom);
            return NULL;
        }
        RG_LOGI("Extracted %u bytes to %s", (unsigned)content_size, s_extracted_rom);
        return s_extracted_rom;
    }

    return path;
}

static bool load_state_handler(const char *filename)
{
    const char *ready_path = prepare_rom_path(filename);
    return spectemu_load_file(ready_path ? ready_path : filename);
}

static bool save_state_handler(const char *filename)
{
    return spectemu_save_snapshot(filename);
}

static bool reset_handler(bool hard)
{
    spectemu_reset(hard);
    return true;
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    while (rg_display_is_busy()) rg_task_yield();
    return output_valid &&
           rg_surface_save_image_file(output, filename, width, height);
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW && output_valid) {
        while (rg_display_is_busy()) rg_task_yield();
        rg_display_submit(output, 0);
    } else if (event == RG_EVENT_SHUTDOWN) {
        rg_audio_set_mute(true);
        while (rg_display_is_busy()) rg_task_yield();
        if (output) {
            rg_surface_free(output);
            output = NULL;
        }
    }
}

void app_main(void)
{
    const rg_config_t system_config = {
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameRate = PAL_FRAME_RATE,
        .storageRequired = true,
        .romRequired = true,
        .isLauncher = false,
        .handlers = {
            .loadState = &load_state_handler,
            .saveState = &save_state_handler,
            .reset = &reset_handler,
            .screenshot = &screenshot_handler,
            .event = &event_handler,
            .memRead = NULL,
            .memWrite = NULL,
            .options = &options_handler,
            .about = NULL,
        },
        .mallocAlwaysInternal = 0,
    };

    app = rg_system_init(&system_config);
    app->frameskip = 0;

    screen_mode = (zx_screen_mode_t)rg_settings_get_number(NS_APP, "screen_mode", ZX_SCREEN_FULL);
    joystick_mode = (zx_joy_type_t)rg_settings_get_number(NS_APP, "joy_mode", ZX_JOY_KEMPSTON);

    if (!rg_settings_exists(NS_APP, "DispScaling")) {
        rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);
    }

    int init_w = (screen_mode == ZX_SCREEN_FULL) ? 256 : 320;
    int init_h = (screen_mode == ZX_SCREEN_FULL) ? 192 : 240;
    output = rg_surface_create(init_w, init_h, RG_PIXEL_565_LE, MEM_SLOW);
    if (!output) {
        rg_gui_alert("ZX Spectrum", "Failed to allocate display surface.");
        rg_system_exit();
        return;
    }

    spectemu_init();

    bool tape_autoload_queued = false;
    int tape_boot_counter = 0;

    // Load ROM / Snapshot if specified
    if (app->romPath && app->romPath[0]) {
        RG_LOGI("Loading ZX Spectrum file: %s", app->romPath);
        const char *ready_path = prepare_rom_path(app->romPath);
        if (!ready_path || !spectemu_load_file(ready_path)) {
            char err[128];
            snprintf(err, sizeof(err), "Could not load file:\n%s", app->romPath);
            rg_gui_alert("Load Error", err);
        } else if (rg_extension_match(ready_path, "tap tzx")) {
            tape_autoload_queued = true;
            tape_boot_counter = 0;
            RG_LOGI("Tape loaded, queued auto LOAD \"\"");
        }
    }

    int16_t audio_buf[312 * 2];
    int64_t select_pressed_time = 0;
    bool select_chord_used = false;
    uint32_t prev_buttons = 0;

    while (true) {
        const int64_t frame_start = rg_system_timer();
        const uint32_t buttons = rg_input_read_gamepad();
        const uint32_t just_pressed = buttons & ~prev_buttons;

        // Auto-run LOAD "" for tape games after Sinclair ROM boot (~1.2s)
        if (tape_autoload_queued) {
            tape_boot_counter++;
            if (tape_boot_counter >= 60) {
                RG_LOGI("Auto-injecting LOAD \"\" for tape...");
                spectemu_inject_string("J\"\"\n");
                tape_autoload_queued = false;
            }
        }

        // In-game Menus
        if (buttons & (RG_KEY_MENU | RG_KEY_OPTION)) {
            rg_audio_set_mute(true);
            if (buttons & RG_KEY_MENU) rg_gui_game_menu();
            else rg_gui_options_menu();
            rg_audio_set_mute(false);
            while (rg_input_read_gamepad() & (RG_KEY_MENU | RG_KEY_OPTION)) {
                rg_task_delay(20);
            }
            prev_buttons = 0;
            continue;
        }

        // Trainer & Virtual Keyboard Shortcuts (Hold SELECT)
        if (buttons & RG_KEY_SELECT) {
            if (select_pressed_time == 0) {
                select_pressed_time = frame_start;
                select_chord_used = false;
            }

            // Quick Trainer keys (edge-triggered: single clean keystroke)
            if (just_pressed & RG_KEY_UP) {
                spectemu_inject_key('1');
                select_chord_used = true;
            } else if (just_pressed & RG_KEY_RIGHT) {
                spectemu_inject_key('2');
                select_chord_used = true;
            } else if (just_pressed & RG_KEY_DOWN) {
                spectemu_inject_key('3');
                select_chord_used = true;
            } else if (just_pressed & RG_KEY_LEFT) {
                spectemu_inject_key('4');
                select_chord_used = true;
            } else if (just_pressed & RG_KEY_START) {
                spectemu_inject_key('5');
                select_chord_used = true;
            } else if (just_pressed & RG_KEY_A) {
                spectemu_inject_key('Y');
                select_chord_used = true;
            } else if (just_pressed & RG_KEY_B) {
                spectemu_inject_key('N');
                select_chord_used = true;
            } else if (just_pressed & RG_KEY_X) {
                spectemu_inject_key(' ');
                select_chord_used = true;
            } else if (just_pressed & RG_KEY_Y) {
                virtual_keyboard_session();
                select_chord_used = true;
                select_pressed_time = 0;
                prev_buttons = 0;
                continue;
            }

            // Long-press SELECT opens virtual keyboard (held for 600ms without chord)
            if (!select_chord_used && (frame_start - select_pressed_time >= 600000)) {
                virtual_keyboard_session();
                select_chord_used = true;
                select_pressed_time = 0;
                prev_buttons = 0;
                continue;
            }
        } else {
            // Tapped SELECT alone sends Space (universal start / action in ZX Spectrum)
            if (select_pressed_time != 0 && !select_chord_used &&
                (frame_start - select_pressed_time < 600000)) {
                spectemu_inject_key(' ');
            }
            select_pressed_time = 0;
            select_chord_used = false;
        }

        // Dedicated shoulder button shortcuts (edge-triggered: single clean keystroke)
        if (just_pressed & RG_KEY_L) {
            spectemu_inject_key('1');
        }
        if (just_pressed & RG_KEY_R) {
            spectemu_inject_key('2');
        }

        prev_buttons = buttons;

        // Pass input to emulator (suppress directional input if SELECT chord was active)
        uint32_t active_buttons = buttons;
        if (buttons & RG_KEY_SELECT) {
            active_buttons &= ~(RG_KEY_UP | RG_KEY_DOWN | RG_KEY_LEFT | RG_KEY_RIGHT | RG_KEY_A | RG_KEY_B | RG_KEY_X | RG_KEY_Y | RG_KEY_START);
        }
        spectemu_set_input(active_buttons, joystick_mode);

        // Run one 50Hz emulation frame
        spectemu_run_frame();

        // Render screen
        if (!rg_display_is_busy() && output) {
            spectemu_render_screen((uint16_t *)output->data, output->width, output->height);
            rg_display_submit(output, 0);
            output_valid = true;
        }

        // Submit audio
        int audio_samples = spectemu_get_audio(audio_buf, 312);
        if (audio_samples > 0) {
            rg_audio_submit((const rg_audio_frame_t *)audio_buf, audio_samples);
        }

        int busy_time = (int)(rg_system_timer() - frame_start);
        rg_system_tick(busy_time);
    }
}
