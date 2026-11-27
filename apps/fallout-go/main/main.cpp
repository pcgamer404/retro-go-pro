#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include <rg_system.h>
#include <rg_display.h>
#include <rg_surface.h>
#include <rg_storage.h>
#include <rg_gui.h>
#include <rg_audio.h>

#include <SDL.h>

static const char *TAG = "fallout-go";

static rg_app_t *app = NULL;
static rg_surface_t *screen_surf = NULL;

extern "C" void SDL_RG_SetSurface(rg_surface_t *surf);
extern "C" void SDL_RG_ResetTiming();

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

namespace fo1 {
int main(int argc, char* argv[]);
void compat_set_base_path(const char* basePath);
}

namespace fo2 {
int main(int argc, char* argv[]);
void compat_set_base_path(const char* basePath);
}

#if RG_SCREEN_PIXEL_FORMAT == 0
#define FB_PIXEL_FORMAT RG_PIXEL_PAL565_BE
#else
#define FB_PIXEL_FORMAT RG_PIXEL_PAL565_LE
#endif

// Virtual mouse state (640x480 coordinate space)
static int g_cursor_x = 320;
static int g_cursor_y = 240;
static int64_t g_dpad_hold_start[4] = {0}; // UP, DOWN, LEFT, RIGHT

// Input state
static uint32_t last_input = 0;
static uint32_t suppressed_input = 0;
static int64_t menu_press_time = 0;
static bool menu_pressed = false;

#define SYSTEM_MENU_HOLD_US 500000

// Event queue for gamepad-generated events
#define EVENT_RING_SIZE 32
static SDL_Event s_event_ring[EVENT_RING_SIZE];
static int s_ring_head = 0;
static int s_ring_tail = 0;

static void queue_event(const SDL_Event *ev)
{
    int next = (s_ring_tail + 1) % EVENT_RING_SIZE;
    if (next != s_ring_head) {
        s_event_ring[s_ring_tail] = *ev;
        s_ring_tail = next;
    }
}

static bool dequeue_event(SDL_Event *ev)
{
    if (s_ring_head == s_ring_tail) return false;
    *ev = s_event_ring[s_ring_head];
    s_ring_head = (s_ring_head + 1) % EVENT_RING_SIZE;
    return true;
}

static void push_mouse_motion(int x, int y)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_MOUSEMOTION;
    ev.motion.x = x;
    ev.motion.y = y;
    queue_event(&ev);
}

static void push_mouse_button(Uint8 button, bool down, int x, int y)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    ev.button.button = button;
    ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.button.x = x;
    ev.button.y = y;
    queue_event(&ev);
}

static void push_key_event(SDL_Scancode scancode, SDL_Keycode keycode, bool down)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.scancode = scancode;
    ev.key.keysym.sym = keycode;
    ev.key.keysym.mod = KMOD_NONE;
    SDL_SetKeyState(keycode, down ? 1 : 0);
    queue_event(&ev);
}

// Retro-Go button mapping for Fallout
typedef struct {
    int rg_key;
    SDL_Scancode scancode;
    SDL_Keycode keycode;
} rg_bind_t;

static const rg_bind_t s_key_binds[] = {
    {RG_KEY_X,      SDL_SCANCODE_I,      SDLK_i},      // Inventory
    {RG_KEY_Y,      SDL_SCANCODE_SPACE,  SDLK_SPACE},  // End Turn / Confirm
    {RG_KEY_START,  SDL_SCANCODE_C,      SDLK_c},      // Character Screen
    {RG_KEY_SELECT, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE}, // Options / ESC
    {0, SDL_SCANCODE_UNKNOWN, (SDL_Keycode)0}
};

