// SOTN for Retro-Go - entry point.
//
// Same shape as the OpenTyrian port: app_main sets retro-go up and starts the
// game on its own task. Everything board related that the standalone port's
// app_main did by hand (panel + SD bring-up, USB console, pin probes) is
// retro-go's job now.

#include <rg_system.h>
#include <rg_display.h>
#include <rg_audio.h>
#include <rg_gui.h>
#include <rg_settings.h>
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>

// Game task stack (internal DRAM). The game ran on an 8 KB stack in the
// standalone port; we allocate 16 KB in internal DRAM for fast zero-wait access
// and to ensure flash writes during menu exit do not violate cache freeze rules.
#define SOTN_TASK_STACK (16 * 1024)

void sotn_run(void);                   // rg_boot.c
bool sotn_data_present(void);          // rg_boot.c
const char *sotn_data_root(void);      // rg_boot.c
int lcd_init(void);                    // rg_lcd.c
void lcd_resubmit(void);               // rg_lcd.c
rg_surface_t *lcd_get_surface(void);   // rg_lcd.c
extern volatile int sotn_paused;       // rg_audio_out.c

static bool screenshot_handler(const char *filename, int width, int height)
{
    rg_surface_t *surf = lcd_get_surface();
    return surf && rg_surface_save_image_file(surf, filename, width, height);
}

// The game keeps its own memory-card saves (files under the data folder).
static bool save_state_handler(const char *filename)
{
    rg_gui_alert("Not supported", "Please use the in-game save menu");
    return false;
}

static bool load_state_handler(const char *filename)
{
    rg_gui_alert("Not supported", "Please use the in-game load menu");
    return false;
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
        // a menu drew over the screen: repaint the last game frame
        lcd_resubmit();
    }
    else if (event == RG_EVENT_SHUTDOWN)
    {
        // stop feeding the sink and silence the amp before retro-go tears down
        sotn_paused = 1;
        rg_audio_set_mute(true);
    }
}

static void sotn_task(void *arg)
{
    sotn_run();
    rg_system_exit();
}

void app_main(void)
{
    static const rg_config_t config = {
        .sampleRate = 44100, // PSX SPU native rate, pulled 1:1 in rg_audio_out.c
        .frameRate = 60,
        .storageRequired = true,
        .romRequired = false,
        .handlers = {
            .loadState = &load_state_handler,
            .saveState = &save_state_handler,
            .reset = &reset_handler,
            .screenshot = &screenshot_handler,
            .event = &event_handler,
        },
        .mallocAlwaysInternal = 1, // internal RAM is nearly full: keep everything else in PSRAM (as opentyrian)
    };

    rg_app_t *app = rg_system_init(&config);
    // The game draws and simulates in lockstep (one logic tick per VSync);
    // dropping frames is done in rg_lcd.c, never by skipping game ticks.
    app->frameskip = -1;

    // Ensure the config directory exists so saving settings/options succeeds without error
    rg_storage_mkdir(RG_BASE_PATH_CONFIG);

    if (!sotn_data_present())
    {
        char msg[200];
        snprintf(msg, sizeof(msg), "Copy the CONTENTS of esp32/fatfs_root/ from the decomp to:\n%s", sotn_data_root());
        rg_display_clear(C_SKY_BLUE);
        rg_gui_alert("SOTN game data missing", msg);
        rg_system_exit();
    }

    if (lcd_init() != 0)
        RG_PANIC("Video init failed (out of memory?)");

    // PSX output is 256x240 for a 4:3 screen: Full is the correct aspect on a
    // 320x240 panel. Only a first-launch preset; the user's choice in
    // Retro-Go's Options menu is kept afterwards.
    if (!rg_settings_exists(NS_APP, "DispScaling"))
        rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);

    rg_task_t *task = rg_task_create("sotn_main", sotn_task, NULL, SOTN_TASK_STACK, 1, RG_TASK_PRIORITY_1, 0);
    RG_ASSERT(task != NULL, "Failed to create game task");

    // Keep app_main alive so tasks[0] ("main") stays valid in FreeRTOS
    while (1)
    {
        rg_task_delay(1000);
    }
}
