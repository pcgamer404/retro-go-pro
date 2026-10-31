/* Nano Java platform adapter for Retro-Go / ESP32-S3. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <rg_system.h>
#include <rg_input.h>
#include <rg_display.h>
#include <rg_surface.h>
#include <rg_gui.h>
#include <rg_settings.h>

#include "../../src/pal/pal.h"
#include "../../src/vm/vm.h"

#define OUT_W 320
#define OUT_H 240
#define EVENT_QUEUE_SIZE 32

typedef struct KeyMap {
    uint32_t mask;
    int code;
} KeyMap;

/* Default phone keypad mapping. Change here to suit a particular handheld.
 *
 * The two soft keys follow the Nokia keypad layout a MIDlet expects: the left
 * soft key ("OK"/Select, -6) is on the device's Select button and the right
 * soft key ("Back", -7) on Start, so mid-game menus and "Press OK" prompts
 * work without a numeric keypad. The digits sit on the face buttons: 0 on Y,
 * * on B and # on X. A is FIRE, which is also the centre key (5) for the
 * games that poll game actions instead of raw key codes. */
static const KeyMap s_keymap[] = {
    { RG_KEY_UP,     -1 }, /* Canvas.KEY_UP */
    { RG_KEY_DOWN,   -2 }, /* Canvas.KEY_DOWN */
    { RG_KEY_LEFT,   -3 }, /* Canvas.KEY_LEFT */
    { RG_KEY_RIGHT,  -4 }, /* Canvas.KEY_RIGHT */
    { RG_KEY_A,      -5 }, /* FIRE / centre 5 */
    { RG_KEY_SELECT, -6 }, /* left soft key: OK / Select */
    { RG_KEY_START,  -7 }, /* right soft key: Back */
    { RG_KEY_Y,      '0' },
    { RG_KEY_B,      '*' },
    { RG_KEY_X,      '#' },
};

static void queue_event(int type, int a, int b);

/* Diagonals as number keys. Several Nokia ports only accept 1/3/7/9 (and not
 * a direction) as the "continue" key on a screen, and this handheld has no
 * numeric keypad. Holding a diagonal for s_diag_ms presses the matching digit
 * and holds it until the pair is released; the two direction keys stay
 * pressed as well, so ordinary diagonal movement is unchanged. */
static const struct { uint32_t mask; int code; } s_diagonals[] = {
    { RG_KEY_UP   | RG_KEY_LEFT,  '1' },
    { RG_KEY_UP   | RG_KEY_RIGHT, '3' },
    { RG_KEY_DOWN | RG_KEY_LEFT,  '7' },
    { RG_KEY_DOWN | RG_KEY_RIGHT, '9' },
};
static int     s_diag_ms;        /* 0 = off; per-game "Keypad diagonals" */
static uint32_t s_diag_mask;     /* pair being timed, 0 = none */
static int     s_diag_code;      /* digit currently held, 0 = none */
static int64_t s_diag_since;

void pal_set_diagonal_ms(int ms)
{
    s_diag_ms = ms > 0 ? ms : 0;
    if (!s_diag_ms) {
        if (s_diag_code)
            queue_event(EV_KEY_UP, s_diag_code, 0);
        s_diag_code = 0;
        s_diag_mask = 0;
    }
}

/* Called once per poll with the buttons that are down. */
static void poll_diagonals(uint32_t buttons, int64_t now_ms)
{
    if (s_diag_ms <= 0)
        return;
    uint32_t mask = 0;
    int      code = 0;
    for (size_t i = 0; i < sizeof(s_diagonals) / sizeof(s_diagonals[0]); i++) {
        if ((buttons & s_diagonals[i].mask) == s_diagonals[i].mask) {
            mask = s_diagonals[i].mask;
            code = s_diagonals[i].code;
            break;
        }
    }
    if (mask != s_diag_mask) {
        /* A different pair (or none): drop the old digit and restart the timer. */
        if (s_diag_code) {
            queue_event(EV_KEY_UP, s_diag_code, 0);
            s_diag_code = 0;
        }
        s_diag_mask = mask;
        s_diag_since = now_ms;
    } else if (mask && !s_diag_code && now_ms - s_diag_since >= s_diag_ms) {
        queue_event(EV_KEY_DOWN, code, 0);
        s_diag_code = code;
    }
}