extern "C" int SDL_RG_PollEvent(SDL_Event *event)
{
    if (dequeue_event(event)) {
        return 1;
    }

    uint32_t raw_input = rg_input_read_gamepad();
    suppressed_input &= raw_input;
    uint32_t current_input = raw_input & ~suppressed_input;
    int64_t now = rg_system_timer();

    // Menu key hold check for Retro-Go GUI
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
    } else {
        menu_pressed = false;
    }

    // Virtual mouse motion via D-pad
    bool moved = false;
    int dpad_keys[4] = {RG_KEY_UP, RG_KEY_DOWN, RG_KEY_LEFT, RG_KEY_RIGHT};
    int dx_table[4] = {0, 0, -1, 1};
    int dy_table[4] = {-1, 1, 0, 0};

    for (int d = 0; d < 4; d++) {
        if (current_input & dpad_keys[d]) {
            if (g_dpad_hold_start[d] == 0) {
                g_dpad_hold_start[d] = now;
            }
            int64_t hold_us = now - g_dpad_hold_start[d];
            int speed = 3;
            if (hold_us > 350000) speed = 10;
            else if (hold_us > 150000) speed = 6;

            g_cursor_x += dx_table[d] * speed;
            g_cursor_y += dy_table[d] * speed;
            moved = true;
        } else {
            g_dpad_hold_start[d] = 0;
        }
    }

    if (g_cursor_x < 0) g_cursor_x = 0;
    if (g_cursor_x > 639) g_cursor_x = 639;
    if (g_cursor_y < 0) g_cursor_y = 0;
    if (g_cursor_y > 479) g_cursor_y = 479;

    if (moved) {
        push_mouse_motion(g_cursor_x, g_cursor_y);
    }

    // Mouse buttons (A = Left Click, B = Right Click)
    uint32_t changed = current_input ^ last_input;

    if (changed & RG_KEY_A) {
        push_mouse_button(SDL_BUTTON_LEFT, (current_input & RG_KEY_A) != 0, g_cursor_x, g_cursor_y);
    }
    if (changed & RG_KEY_B) {
        push_mouse_button(SDL_BUTTON_RIGHT, (current_input & RG_KEY_B) != 0, g_cursor_x, g_cursor_y);
    }

    // Keyboard bindings (X, Y, START, SELECT)
    for (int i = 0; s_key_binds[i].rg_key; i++) {
        if (changed & s_key_binds[i].rg_key) {
            bool down = (current_input & s_key_binds[i].rg_key) != 0;
            push_key_event(s_key_binds[i].scancode, s_key_binds[i].keycode, down);
        }
    }

    last_input = current_input;

    return dequeue_event(event) ? 1 : 0;
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

static void ensure_fallout_cfg(const char *data_dir, int game_version)
{
    char cfg_path[512];
    const char *cfg_name = (game_version == 1) ? "fallout.cfg" : "fallout2.cfg";
    snprintf(cfg_path, sizeof(cfg_path), "%s/%s", data_dir, cfg_name);

    // If config does not exist, write optimal memory defaults
    if (!rg_storage_exists(cfg_path)) {
        RG_LOGI("Creating optimized default %s at %s\n", cfg_name, cfg_path);
        FILE *f = fopen(cfg_path, "w");
        if (f) {
            fprintf(f, "[system]\n");
            fprintf(f, "art_cache_size=1\n");
            fprintf(f, "free_space=0\n");
            fprintf(f, "hashing=1\n");
            fprintf(f, "master_dat=master.dat\n");
            fprintf(f, "critter_dat=critter.dat\n");
            fprintf(f, "master_patches=data\n");
            fprintf(f, "critter_patches=data\n");
            fprintf(f, "[preferences]\n");
            fprintf(f, "play_intro=0\n");
            fprintf(f, "subtitles=1\n");
            fprintf(f, "text_delay=2\n");
            fprintf(f, "running=1\n");
            fprintf(f, "[sound]\n");
            fprintf(f, "music=1\n");
            fprintf(f, "music_volume=22000\n");
            fprintf(f, "sndfx_volume=22000\n");
            fprintf(f, "master_volume=22000\n");
            fprintf(f, "cache_size=32\n");
            fclose(f);
        }
    }
}

static void fallout_engine_task(void *arg)
{
    int game_version = (int)(intptr_t)arg;
    RG_LOGI("Starting Fallout %d engine task...\n", game_version);

    char *argv_fake[] = {(char*)"fallout", NULL};
    int argc_fake = 1;

    int ret = 0;
    if (game_version == 1) {
        ret = fo1::main(argc_fake, argv_fake);
    } else {
        ret = fo2::main(argc_fake, argv_fake);
    }

    RG_LOGI("Fallout engine finished (returned %d), exiting.\n", ret);
    rg_task_delay(500);
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
        .mallocAlwaysInternal = 0,
    };

    RG_LOGI("Fallout-Go initializing...\n");

    app = rg_system_init(&config);
    if (!app) {
        RG_LOGE("rg_system_init returned NULL\n");
        return;
    }

