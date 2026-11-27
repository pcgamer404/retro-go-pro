/**
 * @file display.h
 * @brief Retro-Go display adapter for Command & Conquer.
 */
#ifndef DISPLAYH
#define DISPLAYH
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_WIDTH           320
#define LCD_HEIGHT          240
#define DISPLAY_MAX_QUEUED  2

void displayInit(void);
bool displayIsReady(void);
void displaySetWindow(int firstLine, int numLines);
void displaySendPixels(const uint16_t *data, size_t numPixels, bool last);
int displayWaitOne(void);
void displayWaitAll(void);

#ifdef __cplusplus
}
#endif
#endif