/* Releases the held digit (menu, pause, settings change). */
static void release_diagonal(void)
{
    if (s_diag_code)
        queue_event(EV_KEY_UP, s_diag_code, 0);
    s_diag_code = 0;
    s_diag_mask = 0;
}

static int s_canvas_w = 240;
static int s_canvas_h = 320;
static uint8_t *s_frames[2];
static int s_cur;
static int s_out_w = OUT_W;
static int s_out_h = OUT_H;
static rg_surface_t s_surfaces[2];
static const rg_surface_t s_surface_init = {
    .width = OUT_W,
    .height = OUT_H,
    .stride = OUT_W * 2,
    .offset = 0,
    .format = RG_PIXEL_565_BE,
    .palette = NULL,
    .data = NULL,
    .free_data = false,
    .free_palette = false,
};

static NjEvent s_events[EVENT_QUEUE_SIZE];
static unsigned s_event_head;
static unsigned s_event_tail;
static uint32_t s_previous_buttons;
static bool s_menu_was_down;
static int64_t s_last_present_ms;
static int64_t s_last_dump_ms;
static int64_t s_fps_t0;
/* Struggling-game detector: when a game presents fewer than NJ_SLOW_FPS frames
 * per second for NJ_SLOW_WINDOWS consecutive telemetry windows, its threads are
 * dumped (at most NJ_SLOW_DUMPS times) so a slow or stuck method is visible.
 * NJ_STALL_HINT_DUMPS: after this many "nothing drawn" dumps in a row from a
 * game whose thread died from an uncaught exception, the log says so. The app
 * deliberately does not end the MIDlet itself: a long load after an earlier,
 * harmless exception in a helper thread (audio, a resource parser) looks
 * exactly like a dead game, and quitting it would turn a slow title into a
 * "crash". The user can always leave with the menu. */
#define NJ_SLOW_FPS        5.0
#define NJ_SLOW_WINDOWS    3
#define NJ_SLOW_DUMPS      3
#define NJ_STALL_HINT_DUMPS 2
static int  s_slow_windows;
static int  s_slow_dumps;
static bool s_slow_dump_wanted;
static int  s_stall_dumps;         /* consecutive "nothing drawn" dumps */
static uint32_t s_uncaught_at_frame; /* vm.uncaught_count when the last frame went out */
extern uint32_t nj_stat_sleeps, nj_stat_sleep_ms, nj_stat_yields;
extern uint64_t nj_profile_vm_cyc, nj_profile_native_cyc, nj_profile_gc_cyc;
/* The VM accumulates running time in CPU cycles so that instrumenting every
 * native call and time slice costs a single instruction; cycles are turned
 * back into microseconds only here, once per second. */
#define NJ_CYCLES_PER_US ((double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ)
static int64_t s_idle_us;
static int64_t s_idle_evt_us;
static void present_wait(void);
static SemaphoreHandle_t s_job_sem, s_done_sem;
static bool s_job_outstanding;
static bool s_worker_started;
static uint16_t *s_snap;
static size_t s_snap_bytes;
static int s_job_w, s_job_h;
static uint32_t s_fps_frames;
static bool s_telemetry_enabled = true;
static int64_t s_fps_present_us;
static uint32_t s_fps_convert_us;
/* Source offset of every output column, filled in once per frame by the
 * stretch path of convert_frame() (runs on the display worker only). */
#define NJ_OUT_MAX 1024
static int s_colmap[NJ_OUT_MAX];
/* Display controls are independent of the Java canvas orientation. */
static int s_rotation_mode = -1; /* -1=legacy auto, 0=none, 1=90 CW, 2=180, 3=270 CW */
static bool s_centered_output = true; /* 1:1 pixels, centered; no upscaling */

void pal_set_display_rotation(int mode)
{
    if (mode >= -1 && mode <= 3)
        s_rotation_mode = mode;
}

void pal_set_centered_output(bool enabled)
{
    s_centered_output = enabled;
}

