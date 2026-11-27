#include "SDL_event.h"
#include <rg_system.h>
#include <string.h>

int keyMode = 1;

Uint8 SDL_EventState(Uint32 type, int state)
{
    return 0;
}

extern int SDL_RG_PollEvent(SDL_Event *event);
extern Uint32 SDL_RG_GetMouseState(int *x, int *y);
extern Uint32 SDL_RG_GetRelativeMouseState(int *x, int *y);

#define EVENT_QUEUE_SIZE 64
static SDL_Event s_event_queue[EVENT_QUEUE_SIZE];
static int s_event_head = 0;
static int s_event_tail = 0;

int SDL_PushEvent(SDL_Event *event)
{
    if (!event) return -1;
    int next = (s_event_tail + 1) % EVENT_QUEUE_SIZE;
    if (next == s_event_head) {
        return -1; // Queue full
    }
    s_event_queue[s_event_tail] = *event;
    s_event_tail = next;
    return 0; // SDL 1.2 standard: 0 on success, -1 on error
}

int SDL_PollEvent(SDL_Event *event)
{
    if (s_event_head != s_event_tail) {
        if (event) {
            *event = s_event_queue[s_event_head];
        }
        s_event_head = (s_event_head + 1) % EVENT_QUEUE_SIZE;
        return 1;
    }
    return SDL_RG_PollEvent(event);
}

int SDL_WaitEvent(SDL_Event *event)
{
    while (!SDL_PollEvent(event)) {
        rg_task_yield();
    }
    return 1;
}

void SDL_PumpEvents(void)
{
    SDL_Event ev;
    while (SDL_RG_PollEvent(&ev)) {
        if (SDL_PushEvent(&ev) != 0) {
            break;
        }
    }
}

Uint32 SDL_GetMouseState(int *x, int *y)
{
    return SDL_RG_GetMouseState(x, y);
}

Uint32 SDL_GetRelativeMouseState(int *x, int *y)
{
    return SDL_RG_GetRelativeMouseState(x, y);
}

int SDL_ShowCursor(int toggle)
{
    return 0;
}

int SDL_EnableUNICODE(int enable)
{
    return 0;
}

Uint8 SDL_GetAppState(void)
{
    return (SDL_APPMOUSEFOCUS | SDL_APPINPUTFOCUS | SDL_APPACTIVE);
}
