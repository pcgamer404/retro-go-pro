/**
 * @file cnc_input.cpp
 * @brief Retro-Go Gamepad Input backend for Command & Conquer.
 */
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "macros.h"
#include "wwkeyboard.h"
#include "sdl_keymap.h"
#include "settings.h"
#include "video.h"
#include "display.h"
#include "board_config.h"
#include "cnc_platform.h"
#include "sdl/SDL_scancode.h"

#include <rg_system.h>
#include <rg_input.h>
#include <rg_gui.h>

#define EVENT_QUEUE_LENGTH      64
#define GAMEPAD_CURSOR_SPEED    220.0f

static const char* TAG = "cnc_input";

enum
{
    EV_KEY,         // code = scancode
    EV_MOUSE,       // code = VK_LBUTTON / VK_RBUTTON / VK_MBUTTON; x, y = position (< 0: current)
};

typedef struct
{
    uint8_t type;
    uint8_t release;
    uint16_t code;
    int16_t x, y;
} inputEvent_t;

static QueueHandle_t eventQueue = NULL;
bool inputTrace = false;

static inline void queueEvent(uint8_t type, uint8_t release, uint16_t code, int16_t x, int16_t y)
{
    inputEvent_t e = {type, release, code, x, y};
    if (eventQueue) {
        xQueueSend(eventQueue, &e, 0);
    }
}

void inputInit(void)
{
    if (!eventQueue) {
        eventQueue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(inputEvent_t));
    }
    ESP_LOGI(TAG, "Input initialized (Retro-Go gamepad)");
}

void inputInjectClick(int x, int y, bool right)
{
    uint16_t code = right ? VK_RBUTTON : VK_LBUTTON;
    queueEvent(EV_MOUSE, 0, code, x, y);
    vTaskDelay(pdMS_TO_TICKS(50));
    queueEvent(EV_MOUSE, 1, code, x, y);
}

void inputInjectDrag(int x0, int y0, int x1, int y1)
{
    queueEvent(EV_MOUSE, 0, VK_LBUTTON, x0, y0);
    vTaskDelay(pdMS_TO_TICKS(50));
    queueEvent(EV_MOUSE, 0, VK_LBUTTON, x1, y1);
    vTaskDelay(pdMS_TO_TICKS(50));
    queueEvent(EV_MOUSE, 1, VK_LBUTTON, x1, y1);
}

void inputInjectKey(int scancode)
{
    queueEvent(EV_KEY, 0, scancode, -1, -1);
    vTaskDelay(pdMS_TO_TICKS(50));
    queueEvent(EV_KEY, 1, scancode, -1, -1);
}

void inputInjectScroll(int dx, int dy)
{
}

/*
** Keyboard class for Retro-Go
*/
class WWKeyboardClassESP : public WWKeyboardClass
{
public:
    virtual ~WWKeyboardClassESP()
    {
    }