void pal_set_canvas_size(int w, int h)
{
    if (w >= 64 && w <= 640 && h >= 64 && h <= 640) {
        s_canvas_w = w;
        s_canvas_h = h;
    }
}

void *nj_pal_malloc(size_t size)
{
    if (!size) size = 1;
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return p;
}

void *nj_pal_calloc(size_t count, size_t size)
{
    if (count && size > SIZE_MAX / count) return NULL;
    size_t bytes = count * size;
    void *p = nj_pal_malloc(bytes ? bytes : 1);
    if (p) memset(p, 0, bytes ? bytes : 1);
    return p;
}

void *nj_pal_realloc(void *ptr, size_t size)
{
    if (!size) { heap_caps_free(ptr); return NULL; }
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    return p;
}

/* This allocator is used only for the per-thread operand stack and frame array,
 * i.e. the memory the interpreter touches on almost every bytecode, so the
 * reserve below decides whether the VM runs out of internal SRAM (fast) or
 * falls back to PSRAM (much slower on every local variable access). IDF already
 * keeps CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL available for internal-only
 * allocations, so a compact reserve here is enough to protect the system. */
#define NJ_FAST_RESERVE (48u * 1024u)   /* internal RAM always left free for the system */
void *nj_pal_calloc_fast(size_t size)
{
    if (!size) size = 1;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) > size + NJ_FAST_RESERVE) {
        void *p = heap_caps_calloc(1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p) return p;
    }
    return nj_pal_calloc(1, size);
}

void nj_pal_free(void *ptr)
{
    if (ptr) heap_caps_free(ptr);
}

void *pal_heap_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return p;
}

void pal_heap_free(void *ptr)
{
    if (ptr)
        heap_caps_free(ptr);
}

int64_t pal_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Java time: a cached epoch offset plus the monotonic microsecond timer.
 *
 * Games poll currentTimeMillis() from their frame limiter and from animation
 * and physics code - one Asphalt title calls it ~7300 times per frame.  Going
 * through gettimeofday() for every call costs a lock, a critical section and a
 * 64-bit division (~5 us here) and showed up as 36 ms of a 103 ms frame.  The
 * wall clock is therefore read once and advances with esp_timer_get_time().
 *
 * Reading the wall clock at all (instead of time()) is what gives Java
 * millisecond resolution: time() truncates to seconds, and games that integrate
 * a per-frame delta saw "0 ms" for most frames and one 1000 ms step per second,
 * which made camera and parallax scrolling crawl.
 */
static int64_t s_epoch_base;

