/**
 * @file cnc_debug.cpp
 * @brief Debug console on the USB serial port of the ESP32-S3 Command & Conquer port.
 *
 * Commands (one per line):
 *   shot              dumps the screen: "SHOT <w> <h>", then the 768 byte palette
 *                     and the 8-bit pixels, hex encoded, then "ENDSHOT"
 *   tap <x> <y>       left click at game coordinates
 *   rtap <x> <y>      right click at game coordinates
 *   move <x> <y>      moves the mouse cursor
 *   drag <x0> <y0> <x1> <y1>   left button drag (band box selection)
 *   key <scancode>    presses and releases a key (SDL scancode = HID usage)
 *   scroll <dx> <dy>  scrolls the map (game pixels)
 *   gdi / nod         from the main menu: starts a new campaign on that side
 *   trace <0|1>       logs the input events given to the game
 *   win               wins the current mission (score screen)
 *   units             lists the infantry, vehicles and aircraft
 *   retrieve <file>   looks a file up in the MIX files
 *   mem               heap statistics
 *   tasks             task stack high water marks
 * tools/cnc_console.py talks to it and turns screenshots into PNG files.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "cnc_platform.h"

extern "C" void Debug_Retrieve(const char* name);
extern "C" void Debug_Units(void);
extern "C" void Cnc_Debug_Win(void);
extern bool inputTrace;

#define DEBUG_TASK_PRIORITY     2
#define DEBUG_TASK_CORE         0

// Direct writes to the driver (stdout drops data when its buffer is full). The callers
// hold the stdout lock, so no other output is interleaved.
// cnc_flashcache.cpp
extern "C" void cncFlashCacheWipe(void);

static void writeAll(const char* data, size_t len)
{
    while (len) {
        int n = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(1000));
        if (n <= 0) {
            return;
        }
        data += n;
        len -= n;
    }
}

static void dumpHex(const uint8_t* data, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    char line[129];
    while (len) {
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) {
            line[2 * i] = hex[data[i] >> 4];
            line[2 * i + 1] = hex[data[i] & 15];
        }
        line[2 * n] = '\n';
        writeAll(line, 2 * n + 1);
        data += n;
        len -= n;
    }
}

static void screenshot(void)
{
    int w, h;
    const uint8_t* pixels;
    const uint8_t* palette;
    if (!videoGetScreen(&pixels, &w, &h, &palette)) {
        printf("no screen\n");
        return;
    }
    // flockfile(stdout);
    fflush(stdout);
    char header[32];
    int n = snprintf(header, sizeof(header), "SHOT %d %d\n", w, h);
    writeAll(header, n);
    dumpHex(palette, 768);
    dumpHex(pixels, w * h);
    writeAll("ENDSHOT\n", 8);
    // funlockfile(stdout);
}

static void execute(char* line)
{
    char* argv[6];
    int argc = 0;
    for (char* tok = strtok(line, " \t\r\n"); tok && argc < 6; tok = strtok(nullptr, " \t\r\n")) {
        argv[argc++] = tok;
    }
    if (!argc) {
        return;
    }
    int a[4] = {0};
    for (int i = 1; i < argc && i <= 4; i++) {
        a[i - 1] = atoi(argv[i]);
    }
    if (!strcmp(argv[0], "shot")) {
        screenshot();
    } else if (!strcmp(argv[0], "tap")) {
        inputInjectClick(a[0], a[1], false);
    } else if (!strcmp(argv[0], "rtap")) {
        inputInjectClick(a[0], a[1], true);
    } else if (!strcmp(argv[0], "move")) {
        videoSetMouse(a[0], a[1]);
    } else if (!strcmp(argv[0], "drag")) {
        inputInjectDrag(a[0], a[1], a[2], a[3]);
    } else if (!strcmp(argv[0], "key")) {
        inputInjectKey(a[0]);
    } else if (!strcmp(argv[0], "scroll")) {
        inputInjectScroll(a[0], a[1]);
    } else if (!strcmp(argv[0], "gdi") || !strcmp(argv[0], "nod")) {
        // from the main menu: Start New Game, difficulty OK, side
        inputInjectClick(160, 40, false);
        vTaskDelay(pdMS_TO_TICKS(3000));
        inputInjectClick(250, 121, false);
        vTaskDelay(pdMS_TO_TICKS(4000));
        inputInjectClick(argv[0][0] == 'g' ? 85 : 235, 85, false);
        printf("%s mission requested\n", argv[0]);
    } else if (!strcmp(argv[0], "trace")) {
        inputTrace = a[0] != 0;
    } else if (!strcmp(argv[0], "win")) {
        Cnc_Debug_Win();
    } else if (!strcmp(argv[0], "units")) {
        Debug_Units();
    } else if (!strcmp(argv[0], "retrieve") && argc > 1) {
        Debug_Retrieve(argv[1]);
    } else if (!strcmp(argv[0], "reboot")) {
        esp_restart();
    } else if (!strcmp(argv[0], "mixwipe")) {
        cncFlashCacheWipe();
    } else if (!strcmp(argv[0], "mem")) {
        printf("internal: free %u, min %u, largest %u; PSRAM: free %u, min %u, largest %u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    } else if (!strcmp(argv[0], "tasks")) {
        static const char* names[] = {"cnc", "video", "mixer", "touch", "buttons", "blehid", "stats"};
        for (const char* name : names) {
            TaskHandle_t t = xTaskGetHandle(name);
            if (t) {
                printf("%s: %u bytes of stack never used\n", name, (unsigned)uxTaskGetStackHighWaterMark(t));
            }
        }
    } else {
        printf("unknown command: %s\n", argv[0]);
    }
}

static void debugTask(void* arg)
{
    char line[96];
    int len = 0;
    while (1) {
        char c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\n' || c == '\r') {
            line[len] = 0;
            execute(line);
            len = 0;
        } else if (len < (int)sizeof(line) - 1) {
            line[len++] = c;
        }
    }
}

extern "C" void debugInit(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 256;
    cfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        return;
    }
    // stdout through the driver too, so that both can be used
    usb_serial_jtag_vfs_use_driver();
    xTaskCreatePinnedToCore(debugTask, "debug", 3072, nullptr, DEBUG_TASK_PRIORITY, nullptr, DEBUG_TASK_CORE);
}