    virtual void Fill_Buffer_From_System(void)
    {
        // 1. Poll Retro-Go Gamepad
        uint32_t keys = rg_input_read_gamepad();
        static uint32_t prev_keys = 0;
        uint32_t pressed = keys & ~prev_keys;
        uint32_t released = ~keys & prev_keys;
        prev_keys = keys;

        // In-game menu
        if (pressed & RG_KEY_MENU) {
            rg_gui_game_menu();
        }

        // A = Left Click (select, move, attack, sidebar)
        if (pressed & RG_KEY_A) {
            queueEvent(EV_MOUSE, 0, VK_LBUTTON, -1, -1);
        }
        if (released & RG_KEY_A) {
            queueEvent(EV_MOUSE, 1, VK_LBUTTON, -1, -1);
        }

        // B = Right Click (deselect, cancel)
        if (pressed & RG_KEY_B) {
            queueEvent(EV_MOUSE, 0, VK_RBUTTON, -1, -1);
        }
        if (released & RG_KEY_B) {
            queueEvent(EV_MOUSE, 1, VK_RBUTTON, -1, -1);
        }

        // X = Force Attack (Left Ctrl)
        if (pressed & RG_KEY_X) {
            queueEvent(EV_KEY, 0, SDL_SCANCODE_LCTRL, -1, -1);
        }
        if (released & RG_KEY_X) {
            queueEvent(EV_KEY, 1, SDL_SCANCODE_LCTRL, -1, -1);
        }

        // Y = Guard ('G')
        if (pressed & RG_KEY_Y) {
            queueEvent(EV_KEY, 0, SDL_SCANCODE_G, -1, -1);
        }
        if (released & RG_KEY_Y) {
            queueEvent(EV_KEY, 1, SDL_SCANCODE_G, -1, -1);
        }

        // START = Enter / Options
        if (pressed & RG_KEY_START) {
            queueEvent(EV_KEY, 0, SDL_SCANCODE_RETURN, -1, -1);
        }
        if (released & RG_KEY_START) {
            queueEvent(EV_KEY, 1, SDL_SCANCODE_RETURN, -1, -1);
        }

        // SELECT = Tab (Sidebar toggle)
        if (pressed & RG_KEY_SELECT) {
            queueEvent(EV_KEY, 0, SDL_SCANCODE_TAB, -1, -1);
        }
        if (released & RG_KEY_SELECT) {
            queueEvent(EV_KEY, 1, SDL_SCANCODE_TAB, -1, -1);
        }

        // 2. Cursor movement via D-Pad
        int64_t now = esp_timer_get_time();
        float dt = (now - LastTime) * 1e-6f;
        LastTime = now;
        if (dt > 0.1f) dt = 0.1f;

        float move_x = 0.0f;
        float move_y = 0.0f;
        if (keys & RG_KEY_LEFT)  move_x -= 1.0f;
        if (keys & RG_KEY_RIGHT) move_x += 1.0f;
        if (keys & RG_KEY_UP)    move_y -= 1.0f;
        if (keys & RG_KEY_DOWN)  move_y += 1.0f;

        static float dpad_hold = 0.0f;
        if (move_x != 0.0f || move_y != 0.0f) {
            dpad_hold += dt;
            float speed = 80.0f;
            if (dpad_hold > 0.4f) speed = 240.0f;
            else if (dpad_hold > 0.15f) speed = 140.0f;

            if (keys & (RG_KEY_L | RG_KEY_R)) speed *= 2.0f; // Fast scroll modifier

            videoMoveMouse(move_x * speed * dt, move_y * speed * dt);
        } else {
            dpad_hold = 0.0f;
        }

        // 3. Process queued events into C&C engine
        inputEvent_t e;
        while (!Is_Buffer_Full() && xQueueReceive(eventQueue, &e, 0) == pdTRUE) {
            if (e.type == EV_KEY) {
                Put_Key_Message(e.code, e.release);
                if (inputTrace) {
                    printf("key %d %s\n", e.code, e.release ? "up" : "down");
                }
            } else {
                int x, y;
                if (e.x >= 0) {
                    videoSetMouse(e.x, e.y);
                }
                videoGetMouse(&x, &y);
                Put_Mouse_Message(e.code, x, y, e.release);
                if (inputTrace) {
                    printf("mouse %d %s at %d,%d\n", e.code, e.release ? "up" : "down", x, y);
                }
            }
        }
    }

    virtual bool Is_Gamepad_Active()
    {
        return false;
    }

    virtual bool Is_Analog_Scroll_Active()
    {
        return false;
    }

    virtual unsigned char Get_Scroll_Direction()
    {
        return 0;
    }

    virtual KeyASCIIType To_ASCII(unsigned short key)
    {
        if (key & WWKEY_RLS_BIT) {
            return KA_NONE;
        }
        bool shift = (key & WWKEY_SHIFT_BIT) != 0;
        key &= 0xFF;
        if (key > ARRAY_SIZE(sdl_keymap) / 2 - 1) {
            return KA_NONE;
        }
        return shift ? sdl_keymap[key + ARRAY_SIZE(sdl_keymap) / 2] : sdl_keymap[key];
    }

private:
    int64_t LastTime = 0;
};

WWKeyboardClass* CreateWWKeyboardClass(void)
{
    return new WWKeyboardClassESP();
}

bool Input_Take_Drag_Scroll(int& dx, int& dy)
{
    dx = 0;
    dy = 0;
    return false;
}
