#if !defined(ESP_PLATFORM) && !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oj_sdl_event_compat.h"
#include <string.h>

#define OJ_EVENT_QUEUE_CAPACITY 32
static SDL_Event s_queue[OJ_EVENT_QUEUE_CAPACITY];
static unsigned s_head;
static unsigned s_count;

int SDL_PushEvent(SDL_Event *event)
{
    if (!event || s_count >= OJ_EVENT_QUEUE_CAPACITY) return -1;
    unsigned tail = (s_head + s_count) % OJ_EVENT_QUEUE_CAPACITY;
    s_queue[tail] = *event;
    ++s_count;
    return 1;
}

int SDL_PollEvent(SDL_Event *event)
{
    if (!event || s_count == 0) return 0;
    *event = s_queue[s_head];
    s_head = (s_head + 1) % OJ_EVENT_QUEUE_CAPACITY;
    --s_count;
    return 1;
}

void oj_sdl_event_queue_clear(void)
{
    s_head = 0;
    s_count = 0;
}

void oj_sdl_push_key_event(Sint32 keycode, int pressed)
{
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.type = event.type;
    event.key.key = keycode;
    event.key.keysym.sym = keycode;
    (void)SDL_PushEvent(&event);
}

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static Uint32 s_time_skip;
void oj_sdl_time_skip(Uint32 ms) { s_time_skip += ms; }
Uint32 SDL_GetTicks(void) { return (Uint32)(esp_timer_get_time() / 1000ULL) - s_time_skip; }
void SDL_Delay(Uint32 ms)
{
    if (ms == 0) { taskYIELD(); return; }
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0) ticks = 1;
    vTaskDelay(ticks);
}
#else
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
static Uint32 s_time_skip;
void oj_sdl_time_skip(Uint32 ms) { s_time_skip += ms; }
Uint32 SDL_GetTicks(void) { return (Uint32)GetTickCount() - s_time_skip; }
void SDL_Delay(Uint32 ms) { Sleep(ms); }
#else
static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
static Uint32 s_time_skip;
void oj_sdl_time_skip(Uint32 ms) { s_time_skip += ms; }
Uint32 SDL_GetTicks(void) { return (Uint32)monotonic_ms() - s_time_skip; }
void SDL_Delay(Uint32 ms)
{
    struct timespec req = { (time_t)(ms / 1000U), (long)(ms % 1000U) * 1000000L };
    while (nanosleep(&req, &req) != 0) { }
}
#endif
#endif