int64_t pal_epoch_ms(void)
{
    if (!s_epoch_base) {
        struct timeval tv;
        bool wall = gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1000000000;
        s_epoch_base = wall ? (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 - pal_time_ms()
                            : 1704067200000LL;
        RG_LOGI("Java time base: %d s since epoch (%s)", (int)(s_epoch_base / 1000),
                wall ? "wall clock + monotonic ms" : "monotonic fallback");
    }
    return s_epoch_base + pal_time_ms();
}

void pal_console_write(const char *s, int len)
{
    if (!s || len <= 0)
        return;
    /* Avoid format-string interpretation of game-provided text. */
    fwrite(s, 1, (size_t)len, stdout);
    fflush(stdout);
}

static void queue_event(int type, int a, int b)
{
    unsigned next = (s_event_tail + 1) % EVENT_QUEUE_SIZE;
    if (next == s_event_head)
        return;
    s_events[s_event_tail].type = type;
    s_events[s_event_tail].a = a;
    s_events[s_event_tail].b = b;
    s_events[s_event_tail].t_ms = pal_time_ms();
    s_event_tail = next;
}

bool pal_poll_event(NjEvent *ev)
{
    if (!ev)
        return false;

    int64_t now_ms = pal_time_ms();
    if (!s_last_present_ms) s_last_present_ms = now_ms;
    /* A frame that never arrives is a hang; a game that crawls deserves the
     * same dump. Both are rate limited so the log stays readable. */
    bool stalled = now_ms - s_last_present_ms > 5000;
    if ((stalled || s_slow_dump_wanted) && now_ms - s_last_dump_ms > 10000) {
        s_last_dump_ms = now_ms;
        if (s_slow_dump_wanted) {
            s_slow_dump_wanted = false;
            s_slow_dumps++;
        }
        vm_dump_threads(stalled ? "no frame presented for 5 s" : "frame rate below 5 FPS");
        /* A thread that died from an uncaught exception cannot be replaced.
         * Say so once, so a log shows why the screen stopped moving, but let
         * the user decide: see NJ_STALL_HINT_DUMPS. */
        if (stalled && ++s_stall_dumps == NJ_STALL_HINT_DUMPS &&
            vm.uncaught_count != s_uncaught_at_frame) {
            RG_LOGW("nanojava: a Java thread died from an uncaught exception and nothing has "
                    "been drawn since; if the game stays frozen, exit with the menu");
        }
    }

    uint32_t buttons = rg_input_read_gamepad();
    bool menu_down = (buttons & (RG_KEY_MENU | RG_KEY_OPTION)) != 0;
    if (menu_down && !s_menu_was_down) {
        /* Release held Java keys before entering the Retro-Go menu. */
        for (size_t i = 0; i < sizeof(s_keymap) / sizeof(s_keymap[0]); i++)
            if (s_previous_buttons & s_keymap[i].mask)
                queue_event(EV_KEY_UP, s_keymap[i].code, 0);
        release_diagonal();

        /* Both physical menu buttons open the standard game menu. The
         * Emulator Options row inside it opens the registered options handler. */
        present_wait();
        rg_gui_game_menu();

        buttons = rg_input_read_gamepad();
        s_previous_buttons = 0;
        s_fps_t0 = 0;
        s_last_present_ms = pal_time_ms();
    }
    s_menu_was_down = (buttons & (RG_KEY_MENU | RG_KEY_OPTION)) != 0;

    for (size_t i = 0; i < sizeof(s_keymap) / sizeof(s_keymap[0]); i++) {
        bool was = (s_previous_buttons & s_keymap[i].mask) != 0;
        bool now = (buttons & s_keymap[i].mask) != 0;
        if (now != was)
            queue_event(now ? EV_KEY_DOWN : EV_KEY_UP, s_keymap[i].code, 0);
    }
    s_previous_buttons = buttons;
    poll_diagonals(buttons, now_ms);

    if (s_event_head == s_event_tail)
        return false;
    *ev = s_events[s_event_head];
    s_event_head = (s_event_head + 1) % EVENT_QUEUE_SIZE;
    return true;
}

void pal_wait(int64_t until_ms)
{
    /* Sleep with the scheduler for whole ticks and use the microsecond timer
     * only for the sub-tick tail, so a wait is not a yield loop for its whole
     * duration while still keeping sub-tick accuracy. The tick period is
     * derived rather than assumed: with a coarse tick every delay below one
     * tick rounds down to zero ticks, and this then degrades to the old spin
     * (correct, just wasteful), which is what a board config with
     * CONFIG_FREERTOS_HZ=100 produces. */
    const int64_t tick_us = 1000000 / configTICK_RATE_HZ;
    int64_t t0 = esp_timer_get_time();
    if (until_ms < 0) {
        vTaskDelay(1);
        s_idle_evt_us += esp_timer_get_time() - t0;
    } else {
        int64_t target = until_ms * 1000;
        /* Re-poll the scheduler at least every 10 ms so input and the stall
         * detector keep running during long sleeps. */
        if (target > t0 + 10000)
            target = t0 + 10000;
        int64_t remaining = target - t0;
        if (remaining > tick_us) {
            /* Stop one tick short of the deadline and spin the remainder. */
            vTaskDelay((TickType_t)((remaining - tick_us) / tick_us));
        } else if (remaining >= tick_us / 2 && tick_us <= 1000) {
            /* Less than a tick to go: give up the CPU for one tick rather than
             * burning it in a yield loop. The overshoot is bounded by half a
             * tick, and anything shorter than that is spun (below). */
            vTaskDelay(1);
        }
        while (esp_timer_get_time() < target)
            taskYIELD();
    }
    s_idle_us += esp_timer_get_time() - t0;
}

void pal_screen_size(int *w, int *h)
{
    if (w) *w = s_canvas_w;
    if (h) *h = s_canvas_h;
}

/* Nano Java pixels are RGB555 (red low); Retro-Go wants big-endian RGB565. */
static inline uint16_t convert_px(uint16_t c)
{
    const uint16_t g = c & 0x03e0;
    const uint16_t o = (uint16_t)(((c & 0x001f) << 11) | (g << 1) |
                                  ((g >> 4) & 0x0020) | ((c >> 10) & 0x001f));
    return __builtin_bswap16(o);
}

/* Rotation is an output transform only: it does not change the MIDP Canvas size
 * or the orientation requested by the game.
 *
 * This runs once per presented frame, so the per-pixel work is kept to a load,
 * a colour conversion and a store: the rotation is chosen outside the inner
 * loop and each case walks the source with a constant pointer step. The output
 * is only cleared when the image does not cover every output pixel. */
static void convert_frame(const uint16_t *px, int w, int h, uint16_t *dst)
{
    int rot = s_rotation_mode;
    if (rot < 0) rot = (h > w) ? 1 : 0; /* legacy auto: portrait -> clockwise */

    /* Dimensions after rotation. */
    const int rw = (rot == 1 || rot == 3) ? h : w;
    const int rh = (rot == 1 || rot == 3) ? w : h;

    if (s_centered_output) {
        /* 1:1 and centred: pixels map straight across, so the window that the
         * image covers is written exactly once and never needs clearing. */
        const int left = (s_out_w - rw) / 2;
        const int top  = (s_out_h - rh) / 2;

        if (left > 0 || top > 0 || left + rw < s_out_w || top + rh < s_out_h)
            memset(dst, 0, (size_t)s_out_w * (size_t)s_out_h * sizeof(uint16_t));

        const int xs = left > 0 ? left : 0;
        const int ys = top > 0 ? top : 0;
        const int xe = left + rw < s_out_w ? left + rw : s_out_w;
        const int ye = top + rh < s_out_h ? top + rh : s_out_h;
        if (xs >= xe || ys >= ye)
            return;

        /* Source column/row of the first drawn pixel of each row. */
        const int ix0 = xs - left;

        for (int y = ys; y < ye; y++) {
            uint16_t       *out = dst + (size_t)y * s_out_w + xs;
            const int       ry  = y - top;
            const uint16_t *src;
            int             step;
            switch (rot) {
            case 0: /* rotated(x,y) = source(x, y) */
                src = px + (size_t)ry * w + ix0;
                step = 1;
                break;
            case 2: /* rotated(x,y) = source(w-1-x, h-1-y) */
                src = px + (size_t)(h - 1 - ry) * w + (w - 1 - ix0);
                step = -1;
                break;
            case 3: /* rotated(x,y) = source(w-1-y, x) */
                src = px + (size_t)ix0 * w + (w - 1 - ry);
                step = w;
                break;
            default: /* rot == 1: rotated(x,y) = source(y, h-1-x) */
                src = px + (size_t)(h - 1 - ix0) * w + ry;
                step = -w;
                break;
            }
            for (int x = xs; x < xe; x++, src += step)
                *out++ = convert_px(*src);
        }
        return;
    }

    /* Stretch: the output is fully covered, so there is nothing to clear.
     * Rotation is folded into a row base pointer plus a constant per-source-
     * column stride, and the source column of every output column is turned
     * into a byte offset once per frame: the inner loop then only reads the
     * table, instead of running a Bresenham accumulator and a multiply per
     * pixel (Xtensa has no divide instruction, and this ran on every frame of
     * e.g. a 176x208 canvas stretched to 320x240). */
    int xs_step;
    switch (rot) {
    case 0:  xs_step = 1;  break;
    case 2:  xs_step = -1; break;
    case 3:  xs_step = w;  break;
    default: xs_step = -w; break;
    }
    {
        int rx = 0, rx_acc = 0;
        for (int ox = 0; ox < s_out_w; ox++) {
            s_colmap[ox] = rx * xs_step;
            rx_acc += rw;
            while (rx_acc >= s_out_w) { rx_acc -= s_out_w; rx++; }
        }
    }

    int ry = 0, ry_acc = 0;
    for (int oy = 0; oy < s_out_h; oy++) {
        uint16_t       *out = dst + (size_t)oy * s_out_w;
        const uint16_t *base;
        switch (rot) {
        case 0: /* source(rx, ry) */
            base = px + (size_t)ry * w;
            break;
        case 2: /* source(w-1-rx, h-1-ry) */
            base = px + (size_t)(h - 1 - ry) * w + (w - 1);
            break;
        case 3: /* source(w-1-ry, rx) */
            base = px + (size_t)(w - 1 - ry);
            break;
        default: /* rot == 1: source(ry, h-1-rx) */
            base = px + (size_t)(h - 1) * w + ry;
            break;
        }
        const int *cm = s_colmap;
        for (int ox = 0; ox < s_out_w; ox++)
            *out++ = convert_px(base[cm[ox]]);
        ry_acc += rh;
        while (ry_acc >= s_out_h) { ry_acc -= s_out_h; ry++; }
    }
}

static void present_worker(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_job_sem, portMAX_DELAY);
        /* Snapshot the geometry for this job: the game thread is released as
         * soon as the conversion below is done and may already be preparing
         * the next frame with a different canvas size. */
        const int jw = s_out_w, jh = s_out_h;

        size_t bytes = (size_t)jw * (size_t)jh * 2;
        for (int i = 0; i < 2; i++) {
            if (!s_frames[i])
                s_frames[i] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_frames[i])
                s_frames[i] = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        }
        if (s_frames[0] && s_frames[1]) {
            /* Convert first, then release the game thread: it only needs the
             * snapshot until the conversion has read it, so the next frame's
             * game code overlaps this frame's queue wait and SPI transfer.
             * Only the buffer submitted two frames ago is written here, so
             * converting while the panel is still busy is safe. */
            s_cur ^= 1;
            uint32_t c0 = (uint32_t)esp_timer_get_time();
            convert_frame(s_snap, s_job_w, s_job_h, (uint16_t *)s_frames[s_cur]);
            s_fps_convert_us += (uint32_t)esp_timer_get_time() - c0;
            xSemaphoreGive(s_done_sem);

            while (rg_display_is_busy())
                vTaskDelay(1);

            rg_surface_t *surf = &s_surfaces[s_cur];
            *surf = s_surface_init;
            surf->width = jw;
            surf->height = jh;
            surf->stride = jw * 2;
            surf->format = RG_PIXEL_565_BE;
            surf->data = s_frames[s_cur];
            rg_display_submit(surf, 0);
        } else {
            xSemaphoreGive(s_done_sem);
        }
    }
}