#ifdef ESP_PLATFORM
    // Route all standard malloc allocations directly to external PSRAM (SPIRAM)
    // to preserve internal SRAM for system/DMA/interrupts and avoid out-of-memory.
    heap_caps_malloc_extmem_enable(0);
#endif

    rg_storage_mkdir("/sd/retro-go");
    rg_storage_mkdir("/sd/retro-go/saves");
    rg_storage_mkdir("/sd/retro-go/saves/fallout");
    rg_storage_mkdir("/sd/roms");
    rg_storage_mkdir("/sd/roms/fallout");
    rg_storage_mkdir("/sd/roms/fallout/fallout1");
    rg_storage_mkdir("/sd/roms/fallout/fallout2");

    // Determine whether launching Fallout 1 or Fallout 2
    int game_version = 1;
    const char *rom_path = app ? app->romPath : NULL;
    RG_LOGI("Launched with romPath: %s\n", rom_path ? rom_path : "(null)");

    if (rom_path) {
        if (strstr(rom_path, "Fallout 2") || strstr(rom_path, "fallout2") || strstr(rom_path, "2.fo")) {
            game_version = 2;
        } else if (strstr(rom_path, "Fallout 1") || strstr(rom_path, "fallout1") || strstr(rom_path, "1.fo")) {
            game_version = 1;
        } else {
            // Auto-detect based on presence of master.dat
            if (rg_storage_exists("/sd/roms/fallout/fallout1/master.dat") ||
                rg_storage_exists("/sd/roms/fallout/fallout1/MASTER.DAT")) {
                game_version = 1;
            } else if (rg_storage_exists("/sd/roms/fallout/fallout2/master.dat") ||
                       rg_storage_exists("/sd/roms/fallout/fallout2/MASTER.DAT")) {
                game_version = 2;
            }
        }
    }

    // Set working directory
    const char *data_dir = (game_version == 1) ? "/sd/roms/fallout/fallout1" : "/sd/roms/fallout/fallout2";

    // Configure engine compatibility layer base path
    if (game_version == 1) {
        fo1::compat_set_base_path(data_dir);
    } else {
        fo2::compat_set_base_path(data_dir);
    }

    char master_dat[512];
    snprintf(master_dat, sizeof(master_dat), "%s/master.dat", data_dir);
    bool found_master = rg_storage_exists(master_dat);
    if (!found_master) {
        snprintf(master_dat, sizeof(master_dat), "%s/MASTER.DAT", data_dir);
        found_master = rg_storage_exists(master_dat);
    }

    RG_LOGI("Target data_dir: %s, master.dat found: %d\n", data_dir, (int)found_master);

    if (!found_master) {
        char alert_title[64];
        char alert_msg[256];
        snprintf(alert_title, sizeof(alert_title), "Fallout %d Data Missing", game_version);
        snprintf(alert_msg, sizeof(alert_msg),
                 "Please place master.dat and critter.dat in:\n%s/", data_dir);
        RG_LOGE("%s: %s\n", alert_title, alert_msg);
        rg_gui_alert(alert_title, alert_msg);
        rg_system_exit();
        return;
    }

    ensure_fallout_cfg(data_dir, game_version);

    // Change directory to game data directory
    chdir(data_dir);

    // Initialize display surface (640x480, downscaled 2:1 to 320x240 by Retro-Go Core 1)
    screen_surf = rg_surface_create(640, 480, FB_PIXEL_FORMAT, MEM_SLOW);
    if (!screen_surf) {
        RG_LOGE("Failed to allocate 640x480 paletted framebuffer\n");
        rg_system_exit();
        return;
    }
    SDL_RG_SetSurface(screen_surf);

    if (!rg_settings_exists(NS_APP, "DispScaling")) {
        rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);
    }

    // Start Fallout engine task with 96KB stack in PSRAM on Core 0
    TaskHandle_t task_handle = NULL;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        fallout_engine_task,
        "fallout",
        96 * 1024,
        (void*)(intptr_t)game_version,
        5,
        &task_handle,
        0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (ret != pdPASS) {
        RG_LOGE("CRITICAL: Failed to create fallout task! ret=%d\n", (int)ret);
        rg_gui_alert("Task Creation Failed", "Could not allocate task stack in PSRAM.");
        rg_system_exit();
        return;
    }

    RG_LOGI("Fallout %d task created successfully! handle=%p\n", game_version, task_handle);

    // Main thread yields and processes system events
    while (true) {
        rg_task_delay(100);
    }
}
