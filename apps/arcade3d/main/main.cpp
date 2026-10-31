// "Arcade 3D": davidmonterocrespo24/esp32s3-arcade-3d, an OutRun-style racing
// game for Arduino + TFT_eSPI, run here through components/arcade3d/shim.
#define A3D_AUDIO_RATE 32000
extern "C" {
#include <rg_system.h>
}
#include <Arduino.h>
#include <esp_heap_caps.h>

static rg_app_t *app;
static rg_surface_t *frames[2];
static int frame_index;
static uint32_t buttons;
static int64_t frame_start;

void *a3d_alloc(size_t bytes)
{
    return rg_alloc(bytes, MEM_SLOW);
}

// Internal RAM while it leaves room for the rest of the app (the menu, a
// screenshot, the SD driver), else PSRAM.
void *a3d_alloc_fast(size_t bytes)
{
    static int fast, slow;
    void *ptr = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > bytes + 48 * 1024) // leave room for display DMA, SD, menus
        ptr = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ptr)
        fast++;
    else if ((ptr = rg_alloc(bytes, MEM_SLOW)))
        slow++;
    RG_LOGI("arcade3d: frame strips: %d in internal RAM, %d in PSRAM", fast, slow);
    return ptr;
}

int64_t a3d_micros(void)
{
    return rg_system_timer();
}

uint32_t a3d_buttons(void)
{
    return buttons;
}

// One finished frame: copied to the surface the display is not showing, so
// the game can draw the next one straight away.
void a3d_present(uint16_t *const *rows, int width, int height)
{
    rg_surface_t *frame = frames[frame_index];
    // The display task may still be pushing the previous use of this buffer to the panel: wait for it, otherwise
    // a quick frame overwrites a frame that is still being sent (flicker / tearing)
    rg_display_sync();
    for (int y = 0; y < height; y++)
        memcpy((uint8_t *)frame->data + y * frame->stride, rows[y], (size_t)width * 2);
    rg_system_tick(rg_system_timer() - frame_start);
    rg_display_submit(frame, 0);
    frame_index ^= 1;

    uint32_t joystick = rg_input_read_gamepad();
    if (joystick & RG_KEY_MENU)
        rg_gui_game_menu();
    else if (joystick & RG_KEY_OPTION)
        rg_gui_options_menu();
    // Gas = A or UP, brake = B or DOWN
    buttons = ((joystick & RG_KEY_LEFT) ? A3D_BTN_LEFT : 0) | ((joystick & RG_KEY_RIGHT) ? A3D_BTN_RIGHT : 0)
            | ((joystick & (RG_KEY_B | RG_KEY_DOWN)) ? A3D_BTN_BRAKE : 0)
            | ((joystick & (RG_KEY_A | RG_KEY_UP)) ? A3D_BTN_GAS : 0);
    frame_start = rg_system_timer();
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(frames[frame_index ^ 1], filename, width, height);
}

static bool reset_handler(bool hard)
{
    rg_system_restart();
    return true;
}

static void arcade3d_run(void)
{
    static const rg_handlers_t handlers = {
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
    };
    app = rg_system_reinit(A3D_AUDIO_RATE, &handlers, NULL);
    app->tickRate = 60;

    frames[0] = rg_surface_create(320, 240, RG_PIXEL_565_LE, MEM_SLOW);
    frames[1] = rg_surface_create(320, 240, RG_PIXEL_565_LE, MEM_SLOW);
    if (!frames[0] || !frames[1])
        RG_PANIC("No memory for the frame buffers");

    frame_start = rg_system_timer();
    setup();
    while (true)
        loop();
}

extern "C" void app_main(void)
{
    // C++ cannot take the address of a compound literal (it worked in the C apps), so use a named struct
    rg_config_t config = {};
    config.sampleRate = A3D_AUDIO_RATE;
    config.frameRate = 60;
    config.storageRequired = true;
    config.romRequired = false; // a game port: nothing to load from the SD card
    rg_system_init(&config);
    arcade3d_run();
    RG_PANIC("Never reached");
}
