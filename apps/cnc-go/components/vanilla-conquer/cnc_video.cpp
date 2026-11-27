/**
 * @file cnc_video.cpp
 * @brief Video backend of the ESP32-S3 Command & Conquer port (replaces
 * video_sdl2.cpp).
 *
 * The game draws in 8-bit surfaces (320x200, DOS low resolution mode).
 * Video_Render_Frame() (called by the game's frame limiter) only requests a
 * refresh: a task on core 0 converts the visible surface through the palette to
 * RGB565, stretches it to the 320x240 panel (4:3, as on the original monitors),
 * draws the mouse cursor on top and sends it to the LCD with DMA. The game
 * keeps running on core 1 meanwhile (like a DOS game drawing in VGA memory).
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gbuffer.h"
#include "palette.h"
#include "video.h"
#include "wwmouse.h"
#include "display.h"
#include "font8x8.h"
#include "cnc_platform.h"

#define LINES_PER_CHUNK         8           // LCD lines per DMA transfer (display.h: max 8)
#define NUM_CHUNK_BUFFERS       (DISPLAY_MAX_QUEUED + 1)
#define VIDEO_TASK_PRIORITY     (configMAX_PRIORITIES - 4)
#define VIDEO_TASK_CORE         0
#define MENU_HEIGHT             200         // menus, briefings... are drawn for 320x200
#define CONSOLE_COLOR_TEXT      15
#define CONSOLE_COLOR_BG        0
#define PROGRESS_RAMP_TITLE     16          // console palette: 16 shades per font
#define PROGRESS_RAMP_TEXT      32
#define PROGRESS_COLOR_BAR      48
#define PROGRESS_COLOR_FRAME    49

static const char* TAG = "video";

class VideoSurfaceESP;
static VideoSurfaceESP* frontSurface;       // GBC_VISIBLE surface of the game
static uint8_t* consoleBuffer;              // boot console (GAME_WIDTH x GAME_HEIGHT)
static bool consoleShown = true;
static int consoleLine;
static uint16_t gamePalette[256];           // RGB565, big endian (as sent to the LCD)
static uint16_t consolePalette[256];
static uint8_t gamePaletteRaw[768];         // as set by the game (6-bit components)
static uint8_t consolePaletteRaw[768];
static uint16_t* chunkBuffers[NUM_CHUNK_BUFFERS];
static SemaphoreHandle_t frameRequest;
static volatile uint32_t frameCount;
static volatile bool videoShutdownRequested;
static portMUX_TYPE cursorMux = portMUX_INITIALIZER_UNLOCKED;
static struct
{
    const uint8_t* shape;   // 8-bit, color 0 transparent
    int w, h, hotX, hotY;
    float x, y;
    bool clip;
} cursor = {nullptr, 0, 0, 0, 0, GAME_WIDTH / 2, GAME_HEIGHT / 2, false};

static inline uint16_t rgb565be(int r, int g, int b)
{
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);
}

/*
** Surfaces: plain 8-bit buffers. The large ones go to internal RAM if possible
** (much faster than PSRAM for the many small reads/writes of the game).
*/
static void* allocSurfaceMemory(size_t size, bool preferInternal)
{
    void* p = nullptr;
    if (preferInternal) {
        p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!p) {
        p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!p) {
        p = malloc(size);
    }
    return p;
}

class VideoSurfaceESP : public VideoSurface
{
public:
    VideoSurfaceESP(int w, int h, GBC_Enum flags)
        : Width(w)
        , Height(h)
        , Flags(flags)
    {
        // the visible and hidden pages are used at every frame
        Data = (uint8_t*)allocSurfaceMemory(w * h, (flags & (GBC_VISIBLE | GBC_VIDEOMEM)) != 0);
        if (Data) {
            memset(Data, 0, w * h);
        }
        ESP_LOGI(TAG,
                 "surface %dx%d flags %d at %p (%s)",
                 w,
                 h,
                 flags,
                 Data,
                 esp_ptr_external_ram(Data) ? "PSRAM" : "internal");
        if (flags & GBC_VISIBLE) {
            frontSurface = this;
            consoleShown = false;
        }
    }

