/* See src/mamego_prof.h */
#ifdef NEOPROF
#include <stdio.h>
#include <stdint.h>
#include "mamego_prof.h"
#ifdef ESP_PLATFORM
#include <esp_timer.h>
static int64_t now_us(void) { return esp_timer_get_time(); }
#else
#include <time.h>
static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }
#endif

static int stack[8], depth;
/* core 1 (NEOPROF): us spent by its tasks, added by them (main.c present task,
   fm.c YM2610 task); printed with the core 0 split */
volatile int64_t mamego_core1_us[3]; /* 0 sprite render, 1 convert + display, 2 YM2610 */
volatile int mamego_render_count[3];  /* Neo Geo sprites: 0 deferred, 1 drawn on core 1, 2 drawn on core 0 as fallback */
int64_t mamego_prof_now(void) { return now_us(); }
/* host hooks (mame-go main.c): core 1 idle time and display task busy time, in us since start */
__attribute__((weak)) int64_t mamego_core1_idle_us(void) { return 0; }
__attribute__((weak)) int64_t mamego_display_busy_us(void) { return 0; }
static int64_t last, total[PROF_COUNT], frame_start;
static int frames;

static void account(void)
{
	int64_t t = now_us();
	if (last)
		total[depth ? stack[depth - 1] : PROF_OTHER] += t - last;
	last = t;
}

void mamego_prof_push(int part)
{
	account();
	if (depth < 8)
		stack[depth++] = part;
}

void mamego_prof_pop(void)
{
	account();
	if (depth)
		depth--;
}

void mamego_prof_frame(void)
{
	static const char *names[PROF_COUNT] = {"other", "68000", "z80", "ym2610", "video", "mixer", "blit", "out"};
	int i;
	account();
	if (++frames < 60)
		return;
	{
		int64_t t = now_us(), wall = frame_start ? t - frame_start : 0, busy0 = 0, busy1;
		printf("NEOPROF ms/frame:");
		for (i = 0; i < PROF_COUNT; i++)
		{
			printf(" %s %.2f", names[i], total[i] / 1000.0 / frames);
			if (i != PROF_BLIT)
				busy0 += total[i];
			total[i] = 0;
		}
		busy1 = mamego_core1_us[0] + mamego_core1_us[1] + mamego_core1_us[2];
		printf("\nNEOPROF core1 ms/frame: sprites %.2f convert+display %.2f ym2610 %.2f",
			mamego_core1_us[0] / 1000.0 / frames, mamego_core1_us[1] / 1000.0 / frames, mamego_core1_us[2] / 1000.0 / frames);
		{
			static int64_t idle_prev, disp_prev;
			int64_t idle = mamego_core1_idle_us(), disp = mamego_display_busy_us();
			printf(" display %.2f", (disp - disp_prev) / 1000.0 / frames);
			if (wall)
				printf(" | busy core0 %d%% (without blit/wait) core1 %d%% (idle-measured)",
					(int)(busy0 * 100 / wall), (int)(100 - (idle - idle_prev) * 100 / wall));
			idle_prev = idle;
			disp_prev = disp;
		}
		printf(" | sprites deferred %d on core1 %d fallback %d\n", mamego_render_count[0], mamego_render_count[1], mamego_render_count[2]);
		mamego_render_count[0] = mamego_render_count[1] = mamego_render_count[2] = 0;
		mamego_core1_us[0] = mamego_core1_us[1] = mamego_core1_us[2] = 0;
		frame_start = t;
	}
	frames = 0;
}
#endif
