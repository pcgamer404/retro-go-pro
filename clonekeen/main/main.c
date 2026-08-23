// main.c -- retro-go entry point for CloneKeen
//
// Replaces the original standalone app_main.c, which only spawned
// cgTask() with no retro-go init at all. This mirrors the structure of
// every other retro-go app (see main_nes.c, oswan/main.c, etc).
//
// NOTE: rg_system_init()'s exact signature has changed across the
// different retro-go core versions seen in this project (some take
// (sampleRate, handlers, NULL), others take a single rg_config_t*).
// Match whichever form your current components/retro-go/rg_system.h
// declares -- both are shown below, uncomment the one that applies.

#include <rg_system.h>
#include <unistd.h>
#include "keen.h"
#include "kmain.h"

static rg_app_t *app;

static void cgTask(void *pvParameters)
{
    chdir("/sd/roms/keen");
    char *argv[] = {"/sd/roms/keen/keen.exe", NULL};
    KeenMain(1, argv);

    // KeenMain() should not normally return, but if it does, exit
    // cleanly back to the launcher rather than falling off a task.
    rg_system_exit();
}

void app_main(void)
{
    // --- Option A: newer rg_system.h (single rg_config_t*) ---
    // const rg_handlers_t handlers = {0};
    // const rg_config_t config = {
    //     .sampleRate = 44100,
    //     .handlers = handlers,
    // };
    // app = rg_system_init(&config);

    // --- Option B: older rg_system.h (sampleRate, handlers, NULL) ---
    const rg_handlers_t handlers = {0};
    app = rg_system_init(44100, &handlers, NULL);

    rg_system_set_overclock(3);
    rg_system_set_tick_rate(60);

    xTaskCreatePinnedToCore(&cgTask, "cgTask", 32000, NULL, 2, NULL, 1);
}
