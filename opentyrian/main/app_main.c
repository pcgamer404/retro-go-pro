#include <string.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <rg_system.h>
#include <rg_display.h>
#include <rg_gui.h>
#include <rg_storage.h>

#define TYRIAN_AUDIO_OUTPUT_RATE 44100
#define TYRIAN_TICK_RATE 60

extern int main(int argc, char *argv[]); // OpenTyrian's own main() (opentyr.c)
extern const char *custom_data_dir;      // set below, consumed by file.c's data_dir()

static rg_app_t *app;

static bool save_state_handler(const char *filename)
{
    (void)filename;
    rg_gui_alert("Not implemented", "Please use Tyrian's in-game Save menu.");
    return false;
}

static bool load_state_handler(const char *filename)
{
    (void)filename;
    rg_gui_alert("Not implemented", "Please use Tyrian's in-game Load menu.");
    return false;
}

static bool reset_handler(bool hard)
{
    (void)hard;
    return false;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_SHUTDOWN)
        rg_audio_set_mute(true);
}

// Tyrian expects a flat directory containing tyrian1.lvl etc. If the user
// picked a specific file (e.g. a shortcut), use its containing folder;
// otherwise fall back to roms/tyrian/data.
static bool resolve_data_directory(char *dest, size_t dest_size)
{
    const char *selected = app->romPath;

    if (selected && selected[0])
    {
        struct stat info;
        if (stat(selected, &info) == 0 && S_ISDIR(info.st_mode))
        {
            snprintf(dest, dest_size, "%s", selected);
            return true;
        }

        snprintf(dest, dest_size, "%s", selected);
        char *sep = strrchr(dest, '/');
        if (sep)
        {
            if (sep == dest)
                sep[1] = '\0';
            else
                *sep = '\0';
            return true;
        }
    }

    snprintf(dest, dest_size, "%s/tyrian/data", RG_BASE_PATH_ROMS);
    return true;
}

static void tyrian_task(void *arg)
{
    char *argv[] = {"opentyrian", NULL};
    main(1, argv);
    RG_LOGI("Tyrian exited, returning to launcher.\n");
    rg_system_exit();
    vTaskDelete(NULL);
}

void app_main(void)
{
    const rg_config_t config = {
        .sampleRate = TYRIAN_AUDIO_OUTPUT_RATE,
        .frameRate = TYRIAN_TICK_RATE,
        .storageRequired = true,
        .romRequired = false, // Tyrian ships its own data dir, not a single ROM file
        .handlers = {
            .loadState = &load_state_handler,
            .saveState = &save_state_handler,
            .reset = &reset_handler,
            .event = &event_handler,
        },
    };

    app = rg_system_init(&config);

    if (!rg_settings_exists(NS_APP, "DispScaling"))
        rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);

    static char data_dir_buf[256];
    resolve_data_directory(data_dir_buf, sizeof(data_dir_buf));
    custom_data_dir = data_dir_buf;
    RG_LOGI("Tyrian data directory: %s\n", custom_data_dir);
    rg_storage_mkdir(custom_data_dir);

    // OpenTyrian's main() runs its own blocking game loop, so give it a
    // dedicated task/stack (matches the original port's 34000-byte stack).
    xTaskCreatePinnedToCore(tyrian_task, "tyrian_task", 34000, NULL, 5, NULL, 1);

    while (1)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
