/**
 * @file cnc_platform.h
 * @brief Interfaces between the ESP32-S3 platform layer files of the
 * Command & Conquer port (cnc_*.cpp) and Retro-Go.
 */
#ifndef CNC_PLATFORM_H
#define CNC_PLATFORM_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAME_WIDTH      320
#define GAME_HEIGHT     240

// ---- cnc_video.cpp ----
void videoInit(void);
void videoConsolePrint(const char* format, ...) __attribute__((format(printf, 1, 2)));
void videoShowConsole(void);
void videoProgress(const char* title, const char* detail, const char* note, int percent);
void videoHideConsole(void);
void videoSetMouse(float x, float y);
void videoMoveMouse(float dx, float dy);
void videoGetMouse(int* x, int* y);
void videoLcdToGame(int lx, int ly, int* gx, int* gy);
uint32_t videoGetFrameCount(void);
bool videoGetScreen(const uint8_t** pixels, int* w, int* h, const uint8_t** palette);

// ---- cnc_input.cpp ----
void inputInit(void);
void inputInjectClick(int x, int y, bool right);
void inputInjectDrag(int x0, int y0, int x1, int y1);
void inputInjectKey(int scancode);
void inputInjectScroll(int dx, int dy);

// ---- cnc_debug.cpp ----
void debugInit(void);

// ---- cnc_sound.cpp ----
uint32_t soundGetUnderruns(void);

#ifdef __cplusplus
}
#endif
#endif
