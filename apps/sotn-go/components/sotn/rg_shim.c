// SOTN for Retro-Go - platform shims. Replaces esp_shim.c + esp_input_xiao.inc.c.
//
// Everything psyz's SDL platform files provided that the game core actually
// calls: logging, the 59.94 Hz game clock (VSync + RCnt), the pad, and a few
// video stubs. Differences from the standalone port:
//   - the pad comes from rg_input (retro-go's keymap for your target); MENU /
//     OPTION held ~0.45 s open retro-go's game / options menus
//   - VSync reports a tick to retro-go (rg_system_tick) every frame
//   - the one-off debugging traps (kernel-lock probe, VRAM CRC scan, store
//     watchpoints, USB-serial keyboard) are gone; they were IDF-5.5/XIAO
//     specific and cost frame time

#include <rg_system.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <psyz.h>
#include <psyz/input.h>
#include <psyz/video.h>

// rg_lcd.c
void lcd_present(const unsigned short *src);
void lcd_wait_snapshot(void);
const unsigned short *Soft_DisplayPtr(void); // soft_esp32.c
// rg_audio_out.c
extern volatile int sotn_paused;
void Psyz_RcntAdd(int n); // provided by psyz's libetc.c

// ---- logging (psyz/log.h contract) -----------------------------------------

LOG_LEVEL psyz_logLevel = LOG_LEVEL_I; // DEBUG spam would drown the console

void psyz_log(unsigned int level, const char *file, unsigned int line,
              const char *func, const char *fmt, ...)
{
    static const char lv[] = {'D', 'I', 'W', 'E'};
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)file;
    printf("[%c][%s:%d] %s\n", lv[level & 3], func, line, buf);
}

// ---- frame pacing + RCnt (the game clock) ----------------------------------
//
// The game is fixed-timestep, one logic tick per VSync, and psyz's root
// counter drives the 240 Hz sound sequencer. The audio task accumulates sample
// counts into psyz_pending_rcnt; the RCnt handlers (game code!) must run on the
// game thread, so they are applied here.

#define FRAME_US 16683 // 59.94 Hz NTSC

static int64_t next_frame_us;
static int64_t last_end_us;

int sotn_vsync_count;

void PlatformBackend_Present(void) {}
void PollEvents(void) {}
int InitPlatform(void) { return 1; }
void ResetPlatform(void) {}

int Psyz_VideoVSync(int mode)
{
    (void)mode;
    // scanout first: the conversion on core 1 then overlaps the pacing wait
    lcd_present(Soft_DisplayPtr());

    int64_t now = rg_system_timer();
    // time the game actually spent working this frame (a stage load makes this
    // huge; retro-go only uses it for its busy% statistic)
    int busy_us = last_end_us ? (int)(now - last_end_us) : 0;
    if (next_frame_us == 0)
        next_frame_us = now;
    next_frame_us += FRAME_US;

    int64_t wait = next_frame_us - now;
    if (wait > 0 && wait < 2 * FRAME_US)
    {
        // sleep (letting retro-go's tasks and the idle task run) while far
        // from the deadline, spin only for the last ~2 ms
        while (next_frame_us - rg_system_timer() > 2000)
            vTaskDelay(1);
        int64_t rest = next_frame_us - rg_system_timer();
        if (rest > 0)
            esp_rom_delay_us((uint32_t)rest);
    }
    else if (wait < 0)
    {
        next_frame_us = now; // fell behind: resync deadline so lag does not snowball
    }

    // barrier: the snapshot on core 1 must be complete before the game starts
    // drawing the next frame into VRAM
    lcd_wait_snapshot();

    {
        extern volatile int psyz_pending_rcnt;
        int n = psyz_pending_rcnt;
        if (n > 0)
        {
            psyz_pending_rcnt -= n;
            Psyz_RcntAdd(n);
        }
    }

    last_end_us = rg_system_timer();
    rg_system_tick(busy_us);
    ++sotn_vsync_count;

#ifdef SOTN_TELEMETRY
    if (sotn_vsync_count % 120 == 0)
    {
        extern unsigned sotn_prim_cycles; // soft_esp32.c
        static int64_t last_us;
        int64_t t = rg_system_timer();
        unsigned frame_us = last_us ? (unsigned)((t - last_us) / 120) : 0;
        last_us = t;
        RG_LOGI("frame %d: %u us/f, raster %u us, free int %d KB", sotn_vsync_count, frame_us,
                sotn_prim_cycles / 240 / 120, (int)(rg_system_get_stats().freeMemoryInt / 1024));
        sotn_prim_cycles = 0;
    }
#endif
    return sotn_vsync_count;
}

// ---- pad -------------------------------------------------------------------
//
// psyz bit order: 0 L2, 1 R2, 2 L1, 3 R1, 4 TRIANGLE, 5 CIRCLE, 6 CROSS,
// 7 SQUARE, 8 SELECT, 9 L3, 10 R3, 11 START, 12 UP, 13 RIGHT, 14 DOWN, 15 LEFT
//
//   retro-go    PSX
//   A           CROSS    (jump / confirm)
//   B           SQUARE   (attack)
//   X           CIRCLE
//   Y           TRIANGLE
//   L / R       L1 / R1
//   SELECT+L/R  L2 / R2  (SELECT is swallowed while it is used as a chord)
//   START/SEL   START / SELECT
//   MENU  (hold 0.45 s)  retro-go game menu
//   OPTION (hold 0.45 s) retro-go options menu

