// spi_lcd.c -- retro-go backend
//
// This replaces CloneKeen's original direct-SPI LCD driver with a thin
// shim over retro-go's rg_display API. The public interface declared in
// spi_lcd.h (spi_lcd_init/send/send_boarder/clear/wait_finish, lcdpal[],
// lcd_bpp) is kept 100% identical, so SDL_video.c and every file in
// components/keen require ZERO changes.
//
// CloneKeen renders an 8bpp paletted 320x240 framebuffer (lcd_bpp == 8)
// and keeps a 256-entry RGB565 palette in lcdpal[]. retro-go's rg_surface_t
// natively supports paletted surfaces (see components/retro-go: any
// FB_PIXEL_FORMAT that is a *_PAL* format has a `->palette[256]` array),
// so we map CloneKeen's framebuffer + palette directly onto that.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spi_lcd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <rg_system.h>

#define KEEN_SCREEN_WIDTH  320
#define KEEN_SCREEN_HEIGHT 240

// Paletted big-endian 565 -- matches how other retro-go ports (e.g. NES)
// build their palette surfaces. If your rg_display.h names this format
// differently, change FB_PIXEL_FORMAT below to match (grep rg_surface.h
// for PAL565 / PAL_565).
#ifndef FB_PIXEL_FORMAT
#define FB_PIXEL_FORMAT RG_PIXEL_PAL565_BE
#endif

int lcd_bpp = 8;
int16_t lcdpal[256];

static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static int screen_boarder = 0;

// -------------------------------------------------------------------
// Public API (unchanged signatures)
// -------------------------------------------------------------------

void spi_lcd_init()
{
    printf("spi_lcd_init() [retro-go backend]\n");

    updates[0] = rg_surface_create(KEEN_SCREEN_WIDTH, KEEN_SCREEN_HEIGHT, FB_PIXEL_FORMAT, MEM_FAST);
    updates[1] = rg_surface_create(KEEN_SCREEN_WIDTH, KEEN_SCREEN_HEIGHT, FB_PIXEL_FORMAT, MEM_FAST);
    if (!updates[0] || !updates[1])
        RG_PANIC("spi_lcd_init: failed to allocate framebuffers");

    memset(updates[0]->data, 0, updates[0]->stride * updates[0]->height);
    memset(updates[1]->data, 0, updates[1]->stride * updates[1]->height);
    currentUpdate = updates[0];
    screen_boarder = 0;
}

static inline void push_palette(rg_surface_t *surface)
{
    // lcdpal[] is filled in by SDL_video.c whenever the game sets a
    // palette. It's cheap (256 * 2 bytes) so we just copy it every frame
    // rather than trying to track dirty state.
    memcpy(surface->palette, lcdpal, sizeof(uint16_t) * 256);
}

void spi_lcd_send(uint16_t *scr)
{
    rg_surface_t *update = updates[currentUpdate == updates[0]];

    push_palette(update);
    // scr is really an 8bpp indexed buffer (see original spi_lcd.c: the
    // memcpy there used lcd_bpp/8 == 1 byte per pixel despite the
    // misleading uint16_t* type). Copy it as raw bytes.
    memcpy(update->data, scr, KEEN_SCREEN_WIDTH * KEEN_SCREEN_HEIGHT);

    currentUpdate = update;
    rg_display_submit(currentUpdate, 0);
}

void spi_lcd_send_boarder(uint16_t *scr, int boarder)
{
    rg_surface_t *update = updates[currentUpdate == updates[0]];

    screen_boarder = boarder;
    push_palette(update);

    size_t visible_bytes = KEEN_SCREEN_WIDTH * (KEEN_SCREEN_HEIGHT - boarder * 2);
    memcpy(update->data, scr, visible_bytes);

    currentUpdate = update;
    rg_display_submit(currentUpdate, 0);
}

void spi_lcd_clear()
{
    rg_surface_t *update = updates[currentUpdate == updates[0]];
    memset(update->data, 0, update->stride * update->height);
    currentUpdate = update;
    rg_display_submit(currentUpdate, 0);
}

void spi_lcd_wait_finish()
{
    // rg_display_submit() already queues/paces frames internally (same
    // as every other retro-go emulator core) -- nothing to do here.
}
