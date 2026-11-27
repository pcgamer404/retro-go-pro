#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <rg_system.h>
#include <rg_display.h>
#include <rg_surface.h>
#include <rg_storage.h>
#include <rg_gui.h>

#include <SDL.h>

static const char *TAG = "devilutionx-go";

static rg_app_t *app = NULL;
static rg_surface_t *screen_surf = NULL;

extern "C" void SDL_RG_SetSurface(rg_surface_t *surf);
extern "C" void SDL_RG_ResetTiming();

#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "esp_random.h"

extern "C" {
int chmod(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    return 0;
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    (void)path;
    (void)buf;
    (void)bufsiz;
    errno = EINVAL;
    return -1;
}
}

namespace devilution {
int DiabloMain(int argc, char **argv);
}

// Retro-Go button mapping for DevilutionX (matching HAS_KBCTRL in retrogo.cmake)
typedef struct {
    int rg_key;
    SDL_Scancode scancode;
    SDL_Keycode keycode;
} rg_bind_t;

static const rg_bind_t binds[] = {
    {RG_KEY_UP,     SDL_SCANCODE_UP,     SDLK_UP},
    {RG_KEY_DOWN,   SDL_SCANCODE_DOWN,   SDLK_DOWN},
    {RG_KEY_LEFT,   SDL_SCANCODE_LEFT,   SDLK_LEFT},
    {RG_KEY_RIGHT,  SDL_SCANCODE_RIGHT,  SDLK_RIGHT},
    {RG_KEY_A,      SDL_SCANCODE_SPACE,  SDLK_SPACE},   // Action / Select / Attack
    {RG_KEY_B,      SDL_SCANCODE_ESCAPE, SDLK_ESCAPE},  // Cancel / Back / Close
    {RG_KEY_X,      SDL_SCANCODE_TAB,    SDLK_TAB},     // Automap
    {RG_KEY_Y,      SDL_SCANCODE_E,      SDLK_e},       // Inventory
    {RG_KEY_START,  SDL_SCANCODE_RETURN, SDLK_RETURN},  // Pause / Menu
    {RG_KEY_SELECT, SDL_SCANCODE_RCTRL,  SDLK_RCTRL},   // Quests
    {RG_KEY_L,      SDL_SCANCODE_C,      SDLK_c},       // Character
    {RG_KEY_R,      SDL_SCANCODE_T,      SDLK_t},       // Spell / Potion
    {0, SDL_SCANCODE_UNKNOWN, (SDL_Keycode)0}
};

static uint32_t last_input = 0;
static uint32_t suppressed_input = 0;
static int64_t menu_press_time = 0;
static bool menu_pressed = false;
static bool pending_menu_keyup = false;
static int64_t menu_keydown_time = 0;

#define SYSTEM_MENU_HOLD_US 500000
#define NATIVE_TAP_MIN_US 1000

static void set_key_event(SDL_Event *event, bool pressed,
                          SDL_Scancode scancode, SDL_Keycode keycode)
{
    event->type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    event->key.state = pressed ? SDL_PRESSED : SDL_RELEASED;
    event->key.keysym.scancode = scancode;
    event->key.keysym.sym = keycode;
    event->key.keysym.mod = KMOD_NONE;
    SDL_SetKeyState(keycode, pressed ? 1 : 0);
}