static unsigned mapKeys(uint32_t k)
{
    unsigned p = 0;
    if (k & RG_KEY_UP)    p |= 1u << 12;
    if (k & RG_KEY_RIGHT) p |= 1u << 13;
    if (k & RG_KEY_DOWN)  p |= 1u << 14;
    if (k & RG_KEY_LEFT)  p |= 1u << 15;
    if (k & RG_KEY_A)     p |= 1u << 6;
    if (k & RG_KEY_B)     p |= 1u << 7;
    if (k & RG_KEY_X)     p |= 1u << 5;
    if (k & RG_KEY_Y)     p |= 1u << 4;
    if (k & RG_KEY_L)     p |= 1u << 2;
    if (k & RG_KEY_R)     p |= 1u << 3;
    if (k & RG_KEY_START) p |= 1u << 11;
    if (k & RG_KEY_SELECT) p |= 1u << 8;
    if ((k & RG_KEY_SELECT) && (k & (RG_KEY_L | RG_KEY_R)))
    {
        p &= ~((1u << 8) | (1u << 2) | (1u << 3));
        if (k & RG_KEY_L) p |= 1u << 0; // L2
        if (k & RG_KEY_R) p |= 1u << 1; // R2
    }
    return p;
}

#define LONG_PRESS_US 450000

typedef struct
{
    int64_t since;
    bool fired;
} holdstate_t;

// true once, when `down` has been held for LONG_PRESS_US
static bool longPress(holdstate_t *h, bool down, int64_t now)
{
    if (!down)
    {
        h->since = 0;
        h->fired = false;
        return false;
    }
    if (!h->since)
        h->since = now;
    if (!h->fired && now - h->since >= LONG_PRESS_US)
    {
        h->fired = true;
        return true;
    }
    return false;
}

static holdstate_t sMenuHold, sOptionHold;

static void openMenu(bool options)
{
    // Stop the sequencer clock with the game; a retro-go dialog blocks this
    // (game) thread until it is dismissed.
    sotn_paused = 1;
    if (options)
        rg_gui_options_menu();
    else
        rg_gui_game_menu();
    rg_input_wait_for_key(RG_KEY_MENU | RG_KEY_OPTION, false, 1000);
    rg_display_force_redraw(); // -> RG_EVENT_REDRAW -> lcd_resubmit()
    memset(&sMenuHold, 0, sizeof(sMenuHold));
    memset(&sOptionHold, 0, sizeof(sOptionHold));
    next_frame_us = 0; // do not "catch up" the time spent in the menu
    last_end_us = 0;
    sotn_paused = 0;
}

void MyPadInit(int mode)
{
    (void)mode; // rg_input_init() already ran inside rg_system_init()
}

void Psyz_PadsPoll(void)
{
    uint32_t keys = rg_input_read_gamepad();
    int64_t now = rg_system_timer();
    if (longPress(&sMenuHold, keys & RG_KEY_MENU, now))
    {
        openMenu(false);
        keys = rg_input_read_gamepad();
    }
    else if (longPress(&sOptionHold, keys & RG_KEY_OPTION, now))
    {
        openMenu(true);
        keys = rg_input_read_gamepad();
    }
    unsigned pressed = mapKeys(keys);

    char frame[PSYZ_PAD_BUF_LEN];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x00;
    frame[1] = (char)PSYZ_CTRL_DIGITAL_PAD;
    frame[2] = (char)(~(pressed >> 8) & 0xFF);
    frame[3] = (char)(~pressed & 0xFF);
    Psyz_PadsSet(0, frame, sizeof(frame));
    memset(frame, 0xFF, sizeof(frame)); // port 1: disconnected
    Psyz_PadsSet(1, frame, sizeof(frame));
}

// ---- misc ------------------------------------------------------------------

// room foreground layouts are const flash tables; the game mutates tiles
// (red doors, breakables) so each room load serves a PSRAM copy
u16 *Sotn_TilemapWritableCopy(const u16 *src, int count)
{
    static EXT_RAM_BSS_ATTR u16 pool[0x10000]; // 128KB: one room's fg
    if (count < 0)
        count = 0;
    if (count > (int)(sizeof(pool) / sizeof(pool[0])))
    {
        printf("sotn: tilemap copy clamped (%d tiles)\n", count);
        count = sizeof(pool) / sizeof(pool[0]);
    }
    memcpy(pool, src, (size_t)count * 2);
    return pool;
}

int Psyz_VideoSetInternalResolution(unsigned multiplier)
{
    (void)multiplier;
    return 1; // the soft rasterizer renders at native PSX resolution
}

int Psyz_VideoSetVsyncMode(PsyzVsyncMode mode)
{
    (void)mode;
    return 0;
}

void Psyz_SetTitle(const char *title) { (void)title; }
