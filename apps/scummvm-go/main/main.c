#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <rg_system.h>
#include <rg_display.h>
#include <rg_audio.h>
#include <rg_gui.h>
#include <rg_settings.h>
#include <rg_surface.h>
#include <string.h>
#include <stdio.h>

// components/scummvm/rg/rg_glue.h and scummvm_run.cpp
extern void *rgx_get_surface(void);
extern int rgx_cursor_speed(void);
extern bool rgx_sound_enabled(void);
extern void rgx_request_quicksave(int what);
extern void rgx_audio_stop(void);
extern bool rgx_game_location(char *dir, int dir_size, char *gameid, int gameid_size);
extern int scummvm_run(const char *gameDir, const char *gameIdHint);

static rg_app_t *app = NULL;

static bool screenshot_handler(const char *filename, int width, int height)
{
    rg_surface_t *surf = (rg_surface_t *)rgx_get_surface();
    return surf ? rg_surface_save_image_file(surf, filename, width, height) : false;
}

// Retro-Go save/load slots are mapped to SCUMM's quicksave (Alt+1 / Ctrl+1) on the next poll
static bool save_state_handler(const char *filename)
{
    rgx_request_quicksave(1);
    return true;
}

static bool load_state_handler(const char *filename)
{
    rgx_request_quicksave(2);
    return true;
}

static bool reset_handler(bool hard)
{
    rg_system_restart();
    return true;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_surface_t *surf = (rg_surface_t *)rgx_get_surface();
        if (surf)
            rg_display_submit(surf, 0);
    }
    else if (event == RG_EVENT_SHUTDOWN)
    {
        rgx_audio_stop();
    }
}

static rg_gui_event_t cursor_speed_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char *names[] = { "Slow", "Normal", "Fast" };
    int v = rgx_cursor_speed();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        v = (v + (event == RG_DIALOG_NEXT ? 1 : 2)) % 3;
        rg_settings_set_number(NS_APP, "cursor_speed", v);
        return RG_DIALOG_REDRAW;
    }
    strcpy(option->value, names[(v < 0 || v > 2) ? 1 : v]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t sound_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    bool on = rgx_sound_enabled();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        on = !on;
        rg_settings_set_number(NS_APP, "sound", on);
        return RG_DIALOG_REDRAW;
    }
    strcpy(option->value, on ? "On (restart game)" : "Off (restart game)");
    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Cursor speed"), "-", RG_DIALOG_FLAG_NORMAL, &cursor_speed_cb};
    *dest++ = (rg_gui_option_t){0, _("Sound"), "-", RG_DIALOG_FLAG_NORMAL, &sound_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

static void scummvm_task(void *arg)
{
    char dir[256], gameid[64];
    if (!rgx_game_location(dir, sizeof(dir), gameid, sizeof(gameid)))
    {
        rg_gui_alert("ScummVM", "Launch a game folder, .scummvm stub, .svm ZIP, or .zip game from the launcher");
        rg_system_exit();
        return;
    }
    RG_LOGI("ScummVM game dir: %s (id hint '%s')\n", dir, gameid);
    int rc = scummvm_run(dir, gameid);
    if (rc == -10)
        rg_gui_alert("ScummVM", "Could not read the ZIP file. It may be damaged or not a ZIP.");
    else if (rc == -11)
        rg_gui_alert("ScummVM", "ZIP unpack failed (SD card full, or the ZIP uses ZIP64/Deflate64 or unsafe paths). Re-zip with normal Deflate or Store, or free SD space, then launch again to resume.");
    else if (rc != 0)
        rg_gui_alert("ScummVM", "No supported game found. Check the files are directly in the folder/ZIP.");
    rg_system_exit();
}

void app_main(void)
{
    static const rg_config_t config = {
        .sampleRate = 22050,
        .frameRate = 30,
        .storageRequired = true,
        .romRequired = false,
        .handlers = {
            .loadState = &load_state_handler,
            .saveState = &save_state_handler,
            .reset = &reset_handler,
            .screenshot = &screenshot_handler,
            .event = &event_handler,
            .options = &options_handler,
        },
        .mallocAlwaysInternal = 1,
    };

    app = rg_system_init(&config);
    rg_system_set_tick_rate(30);
    app->frameskip = -1; // frames are only pushed when the picture changes

    // 320x200 games are 4:3 -> fill a 320x240 panel by default, user can change it later
    if (!rg_settings_exists(NS_APP, "DispScaling"))
        rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);

    rg_task_create("scummvm_main", scummvm_task, NULL, 32 * 1024, 1, RG_TASK_PRIORITY_1, 0);

    // Keep app_main alive so tasks[0] ("main") stays valid
    while (1)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