extern "C" int SDL_RG_PollEvent(SDL_Event *event)
{
    uint32_t raw_input = rg_input_read_gamepad();
    suppressed_input &= raw_input;
    uint32_t current_input = raw_input & ~suppressed_input;
    int64_t now = rg_system_timer();

    // A short MENU press is Escape. Defer it until release so a
    // long hold opens Retro-Go's game menu without triggering in-game Escape.
    if (pending_menu_keyup) {
        if (now - menu_keydown_time < NATIVE_TAP_MIN_US)
            return 0;

        pending_menu_keyup = false;
        set_key_event(event, false, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
        return 1;
    }

    if (current_input & RG_KEY_MENU) {
        if (!menu_pressed) {
            menu_pressed = true;
            menu_press_time = now;
        } else if (now - menu_press_time >= SYSTEM_MENU_HOLD_US) {
            suppressed_input |= RG_KEY_MENU;
            menu_pressed = false;
            rg_gui_game_menu();
            return 0;
        }
    } else if (menu_pressed) {
        menu_pressed = false;
        pending_menu_keyup = true;
        menu_keydown_time = now;
        set_key_event(event, true, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
        return 1;
    }

    uint32_t changed = current_input ^ last_input;
    if (changed) {
        for (int i = 0; binds[i].rg_key; i++) {
            if (changed & binds[i].rg_key) {
                bool pressed = (current_input & binds[i].rg_key) != 0;
                set_key_event(event, pressed, binds[i].scancode, binds[i].keycode);
                last_input ^= binds[i].rg_key;
                return 1;
            }
        }
    }

    return 0;
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return screen_surf ? rg_surface_save_image_file(screen_surf, filename, width, height) : false;
}

static bool reset_handler(bool hard)
{
    rg_system_restart();
    return true;
}

static void diablo_task(void *arg)
{
    ESP_LOGI(TAG, "Starting DevilutionX engine task...");

    // Check game data directory (fallback to /sd/roms/diablo or parent of launched ROM)
    const char *data_dir = "/sd/roms/diablo";
    char dir_buf[256];
    if (app && app->romPath && strlen(app->romPath) > 0) {
        strncpy(dir_buf, app->romPath, sizeof(dir_buf) - 1);
        dir_buf[sizeof(dir_buf) - 1] = '\0';
        char *last_slash = strrchr(dir_buf, '/');
        if (last_slash) {
            *last_slash = '\0';
            data_dir = dir_buf;
        }
    }

    char diabdat_path[512];
    snprintf(diabdat_path, sizeof(diabdat_path), "%s/DIABDAT.MPQ", data_dir);

    // Also check lowercase
    bool found = rg_storage_exists(diabdat_path);
    if (!found) {
        snprintf(diabdat_path, sizeof(diabdat_path), "%s/diabdat.mpq", data_dir);
        found = rg_storage_exists(diabdat_path);
    }
    // Also check spawn
    if (!found) {
        snprintf(diabdat_path, sizeof(diabdat_path), "%s/spawn.mpq", data_dir);
        found = rg_storage_exists(diabdat_path);
    }

    if (!found) {
        ESP_LOGE(TAG, "DIABDAT.MPQ not found in %s", data_dir);
        rg_gui_alert("Diablo Data Missing",
                     "Please place DIABDAT.MPQ (or spawn.mpq)\nin /sd/roms/diablo/");
        rg_system_exit();
        return;
    }

    ESP_LOGI(TAG, "Game data found: %s", diabdat_path);

    // Also check for devilutionx.mpq or loose assets folder
    char devilutionx_path[512];
    snprintf(devilutionx_path, sizeof(devilutionx_path), "%s/devilutionx.mpq", data_dir);
    bool found_devx = rg_storage_exists(devilutionx_path);
    if (!found_devx) {
        snprintf(devilutionx_path, sizeof(devilutionx_path), "%s/DEVILUTIONX.MPQ", data_dir);
        found_devx = rg_storage_exists(devilutionx_path);
    }
    char assets_dir_path[512];
    snprintf(assets_dir_path, sizeof(assets_dir_path), "%s/assets", data_dir);
    bool found_assets = rg_storage_exists(assets_dir_path);

    if (!found_devx && !found_assets) {
        ESP_LOGE(TAG, "devilutionx.mpq not found in %s", data_dir);
        rg_gui_alert("DevilutionX Assets Missing",
                     "Please place devilutionx.mpq in\n/sd/roms/diablo/\nalongside DIABDAT.MPQ");
        rg_system_exit();
        return;
    }

    char arg0[] = "devilutionx";
    char arg1[] = "--data-dir";
    char arg2[256];
    snprintf(arg2, sizeof(arg2), "%s", data_dir);
    char arg3[] = "--save-dir";
    char arg4[] = "/sd/retro-go/saves/diablo";
    char arg5[] = "--config-dir";
    char arg6[] = "/sd/retro-go/config/diablo";
    char arg7[] = "--diablo";

    char *argv[] = { arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, NULL };
    int argc = 8;

    ESP_LOGI(TAG, "Launching Diablo main loop...");
    devilution::DiabloMain(argc, argv);

    ESP_LOGI(TAG, "Diablo main loop returned; exiting.");
    rg_system_exit();
}

extern "C" void app_main(void)
{
    static const rg_config_t config = {
        .sampleRate = 22050,
        .frameRate = 30,
        .storageRequired = true,
        .romRequired = false,
        .handlers = {
            .reset = &reset_handler,
            .screenshot = &screenshot_handler,
        },
        .mallocAlwaysInternal = 512,
    };

    ESP_LOGI(TAG, "DevilutionX-Go initializing...");

    app = rg_system_init(&config);
    if (!app) {
        ESP_LOGE(TAG, "rg_system_init returned NULL");
        return;
    }

    rg_storage_mkdir("/sd/retro-go");
    rg_storage_mkdir("/sd/retro-go/saves");
    rg_storage_mkdir("/sd/retro-go/saves/diablo");
    rg_storage_mkdir("/sd/retro-go/config");
    rg_storage_mkdir("/sd/retro-go/config/diablo");
    rg_storage_mkdir("/sd/roms");
    rg_storage_mkdir("/sd/roms/diablo");

    screen_surf = rg_surface_create(640, 480, RG_PIXEL_PAL565_LE, MEM_SLOW);
    if (!screen_surf) {
        ESP_LOGE(TAG, "Failed to allocate 640x480 paletted framebuffer");
        rg_system_exit();
        return;
    }
    SDL_RG_SetSurface(screen_surf);

    if (!rg_settings_exists(NS_APP, "DispScaling")) {
        rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);
    }

    // Stack size: 64KB in PSRAM
    rg_task_create("diablo_task", diablo_task, NULL, 64 * 1024, 1, RG_TASK_PRIORITY_1, 0);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
