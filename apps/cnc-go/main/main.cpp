/**
 * @file main.cpp
 * @brief Entry point for Command & Conquer on Retro-Go (ESP32-S3).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"

#include <rg_system.h>
#include <rg_storage.h>
#include <rg_display.h>
#include <rg_audio.h>
#include <rg_input.h>
#include <rg_gui.h>

#include "display.h"
#include "board_config.h"
#include "cnc_platform.h"

#define GAME_TASK_STACK_SIZE    (48 * 1024)
#define GAME_TASK_PRIORITY      5
#define GAME_TASK_CORE          1
#define STATS_TASK_PERIOD_MS    10000

static const char* TAG = "cnc_main";

int cnc_game_main(int argc, char** argv);
extern "C" int Debug_Game_Frame(void);
extern "C" void soundShutdown(void);
extern "C" void videoShutdown(void);

static void eventHandler(int event, void* data)
{
    if (event == RG_EVENT_SHUTDOWN) {
        soundShutdown();
        videoShutdown();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

extern "C" int nanosleep(const struct timespec* req, struct timespec* rem)
{
    uint64_t us = (uint64_t)req->tv_sec * 1000000 + req->tv_nsec / 1000;
    if (us) {
        usleep(us);
    }
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

static void allocFailed(size_t size, uint32_t caps, const char* function)
{
    printf("Allocation of %u bytes failed (caps 0x%x, %s); PSRAM free %u, largest %u\n",
           (unsigned)size,
           (unsigned)caps,
           function,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static void halt(const char* msg)
{
    videoConsolePrint("%s", msg);
    videoConsolePrint("Press MENU to return to Launcher.");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        uint32_t keys = rg_input_read_gamepad();
        if (keys & (RG_KEY_MENU | RG_KEY_START | RG_KEY_B)) {
            rg_system_exit();
        }
    }
}

static volatile bool s_game_done = false;

static void gameTask(void* arg)
{
    const char* data_dir = cnc_get_data_dir();
    char arg0[300];
    snprintf(arg0, sizeof(arg0), "%s/%s", data_dir, CNC_PROGRAM_NAME);
    char* argv[] = {arg0, nullptr};
    
    printf("[cnc] Starting C&C game engine with data dir: %s\n", data_dir);

    // List all files in data_dir so we can see what's on the SD card in serial log
    printf("[cnc] Scanning directory '%s':\n", data_dir);
    DIR* dir = opendir(data_dir);
    if (dir) {
        struct dirent* de;
        int count = 0;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] != '.') {
                printf("  [cnc-file] %s\n", de->d_name);
                count++;
            }
        }
        closedir(dir);
        printf("[cnc] Found %d file(s) in %s\n", count, data_dir);
    } else {
        printf("[cnc warn] Unable to opendir('%s')\n", data_dir);
    }

    // Change current working directory to game data directory
    chdir(data_dir);

    int ret = cnc_game_main(1, argv);
    printf("[cnc] Game exited (%d), returning to Retro-Go launcher\n", ret);
    vTaskDelay(pdMS_TO_TICKS(500));
    s_game_done = true;
    vTaskDelete(NULL);
}

static void statsTask(void* arg)
{
    uint32_t lastFrames = 0;
    int lastGameFrame = 0;
    int64_t lastTime = esp_timer_get_time();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(STATS_TASK_PERIOD_MS));
        int64_t now = esp_timer_get_time();
        uint32_t frames = videoGetFrameCount();
        int gameFrame = Debug_Game_Frame();
        printf("C&C Game fps: %.1f, LCD fps: %.1f, free: internal %u kB, PSRAM %u kB (largest %u kB)\n",
               (gameFrame - lastGameFrame) * 1e6f / (now - lastTime),
               (frames - lastFrames) * 1e6f / (now - lastTime),
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
               (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
        lastFrames = frames;
        lastGameFrame = gameFrame;
        lastTime = now;
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Command & Conquer for Retro-Go starting up...");

    rg_config_t config = {};
    config.sampleRate = AUDIO_SAMPLE_RATE;
    config.frameRate = 30;
    config.storageRequired = true;
    config.romRequired = false;
    config.handlers.event = eventHandler;

    rg_app_t *app = rg_system_init(&config);
    if (app && app->romPath && app->romPath[0]) {
        char dir_buf[256];
        strncpy(dir_buf, app->romPath, sizeof(dir_buf) - 1);
        dir_buf[sizeof(dir_buf) - 1] = '\0';
        char* last_slash = strrchr(dir_buf, '/');
        if (last_slash) {
            *last_slash = '\0';
            cnc_set_data_dir(dir_buf);
        }
    }

    debugInit();
    heap_caps_register_failed_alloc_callback(allocFailed);

    displayInit();
    videoInit();

    videoConsolePrint("Command & Conquer for Retro-Go");
    videoConsolePrint("Vanilla-Conquer port (ESP32-S3)");
    videoConsolePrint("Build: %s %s", __DATE__, __TIME__);

    if (!esp_psram_is_initialized()) {
        halt("PSRAM not found: 8MB PSRAM is required!");
    }

    videoConsolePrint("PSRAM: %u kB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    videoConsolePrint("Internal RAM: %u kB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    // Check game data directory
    const char* data_dir = cnc_get_data_dir();
    char check_path[300];
    snprintf(check_path, sizeof(check_path), "%s/%s", data_dir, CNC_CHECK_FILE);

    struct stat st;
    if (stat(check_path, &st) != 0) {
        videoConsolePrint("%s not found!", check_path);
        videoConsolePrint("Please copy C&C DOS files (CONQUER.MIX, etc.)");
        videoConsolePrint("to /sd/roms/cnc/ on your SD card.");
        halt("Missing game data files.");
    }

    inputInit();

    videoConsolePrint("Starting the game...");
    xTaskCreatePinnedToCoreWithCaps(statsTask, "stats", 3072, nullptr, 1, nullptr, 0, MALLOC_CAP_SPIRAM);

    BaseType_t res = xTaskCreatePinnedToCoreWithCaps(gameTask,
                                                    "cnc",
                                                    GAME_TASK_STACK_SIZE,
                                                    nullptr,
                                                    GAME_TASK_PRIORITY,
                                                    nullptr,
                                                    GAME_TASK_CORE,
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (res != pdPASS) {
        ESP_LOGW(TAG, "Using PSRAM for game task stack");
        xTaskCreatePinnedToCoreWithCaps(gameTask,
                                       "cnc",
                                       GAME_TASK_STACK_SIZE,
                                       nullptr,
                                       GAME_TASK_PRIORITY,
                                       nullptr,
                                       GAME_TASK_CORE,
                                       MALLOC_CAP_SPIRAM);
    }

    while (!s_game_done) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    rg_system_exit();
}
