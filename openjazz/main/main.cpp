#include <rg_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>

#include "retrogo_video_adapter.h"

extern int openjazz_main(int argc, char **argv);

/* The engine runs on its own task: the Font loaders alone need 5-7 KB of stack,
 * more than the 8 KB main task can take. Created with rg_task_create so Retro-Go's
 * monitor reports the stack high-water mark in the log. */
#define OPENJAZZ_TASK_STACK_SIZE (32 * 1024)

/* --- Retro-Go menu handlers (same pattern as the OpenTyrian port) ---------- */

static bool screenshot_handler(const char *filename, int width, int height)
{
    rg_surface_t *frame = oj_rg_get_frame();
    return frame && rg_surface_save_image_file(frame, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    (void)filename;
    rg_gui_alert("Not supported", "Please use the in-game save menu");
    return false;
}

static bool load_state_handler(const char *filename)
{
    (void)filename;
    rg_gui_alert("Not supported", "Please use the in-game load menu");
    return false;
}

static bool reset_handler(bool hard)
{
    (void)hard;
    rg_system_restart();
    return true;
}

static void event_handler(int event, void *arg)
{
    (void)arg;
    if (event == RG_EVENT_REDRAW) {
        rg_surface_t *frame = oj_rg_get_frame();
        if (frame)
            rg_display_submit(frame, 0);
    }
}

static void openjazz_task(void *arg)
{
    (void)arg;

    /* OpenJazz's OJ_RETROGO startup path searches /sd/roms/openjazz. */
    char arg0[] = "openjazz";
    char *argv[] = { arg0, NULL };
    (void)openjazz_main(1, argv);

    rg_system_exit();
}

extern "C" void app_main(void)
{
    rg_config_t config = {};
    config.sampleRate = 22050;      /* matches SOUND_FREQ in openjazz/CMakeLists.txt */
    config.frameRate = 50;
    config.storageRequired = true;
    config.romRequired = false;     /* game data is a folder on SD */
    config.isLauncher = false;
    config.handlers.loadState = &load_state_handler;
    config.handlers.saveState = &save_state_handler;
    config.handlers.reset = &reset_handler;
    config.handlers.screenshot = &screenshot_handler;
    config.handlers.event = &event_handler;
    /* malloc() calls larger than this go to PSRAM. A Jazz level keeps ~270
     * surfaces (~1.3 MB) alive, far more than the free internal RAM. */
    config.mallocAlwaysInternal = 256;

    rg_app_t *app = rg_system_init(&config);
    if (!app) {
        printf("OpenJazz: Retro-Go system init failed\n");
        return;
    }

    /* Jazz is 320x200 drawn for 4:3 pixels: stretch to the 320x240 screen the
     * first time only, so the user's choice in Options -> Scaling sticks. */
    if (!rg_settings_exists(NS_APP, "DispScaling"))
        rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);

    rg_task_create("openjazz", openjazz_task, NULL, OPENJAZZ_TASK_STACK_SIZE,
                   1, RG_TASK_PRIORITY_1, -1);

    /* Keep app_main alive so tasks[0] ("main") stays valid for Retro-Go's monitor. */
    while (1)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