/* Blocks until the worker has finished the last frame (cheap in practice). */
static void present_wait(void)
{
    if (s_job_outstanding) {
        xSemaphoreTake(s_done_sem, portMAX_DELAY);
        s_job_outstanding = false;
    }
}

void pal_set_telemetry_enabled(bool enabled)
{
    s_telemetry_enabled = enabled;
}

void pal_present(const uint16_t *px, int w, int h)
{
    if (!px || w <= 0 || h <= 0)
        return;

    int64_t t_start = esp_timer_get_time();
    s_last_present_ms = pal_time_ms();

    if (!s_worker_started) {
        s_job_sem = xSemaphoreCreateBinary();
        s_done_sem = xSemaphoreCreateBinary();
        /* Run on the core the VM is not using. */
        if (!s_job_sem || !s_done_sem ||
            xTaskCreatePinnedToCore(present_worker, "nj_present", 4096, NULL, 5, NULL,
                                    1 - xPortGetCoreID()) != pdPASS)
            pal_fatal("Cannot start the Java display worker.");
        s_worker_started = true;
    }

    present_wait();

    s_out_w = rg_display_get_width();
    s_out_h = rg_display_get_height();
    if (s_out_w <= 0) s_out_w = OUT_W;
    if (s_out_h <= 0) s_out_h = OUT_H;
    if (s_out_w > 1024 || s_out_h > 1024)
        return;

    size_t need = (size_t)w * (size_t)h * 2;
    if (need > s_snap_bytes) {
        heap_caps_free(s_snap);
        s_snap = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_snap) {
            s_snap_bytes = 0;
            pal_fatal("Not enough memory for the Java display snapshot.");
        }
        s_snap_bytes = need;
    }
    memcpy(s_snap, px, need);
    s_job_w = w;
    s_job_h = h;
    s_job_outstanding = true;
    xSemaphoreGive(s_job_sem);

    int64_t now = esp_timer_get_time();
    s_fps_present_us += now - t_start;
    s_fps_frames++;
    /* A frame arrived: the game is alive, whatever happened before it. */
    s_stall_dumps = 0;
    s_uncaught_at_frame = vm.uncaught_count;
    if (!s_fps_t0) s_fps_t0 = now;
    if (now - s_fps_t0 >= 1000000) {
        double fps = s_fps_frames * 1e6 / (double)(now - s_fps_t0);
        double vm_ms = nj_profile_vm_cyc / NJ_CYCLES_PER_US / 1000.0 / s_fps_frames;
        double native_ms = nj_profile_native_cyc / NJ_CYCLES_PER_US / 1000.0 / s_fps_frames;
        double bytecode_est_ms = vm_ms > native_ms ? vm_ms - native_ms : 0.0;
        if (s_telemetry_enabled) {
            RG_LOGI("FPS %.1f | present %.1f + convert %.1f ms/frame | VM %.1f | native %.1f | bytecode-est %.1f | GC %.2f ms/frame (subset) | idle %.1f ms/frame (event-wait %.1f) | frame %.1f ms | sleeps %.1f/frame, requested %.1f ms/frame (avg %u ms/call), yields %.1f/frame",
                    fps,
                    s_fps_present_us / 1000.0 / s_fps_frames,
                    s_fps_convert_us / 1000.0 / s_fps_frames,
                    vm_ms,
                    native_ms,
                    bytecode_est_ms,
                    nj_profile_gc_cyc / NJ_CYCLES_PER_US / 1000.0 / s_fps_frames,
                    s_idle_us / 1000.0 / s_fps_frames,
                    s_idle_evt_us / 1000.0 / s_fps_frames,
                    (now - s_fps_t0) / 1000.0 / s_fps_frames,
                    (double)nj_stat_sleeps / s_fps_frames,
                    (double)nj_stat_sleep_ms / s_fps_frames,
                    nj_stat_sleeps ? (unsigned)(nj_stat_sleep_ms / nj_stat_sleeps) : 0u,
                    (double)nj_stat_yields / s_fps_frames);
            if (nj_stat_input_events || nj_stat_input_dropped) {
                double in_avg = nj_stat_input_events
                                    ? (double)nj_stat_input_ms / nj_stat_input_events : 0.0;
                RG_LOGI("input: %u events/frame, waited %.1f ms avg, %.0f ms max, %u dropped (button to the game's event loop)",
                        (unsigned)nj_stat_input_events, in_avg,
                        (double)nj_stat_input_max_ms,
                        (unsigned)nj_stat_input_dropped);
            }
            /* Where the "native" milliseconds above went: the three native
             * methods with the most CPU time in this window. Games draw a
             * frame with a few thousand native calls, so knowing which of
             * them is expensive is the difference between guessing and
             * optimising the right one. */
            char top[192];
            top[0] = '\0';
            for (int n = 0; n < 3; n++) {
                int best = -1;
                for (int i = 0; i < NJ_NATIVE_SLOTS; i++) {
                    if (!nj_native_stats[i].fn)
                        continue;
                    if (best < 0 || nj_native_stats[i].cyc > nj_native_stats[best].cyc)
                        best = i;
                }
                if (best < 0 || !nj_native_stats[best].cyc)
                    break;
                int len = (int)strlen(top);
                snprintf(top + len, sizeof(top) - (size_t)len, "%s%s.%s %.1f",
                         len ? ", " : "", nj_native_stats[best].cls,
                         nj_native_stats[best].name,
                         nj_native_stats[best].cyc / NJ_CYCLES_PER_US / 1000.0 / s_fps_frames);
                nj_native_stats[best].cyc = 0;   /* do not pick it again */
            }
            if (top[0])
                RG_LOGI("native %.1f calls/frame, top: %s ms/frame",
                        (double)nj_stat_native_calls / s_fps_frames, top);

            /* Which Java methods the interpreted time belongs to. This is the
             * "bytecode-est" part of the line split by method, so it says
             * whether the game's own code or the class library is the reason a
             * title is slow - and therefore whether a VM-level change can help
             * at all. */
            char jtop[192];
            jtop[0] = '\0';
            for (int n = 0; n < 3; n++) {
                int best = -1;
                for (int i = 0; i < NJ_JAVA_SLOTS; i++) {
                    if (!nj_java_stats[i].m)
                        continue;
                    if (best < 0 || nj_java_stats[i].cyc > nj_java_stats[best].cyc)
                        best = i;
                }
                if (best < 0 || !nj_java_stats[best].cyc)
                    break;
                int len = (int)strlen(jtop);
                snprintf(jtop + len, sizeof(jtop) - (size_t)len, "%s%s.%s %.1f",
                         len ? ", " : "", nj_java_stats[best].cls,
                         nj_java_stats[best].name,
                         nj_java_stats[best].cyc / NJ_CYCLES_PER_US / 1000.0 / s_fps_frames);
                nj_java_stats[best].cyc = 0;   /* do not pick it again */
            }
            if (jtop[0])
                RG_LOGI("java %.0f slices/frame, top: %s ms/frame",
                        (double)nj_java_slices / s_fps_frames, jtop);
        }
        memset(nj_native_stats, 0, sizeof nj_native_stats);
        memset(nj_java_stats, 0, sizeof nj_java_stats);
        nj_stat_native_calls = 0;
        nj_java_slices = 0;
        /* A crawl is as good as a hang for the player: ask for a thread dump
         * the next time the scheduler polls (pal_poll_event). */
        s_slow_windows = fps < NJ_SLOW_FPS ? s_slow_windows + 1 : 0;
        if (s_slow_windows >= NJ_SLOW_WINDOWS && s_slow_dumps < NJ_SLOW_DUMPS)
            s_slow_dump_wanted = true;
        nj_stat_sleeps = nj_stat_sleep_ms = nj_stat_yields = 0;
        nj_stat_input_events = nj_stat_input_ms = nj_stat_input_max_ms = 0;
        nj_stat_input_dropped = 0;
        nj_profile_vm_cyc = nj_profile_native_cyc = nj_profile_gc_cyc = 0; s_idle_evt_us = 0;
        s_fps_t0 = now; s_fps_frames = 0; s_fps_present_us = 0; s_idle_us = 0;
        s_fps_convert_us = 0;
    }
}

void pal_soft_labels(const char *left, const char *right)
{
    (void)left;
    (void)right;
}

bool pal_audio_available(void)
{
    /* Audio is deliberately disabled in the first porting milestone. */
    return false;
}

void pal_audio_song(int slot, const NjaEvent *ev, uint32_t n, uint32_t start_ms, int volume)
{
    (void)slot; (void)ev; (void)n; (void)start_ms; (void)volume;
}

void pal_audio_pcm(int slot, const int16_t *pcm, uint32_t samples, uint32_t rate,
                   uint32_t start, int volume)
{
    (void)slot; (void)pcm; (void)samples; (void)rate; (void)start; (void)volume;
}

void pal_audio_stop(int slot) { (void)slot; }
void pal_audio_volume(int slot, int volume) { (void)slot; (void)volume; }

const char *pal_data_dir(void)
{
    (void)mkdir("/sd/retro-go", 0775);
    (void)mkdir("/sd/retro-go/nanojava", 0775);
    return "/sd/retro-go/nanojava";
}

void pal_fatal(const char *msg)
{
    RG_LOGE("Nano Java fatal error: %s", msg ? msg : "unknown error");
    rg_gui_alert("Nano Java error", msg ? msg : "unknown error");
    rg_system_exit();
}
