#include "oj_sdl_audio_compat.h"
#include <stdlib.h>
#include <string.h>
#include "rg_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define AUDIO_TASK_STACK (12 * 1024)

static SDL_AudioSpec s_spec;
static SemaphoreHandle_t s_lock;
static Uint8 *s_buf;
static volatile int s_open, s_paused = 1, s_suspended;

extern const char *SDL_GetError(void);

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_open) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

        const int frames = s_spec.samples;
        const int64_t period_us = 1000000LL * frames / s_spec.freq;
        const int64_t t0 = rg_system_timer();

        if (s_paused || s_suspended || !s_spec.callback) {
            memset(s_buf, 0, s_spec.size);
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_spec.callback(s_spec.userdata, s_buf, (int)s_spec.size);
            xSemaphoreGive(s_lock);
        }

        /* Blocks until the I2S driver has room, which paces the loop. */
        rg_audio_submit((const rg_audio_frame_t *)s_buf, frames);

        /* If there is no audio driver the call returns at once: pace ourselves. */
        const int64_t spent = rg_system_timer() - t0;
        if (spent < period_us / 2) {
            TickType_t ticks = pdMS_TO_TICKS((period_us - spent) / 1000);
            vTaskDelay(ticks ? ticks : 1);
        }
    }
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    if (s_open || !desired || !desired->callback) return -1;

    s_spec = *desired;
    s_spec.format = AUDIO_S16SYS;                 /* the only format we produce */
    s_spec.channels = 2;
    s_spec.silence = 0;
    if (!s_spec.samples) s_spec.samples = 1024;
    s_spec.size = (Uint32)s_spec.samples * 2 * sizeof(Sint16);

    /* The sample rate is fixed by rg_system_init() (config.sampleRate). */
    int rate = rg_audio_get_sample_rate();
    if (rate > 0) s_spec.freq = rate;

    s_buf = (Uint8 *)calloc(1, s_spec.size);
    s_lock = xSemaphoreCreateMutex();
    if (!s_buf || !s_lock) return -1;

    s_paused = 1;
    s_open = 1;
    rg_task_create("oj_audio", audio_task, NULL, AUDIO_TASK_STACK, 1, RG_TASK_PRIORITY_2, -1);

    if (obtained) *obtained = s_spec;
    return 0;
}

void SDL_PauseAudio(int pause_on) { s_paused = pause_on; }
void SDL_CloseAudio(void) { s_paused = 1; }   /* the task keeps running and sends silence */
void SDL_LockAudio(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void SDL_UnlockAudio(void) { if (s_lock) xSemaphoreGive(s_lock); }
void oj_sdl_audio_suspend(int on) { s_suspended = on; }

/* Mix signed 16-bit little-endian samples with saturation. volume is 0..128. */
void SDL_MixAudio(Uint8 *dst, const Uint8 *src, Uint32 len, int volume)
{
    Sint16 *d = (Sint16 *)dst;
    const Sint16 *s = (const Sint16 *)src;
    if (volume > SDL_MIX_MAXVOLUME) volume = SDL_MIX_MAXVOLUME;
    for (Uint32 i = 0; i < len / 2; i++) {
        int v = d[i] + ((s[i] * volume) >> 7);
        d[i] = (Sint16)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
}
