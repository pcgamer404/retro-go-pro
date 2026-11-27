// SOTN for Retro-Go - video output.
//
// SOTN renders into a 1024x512 RGB5551 PSX VRAM; the visible area changes with
// the screen (stage 256x240, menu 384x240, title/logos 512x240). Each finished
// frame is converted to RGB565 and scaled (nearest) to a native 320x240 retro-go
// surface using the game's CURRENT display size, so every screen fills the
// panel with the correct 4:3 aspect.
//
// The surface header is static (internal RAM) and the pixels are double-buffered
// in PSRAM: the game's soft rasterizer sits right below the first heap
// blocks, and a stray write past VRAM must not be able to hit the header.

#include <rg_system.h>
#include <string.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define OUT_W 320
#define OUT_H 240
#define VRAM_STRIDE 1024

#if RG_SCREEN_PIXEL_FORMAT == 0
#define FB_PIXEL_FORMAT RG_PIXEL_565_BE
#define OUT16(p) ((uint16_t)(((p) >> 8) | ((p) << 8)))
#else
#define FB_PIXEL_FORMAT RG_PIXEL_565_LE
#define OUT16(p) ((uint16_t)(p))
#endif

void Sotn_DisplaySize(int *w, int *h); // rg_dispinfo.c

static rg_surface_t sHdr;            // static header, never on the heap
static uint32_t *sPixels[2];         // OUT_W x OUT_H, double-buffered in PSRAM
static int sActiveBuf = 0;
static rg_task_t *sCopyTaskH;
static volatile TaskHandle_t sCopyTask;
static SemaphoreHandle_t sSnapDone;
static volatile bool sOffered;
static const uint16_t *volatile sScanSrc;
static volatile int sSrcW = 256, sSrcH = 240;
static volatile bool sHaveFrame;

static uint16_t sColMap[OUT_W];
static uint16_t sRowMap[OUT_H];
static int sMapW, sMapH;

volatile int sotn_lcd_freeze;
unsigned sotn_lcd_drops;

static inline IRAM_ATTR uint16_t px(uint16_t c)
{
    // PSX RGB5551 -> RGB565
    uint16_t p = ((c & 0x1F) << 11) | ((c & 0x03E0) << 1) | ((c >> 4) & 0x20) | ((c >> 10) & 0x1F);
    return OUT16(p);
}

static void buildMaps(int w, int h)
{
    for (int dx = 0; dx < OUT_W; dx++)
        sColMap[dx] = (uint16_t)((dx * w) / OUT_W);
    for (int dy = 0; dy < OUT_H; dy++)
        sRowMap[dy] = (uint16_t)((dy * h) / OUT_H);
    sMapW = w;
    sMapH = h;
    RG_LOGI("PSX display %dx%d -> %dx%d", w, h, OUT_W, OUT_H);
}

static IRAM_ATTR void convertFrame(const uint16_t *src, int w, int h, uint32_t *dst)
{
    // Fast path: standard 256x240 gameplay resolution -> 320x240 (exact 4:5 scaling)
    if (w == 256 && h == OUT_H)
    {
        for (int y = 0; y < OUT_H; y++)
        {
            const uint16_t *row = src + (size_t)y * VRAM_STRIDE;
            // 256 input pixels -> 320 output pixels: 32 chunks of 8 input -> 10 output pixels
            for (int x = 0; x < 256; x += 8)
            {
                uint32_t in01 = *(const uint32_t *)(row + x);
                uint32_t in23 = *(const uint32_t *)(row + x + 2);
                uint32_t in45 = *(const uint32_t *)(row + x + 4);
                uint32_t in67 = *(const uint32_t *)(row + x + 6);

                uint16_t s0 = (uint16_t)in01;
                uint16_t s1 = (uint16_t)(in01 >> 16);
                uint16_t s2 = (uint16_t)in23;
                uint16_t s3 = (uint16_t)(in23 >> 16);
                uint16_t s4 = (uint16_t)in45;
                uint16_t s5 = (uint16_t)(in45 >> 16);
                uint16_t s6 = (uint16_t)in67;
                uint16_t s7 = (uint16_t)(in67 >> 16);

                uint32_t d0 = px(s0), d1 = d0;
                uint32_t d2 = px(s1), d3 = px(s2), d4 = px(s3);
                uint32_t d5 = px(s4), d6 = d5;
                uint32_t d7 = px(s5), d8 = px(s6), d9 = px(s7);

                *dst++ = d0 | (d1 << 16);
                *dst++ = d2 | (d3 << 16);
                *dst++ = d4 | (d5 << 16);
                *dst++ = d6 | (d7 << 16);
                *dst++ = d8 | (d9 << 16);
            }
        }
        return;
    }

    // Fast path: 320x240 native 1:1
    if (w == OUT_W && h == OUT_H)
    {
        for (int y = 0; y < OUT_H; y++)
        {
            const uint16_t *row = src + (size_t)y * VRAM_STRIDE;
            for (int x = 0; x < OUT_W; x += 2)
            {
                uint32_t in01 = *(const uint32_t *)(row + x);
                *dst++ = (uint32_t)px((uint16_t)in01) | ((uint32_t)px((uint16_t)(in01 >> 16)) << 16);
            }
        }
        return;
    }

    // Fast path: 384x240 menu/name entry -> 320x240 (exact 6:5 downscale)
    if (w == 384 && h == OUT_H)
    {
        for (int y = 0; y < OUT_H; y++)
        {
            const uint16_t *row = src + (size_t)y * VRAM_STRIDE;
            // 384 input -> 320 output: 32 blocks of 12 input -> 10 output pixels
            for (int x = 0; x < 384; x += 12)
            {
                uint32_t in01 = *(const uint32_t *)(row + x + 0);
                uint32_t in23 = *(const uint32_t *)(row + x + 2);
                uint32_t in45 = *(const uint32_t *)(row + x + 4);
                uint32_t in67 = *(const uint32_t *)(row + x + 6);
                uint32_t in89 = *(const uint32_t *)(row + x + 8);
                uint32_t inAB = *(const uint32_t *)(row + x + 10);

                uint16_t s0 = (uint16_t)in01;
                uint16_t s1 = (uint16_t)(in01 >> 16);
                uint16_t s2 = (uint16_t)in23;
                uint16_t s3 = (uint16_t)(in23 >> 16);
                uint16_t s4 = (uint16_t)in45;
                uint16_t s6 = (uint16_t)in67;
                uint16_t s7 = (uint16_t)(in67 >> 16);
                uint16_t s8 = (uint16_t)in89;
                uint16_t s9 = (uint16_t)(in89 >> 16);
                uint16_t s10 = (uint16_t)inAB;

                *dst++ = (uint32_t)px(s0) | ((uint32_t)px(s1) << 16);
                *dst++ = (uint32_t)px(s2) | ((uint32_t)px(s3) << 16);
                *dst++ = (uint32_t)px(s4) | ((uint32_t)px(s6) << 16);
                *dst++ = (uint32_t)px(s7) | ((uint32_t)px(s8) << 16);
                *dst++ = (uint32_t)px(s9) | ((uint32_t)px(s10) << 16);
            }
        }
        return;
    }

    // Fast path: 512x240 title/intro -> 320x240 (2:1 downscale)
    if (w == 512 && h == OUT_H)
    {
        for (int y = 0; y < OUT_H; y++)
        {
            const uint16_t *row = src + (size_t)y * VRAM_STRIDE;
            for (int dx = 0; dx < OUT_W; dx += 2)
            {
                int sx0 = (dx * 512) / OUT_W;
                int sx1 = ((dx + 1) * 512) / OUT_W;
                *dst++ = (uint32_t)px(row[sx0]) | ((uint32_t)px(row[sx1]) << 16);
            }
        }
        return;
    }

    // General fallback for other resolutions (e.g. 384x240)
    if (w != sMapW || h != sMapH)
        buildMaps(w, h);

    for (int y = 0; y < OUT_H; y++)
    {
        const uint16_t *row = src + (size_t)sRowMap[y] * VRAM_STRIDE;
        for (int dx = 0; dx < OUT_W; dx += 2)
            *dst++ = (uint32_t)px(row[sColMap[dx]]) | ((uint32_t)px(row[sColMap[dx + 1]]) << 16);
    }
}

