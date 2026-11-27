/**
 * @file display.c
 * @brief Retro-Go display adapter implementation.
 */
#include "display.h"
#include <rg_system.h>
#include <rg_display.h>
#include <rg_surface.h>
#include <string.h>

static rg_surface_t *s_surfaces[2] = {NULL, NULL};
static int s_current_surf = 0;
static int s_currentLine = 0;

void displayInit(void)
{
    s_surfaces[0] = rg_surface_create(LCD_WIDTH, LCD_HEIGHT, RG_PIXEL_565_BE, MEM_SLOW | MEM_NOPANIC);
    s_surfaces[1] = rg_surface_create(LCD_WIDTH, LCD_HEIGHT, RG_PIXEL_565_BE, MEM_SLOW | MEM_NOPANIC);
    if (!s_surfaces[0] || !s_surfaces[1]) {
        RG_PANIC("Failed to allocate display surfaces");
    }
    s_current_surf = 0;
    s_currentLine = 0;
}

bool displayIsReady(void)
{
    return true;
}

void displaySetWindow(int firstLine, int numLines)
{
    s_currentLine = firstLine;
}

void displaySendPixels(const uint16_t *data, size_t numPixels, bool last)
{
    if (!s_surfaces[s_current_surf]) return;

    int lines = (int)(numPixels / LCD_WIDTH);
    if (lines <= 0) return;

    if (s_currentLine + lines > LCD_HEIGHT) {
        lines = LCD_HEIGHT - s_currentLine;
    }
    if (lines <= 0) return;

    uint16_t *dst = (uint16_t*)s_surfaces[s_current_surf]->data + (s_currentLine * LCD_WIDTH);
    memcpy(dst, data, lines * LCD_WIDTH * sizeof(uint16_t));
    s_currentLine += lines;

    if (last || s_currentLine >= LCD_HEIGHT) {
        rg_display_submit(s_surfaces[s_current_surf], 0);
        s_current_surf ^= 1;
        s_currentLine = 0;
    }
}

int displayWaitOne(void)
{
    return 0;
}

void displayWaitAll(void)
{
    while (rg_display_is_busy()) {
        rg_task_yield();
    }
}