    virtual ~VideoSurfaceESP()
    {
        if (frontSurface == this) {
            frontSurface = nullptr;
            // let the display task finish with this buffer
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        free(Data);
    }

    virtual void* GetData() const
    {
        return Data;
    }
    virtual int GetPitch() const
    {
        return Width;
    }
    virtual bool IsAllocated() const
    {
        return false;
    }
    virtual void AddAttachedSurface(VideoSurface* surface)
    {
    }
    virtual bool IsReadyToBlit()
    {
        return true;
    }
    virtual bool LockWait()
    {
        return Data != nullptr;
    }
    virtual bool Unlock()
    {
        return true;
    }

    virtual void Blt(const Rect& destRect, VideoSurface* src, const Rect& srcRect, bool mask)
    {
        VideoSurfaceESP* s = (VideoSurfaceESP*)src;
        int w = srcRect.Width, h = srcRect.Height;
        int sx = srcRect.X, sy = srcRect.Y, dx = destRect.X, dy = destRect.Y;
        // clip against both surfaces
        if (sx < 0) {
            w += sx;
            dx -= sx;
            sx = 0;
        }
        if (sy < 0) {
            h += sy;
            dy -= sy;
            sy = 0;
        }
        if (dx < 0) {
            w += dx;
            sx -= dx;
            dx = 0;
        }
        if (dy < 0) {
            h += dy;
            sy -= dy;
            dy = 0;
        }
        if (sx + w > s->Width)
            w = s->Width - sx;
        if (sy + h > s->Height)
            h = s->Height - sy;
        if (dx + w > Width)
            w = Width - dx;
        if (dy + h > Height)
            h = Height - dy;
        if (w <= 0 || h <= 0) {
            return;
        }
        const uint8_t* sp = s->Data + sy * s->Width + sx;
        uint8_t* dp = Data + dy * Width + dx;
        // overlapping copies within the same surface: go bottom up when needed
        int step = 1;
        if (s == this && dy > sy) {
            sp += (h - 1) * s->Width;
            dp += (h - 1) * Width;
            step = -1;
        }
        for (int y = 0; y < h; y++) {
            if (mask) {
                for (int x = 0; x < w; x++) {
                    if (sp[x]) {
                        dp[x] = sp[x];
                    }
                }
            } else {
                memmove(dp, sp, w);
            }
            sp += step * s->Width;
            dp += step * Width;
        }
    }

    virtual void FillRect(const Rect& rect, unsigned char color)
    {
        // the rectangle is inclusive (see GraphicViewPortClass::Fill_Rect())
        int x0 = rect.X < 0 ? 0 : rect.X;
        int y0 = rect.Y < 0 ? 0 : rect.Y;
        int x1 = rect.X + rect.Width + 1;
        int y1 = rect.Y + rect.Height + 1;
        if (x1 > Width)
            x1 = Width;
        if (y1 > Height)
            y1 = Height;
        for (int y = y0; y < y1; y++) {
            memset(Data + y * Width + x0, color, x1 - x0);
        }
    }

    // display task
    const uint8_t* Line(int y) const
    {
        return Data + y * Width;
    }
    int Get_Width() const
    {
        return Width;
    }
    int Get_Height() const
    {
        return Height;
    }

private:
    uint8_t* Data;
    int Width;
    int Height;
    GBC_Enum Flags;
};

/*
** Outside of the map view (main loop), the game draws 320x200 screens: they are
** shown centered vertically. Returns the LCD line of the first game line.
*/
static int screenYOffset(int srcH)
{
    if (!consoleShown && frontSurface && !cursor.clip && srcH > MENU_HEIGHT) {
        return (LCD_HEIGHT - MENU_HEIGHT) / 2;
    }
    return 0;
}

/*
** Display task
*/
static void drawCursor(uint16_t* out, int gameY, const uint16_t* pal, int cx, int cy, const uint8_t* shape, int cw, int ch)
{
    int row = gameY - cy;
    if (!shape || row < 0 || row >= ch) {
        return;
    }
    const uint8_t* s = shape + row * cw;
    for (int i = 0; i < cw; i++) {
        int x = cx + i;
        if (s[i] && x >= 0 && x < GAME_WIDTH) {
            out[x] = pal[s[i]];
        }
    }
}

static void videoTask(void* arg)
{
    while (!videoShutdownRequested) {
        xSemaphoreTake(frameRequest, portMAX_DELAY);
        if (videoShutdownRequested) {
            break;
        }
        const uint8_t* src = consoleBuffer;
        const uint16_t* pal = consolePalette;
        int srcW = GAME_WIDTH, srcH = GAME_HEIGHT;
        VideoSurfaceESP* front = frontSurface;
        bool drawMouse = false;
        if (!consoleShown && front) {
            src = (const uint8_t*)front->GetData();
            srcW = front->Get_Width();
            srcH = front->Get_Height();
            pal = gamePalette;
            drawMouse = !Get_Mouse_State();
        }
        int cx = 0, cy = 0, cw = 0, ch = 0;
        const uint8_t* shape = nullptr;
        if (drawMouse) {
            portENTER_CRITICAL(&cursorMux);
            shape = cursor.shape;
            cw = cursor.w;
            ch = cursor.h;
            cx = (int)cursor.x - cursor.hotX;
            cy = (int)cursor.y - cursor.hotY;
            portEXIT_CRITICAL(&cursorMux);
        }
        int yOffset = screenYOffset(srcH);
        if (yOffset) {
            srcH = MENU_HEIGHT;
        }
        displaySetWindow(0, LCD_HEIGHT);
        for (int chunk = 0; chunk < LCD_HEIGHT / LINES_PER_CHUNK; chunk++) {
            uint16_t* buf = chunkBuffers[chunk % NUM_CHUNK_BUFFERS];
            // the buffer of transfer (chunk - NUM_CHUNK_BUFFERS) must be done
            if (chunk >= DISPLAY_MAX_QUEUED) {
                displayWaitOne();
            }
            for (int l = 0; l < LINES_PER_CHUNK; l++) {
                int lcdY = chunk * LINES_PER_CHUNK + l;
                // centered menu screen, or game screen stretched to the panel if smaller
                int gameY = yOffset ? lcdY - yOffset : lcdY * srcH / LCD_HEIGHT;
                uint16_t* out = buf + l * LCD_WIDTH;
                if (gameY >= 0 && gameY < srcH) {
                    const uint8_t* in = src + gameY * srcW;
                    int w = srcW < LCD_WIDTH ? srcW : LCD_WIDTH;
                    for (int x = 0; x < w; x += 4) {
                        out[x] = pal[in[x]];
                        out[x + 1] = pal[in[x + 1]];
                        out[x + 2] = pal[in[x + 2]];
                        out[x + 3] = pal[in[x + 3]];
                    }
                    if (w < LCD_WIDTH) {
                        memset(out + w, 0, (LCD_WIDTH - w) * 2);
                    }
                    if (drawMouse) {
                        drawCursor(out, gameY, pal, cx, cy, shape, cw, ch);
                    }
                } else {
                    memset(out, 0, LCD_WIDTH * 2);
                }
            }
            if (videoShutdownRequested) {
                break;
            }
            displaySendPixels(buf, LCD_WIDTH * LINES_PER_CHUNK, chunk == LCD_HEIGHT / LINES_PER_CHUNK - 1);
        }
        if (!videoShutdownRequested) {
            displayWaitAll();
            frameCount++;
        }
    }
    vTaskDelete(NULL);
}

static void requestFrame(void)
{
    if (frameRequest && !videoShutdownRequested) {
        xSemaphoreGive(frameRequest);
    }
}

extern "C" void videoShutdown(void)
{
    videoShutdownRequested = true;
    if (frameRequest) {
        xSemaphoreGive(frameRequest);
    }
}

extern "C" void videoInit(void)
{
    for (int i = 0; i < NUM_CHUNK_BUFFERS; i++) {
        chunkBuffers[i] = (uint16_t*)heap_caps_malloc(
            LCD_WIDTH * LINES_PER_CHUNK * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(chunkBuffers[i]);
    }
    consoleBuffer = (uint8_t*)heap_caps_calloc(1, GAME_WIDTH * GAME_HEIGHT, MALLOC_CAP_SPIRAM);
    // console palette: EGA like
    static const uint8_t ega[16][3] = {{0, 0, 0},
                                       {0, 0, 170},
                                       {0, 170, 0},
                                       {0, 170, 170},
                                       {170, 0, 0},
                                       {170, 0, 170},
                                       {170, 85, 0},
                                       {170, 170, 170},
                                       {85, 85, 85},
                                       {85, 85, 255},
                                       {85, 255, 85},
                                       {85, 255, 255},
                                       {255, 85, 85},
                                       {255, 85, 255},
                                       {255, 255, 85},
                                       {255, 255, 255}};
    for (int i = 0; i < 16; i++) {
        consolePalette[i] = rgb565be(ega[i][0], ega[i][1], ega[i][2]);
        for (int c = 0; c < 3; c++) {
            consolePaletteRaw[i * 3 + c] = ega[i][c] >> 2;
        }
    }
    // progress screen: shades for the game fonts (pixel value 1 = main color), bar
    static const uint8_t extra[][3] = {
        {255, 210, 120}, {255, 230, 170}, {240, 190, 100}, {220, 170, 80}, {200, 150, 60}, {180, 130, 50},
        {160, 110, 40}, {140, 95, 35}, {120, 80, 30}, {100, 65, 25}, {80, 50, 20}, {60, 40, 15},
        {50, 30, 10}, {40, 25, 8}, {30, 20, 5},                   // 17..31: title (gold)
        {220, 220, 220}, {240, 240, 240}, {200, 200, 200}, {180, 180, 180}, {160, 160, 160}, {140, 140, 140},
        {120, 120, 120}, {100, 100, 100}, {90, 90, 90}, {80, 80, 80}, {70, 70, 70}, {60, 60, 60},
        {50, 50, 50}, {40, 40, 40}, {30, 30, 30},                 // 33..47: text (gray)
        {200, 40, 30},                                            // 48: bar
        {150, 150, 150},                                          // 49: bar frame
    };
    for (int i = 0; i < (int)(sizeof(extra) / sizeof(extra[0])); i++) {
        int index = PROGRESS_RAMP_TITLE + 1 + i;
        if (i >= 30) {
            index = PROGRESS_COLOR_BAR + (i - 30);
        } else if (i >= 15) {
            index = PROGRESS_RAMP_TEXT + 1 + (i - 15);
        }
        consolePalette[index] = rgb565be(extra[i][0], extra[i][1], extra[i][2]);
        for (int c = 0; c < 3; c++) {
            consolePaletteRaw[index * 3 + c] = extra[i][c] >> 2;
        }
    }
    // binary semaphore: requests while a frame is being sent are merged in one
    frameRequest = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(videoTask, "video", 4096, nullptr, VIDEO_TASK_PRIORITY, nullptr, VIDEO_TASK_CORE);
}

extern "C" void videoConsolePrint(const char* format, ...)
{
    char text[128];
    va_list va;
    va_start(va, format);
    vsnprintf(text, sizeof(text), format, va);
    va_end(va);
    printf("%s\n", text);
    if (!consoleBuffer) {
        return;
    }
    // word wrap is not needed for our messages: split at the screen width
    const char* p = text;
    do {
        if (consoleLine >= GAME_HEIGHT / FONT_HEIGHT) {
            memmove(consoleBuffer,
                    consoleBuffer + GAME_WIDTH * FONT_HEIGHT,
                    GAME_WIDTH * (GAME_HEIGHT - FONT_HEIGHT));
            consoleLine--;
        }
        uint8_t* line = consoleBuffer + consoleLine * FONT_HEIGHT * GAME_WIDTH;
        memset(line, CONSOLE_COLOR_BG, GAME_WIDTH * FONT_HEIGHT);
        for (int c = 0; c < GAME_WIDTH / FONT_WIDTH && *p; c++, p++) {
            const uint8_t* glyph = font8x8_basic[*p & 0x7F];
            for (int y = 0; y < FONT_HEIGHT; y++) {
                for (int x = 0; x < FONT_WIDTH; x++) {
                    if (glyph[y] & (1 << x)) {
                        line[y * GAME_WIDTH + c * FONT_WIDTH + x] = CONSOLE_COLOR_TEXT;
                    }
                }
            }
        }
        consoleLine++;
    } while (*p);
    requestFrame();
}

// cnc_gamehooks.cpp: a font of the game (Westwood .FNT), or null if not loaded yet
extern "C" const void* cncProgressFont(int big);


static uint8_t* drawTarget;                 // GAME_WIDTH x GAME_HEIGHT

// Text in the 8x8 console font, glyphs scaled up
static void drawText(int x0, int y0, int scale, uint8_t color, const char* text)
{
    for (const char* p = text; *p; p++, x0 += FONT_WIDTH * scale) {
        const uint8_t* glyph = font8x8_basic[*p & 0x7F];
        for (int y = 0; y < FONT_HEIGHT * scale; y++) {
            for (int x = 0; x < FONT_WIDTH * scale; x++) {
                int px = x0 + x, py = y0 + y;
                if ((glyph[y / scale] & (1 << (x / scale))) && px >= 0 && px < GAME_WIDTH && py >= 0
                    && py < GAME_HEIGHT) {
                    drawTarget[py * GAME_WIDTH + px] = color;
                }
            }
        }
    }
}

// Westwood font: header offsets (little endian 16-bit)
static inline int fntWord(const uint8_t* f, int offset)
{
    return f[offset] | (f[offset + 1] << 8);
}

static int fontTextWidth(const uint8_t* f, const char* text)
{
    const uint8_t* widths = f + fntWord(f, 8);
    int w = 0;
    for (const uint8_t* p = (const uint8_t*)text; *p; p++) {
        w += widths[*p] + 1;
    }
    return w;
}

// Text in a game font: the 4-bit pixel values are shades base + value, up to maxValue
// (the higher values are shadows and outlines that a plain text print leaves out)
static void drawFontText(const uint8_t* f, int x0, int y0, uint8_t base, int maxValue, const char* text)
{
    const uint8_t* widths = f + fntWord(f, 8);
    for (const uint8_t* p = (const uint8_t*)text; *p; p++) {
        int c = *p;
        int width = widths[c];
        const uint8_t* data = f + fntWord(f, fntWord(f, 6) + 2 * c);
        int lle = fntWord(f, fntWord(f, 12) + 2 * c);
        int top = lle & 0xFF, lines = lle >> 8;
        for (int y = 0; y < lines; y++) {
            for (int x = 0; x < width; x++) {
                int v = (x & 1) ? data[x / 2] >> 4 : data[x / 2] & 0x0F;
                int px = x0 + x, py = y0 + top + y;
                if (v && v <= maxValue && px >= 0 && px < GAME_WIDTH && py >= 0 && py < GAME_HEIGHT) {
                    drawTarget[py * GAME_WIDTH + px] = base + v;
                }
            }
            data += (width + 1) / 2;
        }
        x0 += width + 1;
    }
}

// Centered text, in the game font if there is one; returns the height used
static int drawCentered(int y, bool big, const char* text)
{
    const uint8_t* f = (const uint8_t*)cncProgressFont(big);
    if (f) {
        drawFontText(f,
                     (GAME_WIDTH - fontTextWidth(f, text)) / 2,
                     y,
                     big ? PROGRESS_RAMP_TITLE : PROGRESS_RAMP_TEXT,
                     big ? 15 : 1,
                     text);
        return f[18];
    }
    int scale = big ? 2 : 1;
    drawText((GAME_WIDTH - (int)strlen(text) * FONT_WIDTH * scale) / 2, y, scale, big ? 15 : 7, text);
    return FONT_HEIGHT * scale;
}

extern "C" void videoProgress(const char* title, const char* detail, const char* note, int percent)
{
    if (!consoleBuffer) {
        return;
    }
    // composed aside then copied: the video task never sees a half drawn screen
    static uint8_t* scratch;
    if (!scratch) {
        scratch = (uint8_t*)heap_caps_malloc(GAME_WIDTH * GAME_HEIGHT, MALLOC_CAP_SPIRAM);
        if (!scratch) {
            return;
        }
    }
    drawTarget = scratch;
    memset(scratch, CONSOLE_COLOR_BG, GAME_WIDTH * GAME_HEIGHT);
    drawCentered(66, true, title);
    drawCentered(96, false, detail);
    // bar
    const int barX = 50, barY = 116, barW = GAME_WIDTH - 2 * barX, barH = 10;
    int fill = (barW - 4) * (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100;
    for (int y = 0; y < barH; y++) {
        uint8_t* line = scratch + (barY + y) * GAME_WIDTH + barX;
        bool edge = y == 0 || y == barH - 1;
        for (int x = 0; x < barW; x++) {
            if (edge || x == 0 || x == barW - 1) {
                line[x] = PROGRESS_COLOR_FRAME;
            } else if (y >= 2 && y < barH - 2 && x >= 2 && x < 2 + fill) {
                line[x] = PROGRESS_COLOR_BAR;
            }
        }
    }
    char text[16];
    snprintf(text, sizeof(text), "%d%%", percent);
    drawCentered(134, false, text);
    drawCentered(180, false, note);
    drawTarget = consoleBuffer;
    memcpy(consoleBuffer, scratch, GAME_WIDTH * GAME_HEIGHT);
    consoleLine = 0;
    consoleShown = true;
    requestFrame();
}

extern "C" bool videoGamePaletteVisible(void)
{
    int sum = 0;
    for (int i = 0; i < 768; i++) {
        sum += gamePaletteRaw[i];
    }
    return frontSurface != nullptr && sum > 768;
}

extern "C" void videoShowConsole(void)
{
    consoleShown = true;
    requestFrame();
}

extern "C" void videoHideConsole(void)
{
    if (frontSurface) {
        consoleShown = false;
        requestFrame();
    }
}

// Size of the screen shown
static void screenSize(int* w, int* h)
{
    VideoSurfaceESP* front = frontSurface;
    if (!consoleShown && front) {
        *w = front->Get_Width();
        *h = front->Get_Height();
    } else {
        *w = GAME_WIDTH;
        *h = GAME_HEIGHT;
    }
}

extern "C" void videoSetMouse(float x, float y)
{
    int w, h;
    screenSize(&w, &h);
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x > w - 1)
        x = w - 1;
    if (y > h - 1)
        y = h - 1;
    portENTER_CRITICAL(&cursorMux);
    cursor.x = x;
    cursor.y = y;
    portEXIT_CRITICAL(&cursorMux);
}

extern "C" void videoMoveMouse(float dx, float dy)
{
    videoSetMouse(cursor.x + dx, cursor.y + dy);
}

extern "C" void videoGetMouse(int* x, int* y)
{
    *x = (int)cursor.x;
    *y = (int)cursor.y;
}

extern "C" void videoLcdToGame(int lx, int ly, int* gx, int* gy)
{
    int w, h;
    screenSize(&w, &h);
    int yOffset = screenYOffset(h);
    *gx = lx * w / LCD_WIDTH;
    if (yOffset) {
        int y = ly - yOffset;
        *gy = y < 0 ? 0 : (y >= MENU_HEIGHT ? MENU_HEIGHT - 1 : y);
    } else {
        *gy = ly * h / LCD_HEIGHT;
    }
}

extern "C" uint32_t videoGetFrameCount(void)
{
    return frameCount;
}

extern "C" bool videoGetScreen(const uint8_t** pixels, int* w, int* h, const uint8_t** palette)
{
    VideoSurfaceESP* front = frontSurface;
    if (!consoleShown && front) {
        *pixels = (const uint8_t*)front->GetData();
        *w = front->Get_Width();
        *h = front->Get_Height();
        *palette = gamePaletteRaw;
    } else {
        *pixels = consoleBuffer;
        *w = GAME_WIDTH;
        *h = GAME_HEIGHT;
        *palette = consolePaletteRaw;
    }
    return *pixels != nullptr;
}

/*
** Game side video API (video.h)
*/
class SurfaceMonitorClassDummy : public SurfaceMonitorClass
{
public:
    virtual void Restore_Surfaces()
    {
    }
    virtual void Set_Surface_Focus(bool in_focus)
    {
    }
    virtual void Release()
    {
    }
};

SurfaceMonitorClassDummy AllSurfacesDummy;
SurfaceMonitorClass& AllSurfaces = AllSurfacesDummy;

SurfaceMonitorClass::SurfaceMonitorClass()
{
    SurfacesRestored = false;
}

bool Set_Video_Mode(int w, int h, int bits_per_pixel)
{
    printf("[cnc_video] Set_Video_Mode %dx%d %d bpp -> %s\n", w, h, bits_per_pixel,
           (w <= LCD_WIDTH && bits_per_pixel == 8) ? "OK" : "FAILED");
    return w <= LCD_WIDTH && bits_per_pixel == 8;
}

void Reset_Video_Mode(void)
{
}

void Toggle_Video_Fullscreen()
{
}

bool Is_Video_Fullscreen()
{
    return true;
}

void Get_Video_Scale(float& x, float& y)
{
    x = 1.0f;
    y = 1.0f;
}

void Set_Video_Cursor_Clip(bool clipped)
{
    cursor.clip = clipped;
}

void Move_Video_Mouse(float xrel, float yrel)
{
    videoMoveMouse(xrel, yrel);
}

void Get_Video_Mouse(int& x, int& y)
{
    videoGetMouse(&x, &y);
}

void Set_Video_Cursor(void* shape, int w, int h, int hotx, int hoty)
{
    portENTER_CRITICAL(&cursorMux);
    cursor.shape = (const uint8_t*)shape;
    cursor.w = w;
    cursor.h = h;
    cursor.hotX = hotx;
    cursor.hotY = hoty;
    portEXIT_CRITICAL(&cursorMux);
}

unsigned int Get_Free_Video_Memory(void)
{
    return 1000000000;
}

unsigned Get_Video_Hardware_Capabilities(void)
{
    return VIDEO_BLITTER;
}

void Wait_Vert_Blank(void)
{
}

void Wait_Blit(void)
{
}

void Set_DD_Palette(void* rpalette)
{
    const uint8_t* p = (const uint8_t*)rpalette;
    memcpy(gamePaletteRaw, p, sizeof(gamePaletteRaw));
    for (int i = 0; i < 256; i++) {
        // 6-bit VGA components
        int r = p[i * 3] & 0x3F, g = p[i * 3 + 1] & 0x3F, b = p[i * 3 + 2] & 0x3F;
        gamePalette[i] = rgb565be((r << 2) | (r >> 4), (g << 2) | (g >> 4), (b << 2) | (b >> 4));
    }
}

void Video_Render_Frame()
{
    requestFrame();
}

Video::Video()
{
}

Video::~Video()
{
}

Video& Video::Shared()
{
    static Video video;
    return video;
}

VideoSurface* Video::CreateSurface(int w, int h, GBC_Enum flags)
{
    return new VideoSurfaceESP(w, h, flags);
}