static IRAM_ATTR void copyTask(void *arg)
{
    (void)arg;
    sCopyTask = xTaskGetCurrentTaskHandle();

    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const uint16_t *src = sScanSrc;
        if (!src)
        {
            xSemaphoreGive(sSnapDone);
            continue;
        }

        int next_buf = sActiveBuf ^ 1;
        uint32_t *dst = sPixels[next_buf];

        if (!sotn_lcd_freeze)
            convertFrame(src, sSrcW, sSrcH, dst);

        // Signal game thread that the VRAM snapshot is safely captured in PSRAM buffer
        xSemaphoreGive(sSnapDone);

        sHdr.data = dst;
        sHaveFrame = true;
        sActiveBuf = next_buf;

        rg_display_submit(&sHdr, 0);
    }
}

int lcd_init(void)
{
    sPixels[0] = rg_alloc(OUT_W * OUT_H * 2, MEM_SLOW);
    sPixels[1] = rg_alloc(OUT_W * OUT_H * 2, MEM_SLOW);
    if (!sPixels[0] || !sPixels[1])
        return -1;
    memset(sPixels[0], 0, OUT_W * OUT_H * 2);
    memset(sPixels[1], 0, OUT_W * OUT_H * 2);

    memset(&sHdr, 0, sizeof(sHdr));
    sHdr.width = OUT_W;
    sHdr.height = OUT_H;
    sHdr.stride = OUT_W * 2;
    sHdr.format = FB_PIXEL_FORMAT;
    sHdr.data = sPixels[0];

    sSnapDone = xSemaphoreCreateBinary();
    if (!sSnapDone)
        return -1;
    xSemaphoreGive(sSnapDone);

    sCopyTaskH = rg_task_create("sotn_lcd", copyTask, NULL, 3072, 1, RG_TASK_PRIORITY_6, 1);
    return sCopyTaskH ? 0 : -1;
}

rg_surface_t *lcd_get_surface(void)
{
    return sPixels[sActiveBuf] ? &sHdr : NULL;
}

void lcd_resubmit(void)
{
    if (sPixels[sActiveBuf] && sHaveFrame)
        rg_display_submit(&sHdr, 0);
}

void lcd_present(const uint16_t *src)
{
    if (!sPixels[0] || !sCopyTask)
    {
        sOffered = false;
        return;
    }

    // If previous snapshot is still in progress, wait briefly for it to complete
    if (sOffered)
    {
        xSemaphoreTake(sSnapDone, pdMS_TO_TICKS(50));
    }

    // sampled here, on the game thread, while the finished buffer is current
    int w, h;
    Sotn_DisplaySize(&w, &h);
    sSrcW = w;
    sSrcH = h;

    sScanSrc = src;
    sOffered = true;
    xTaskNotifyGive(sCopyTask);
}

void lcd_wait_snapshot(void)
{
    if (sOffered)
    {
        xSemaphoreTake(sSnapDone, pdMS_TO_TICKS(50));
        xSemaphoreGive(sSnapDone);
    }
}
