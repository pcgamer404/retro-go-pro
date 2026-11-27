// SOTN for Retro-Go - audio backend.
//
// The psyz SPU mixer produces 44100 Hz stereo s16 and Psyz_SpuPullSamples also
// advances the game's sequencer clock, so the pull must follow WALL-CLOCK time:
// a fixed "441 frames per tick" falls behind whenever the task is delayed and
// the music runs slow. Here the number of frames owed is computed from the
// real elapsed time, and the PCM is resampled (linear) to whatever rate the
// retro-go sink runs at (a USB DAC at 48 kHz would otherwise play too fast).
//
// Pulling keeps running while retro-go has audio muted; it stops only while the
// game itself is stopped (sotn_paused).

#include <rg_system.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <psyz/spu.h>

#define SRC_RATE 44100
#define CHUNK 441              // 10 ms
#define OUT_MAX 520            // up to ~51 kHz sinks; above that: no resampling

static rg_audio_frame_t pcm[CHUNK + 1]; // [0] holds the previous chunk's last frame
static rg_audio_frame_t out[OUT_MAX];
static SemaphoreHandle_t sAudioMutex;
static uint32_t sPos;                    // 16.16 read position inside pcm[]

volatile int sotn_paused;

void Psyz_AudioLock(void)
{
    if (sAudioMutex)
        xSemaphoreTakeRecursive(sAudioMutex, portMAX_DELAY);
}

void Psyz_AudioUnlock(void)
{
    if (sAudioMutex)
        xSemaphoreGiveRecursive(sAudioMutex);
}

static void submitChunk(void)
{
    if (rg_audio_get_mute())
        return;

    int rate = rg_audio_get_sample_rate();
    if (rate == SRC_RATE || rate <= 0 || rate > 51000)
    {
        rg_audio_submit(&pcm[1], CHUNK);
        return;
    }

    // pcm[0] = previous frame, pcm[1..CHUNK] = new; interpolate between neighbours
    uint32_t step = (uint32_t)(((uint64_t)SRC_RATE << 16) / rate);
    int n = 0;
    while ((sPos >> 16) < CHUNK && n < OUT_MAX)
    {
        uint32_t i = sPos >> 16, f = sPos & 0xFFFF;
        const rg_audio_frame_t *a = &pcm[i], *b = &pcm[i + 1];
        out[n].left = (int16_t)(a->left + (((int32_t)b->left - a->left) * (int32_t)f >> 16));
        out[n].right = (int16_t)(a->right + (((int32_t)b->right - a->right) * (int32_t)f >> 16));
        n++;
        sPos += step;
    }
    sPos -= (uint32_t)CHUNK << 16;
    if (n)
        rg_audio_submit(out, n);
}

static void audioTask(void *arg)
{
    (void)arg;
    int64_t t0 = rg_system_timer();
    int64_t produced = 0; // frames pulled since t0

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(5));

        int64_t now = rg_system_timer();
        if (sotn_paused)
        {
            t0 = now;
            produced = 0;
            continue;
        }

        int64_t owed = (now - t0) * SRC_RATE / 1000000 - produced;
        if (owed > SRC_RATE / 5) // >200 ms behind: drop the backlog, stay in sync
        {
            t0 = now;
            produced = 0;
            owed = 0;
        }

        for (int chunks = 0; owed >= CHUNK && chunks < 4; chunks++)
        {
            Psyz_AudioLock();
            Psyz_SpuPullSamples((short *)&pcm[1], CHUNK); // also ticks RCnt
            Psyz_AudioUnlock();
            submitChunk();
            pcm[0] = pcm[CHUNK]; // keep the last frame for interpolation
            produced += CHUNK;
            owed -= CHUNK;
        }
    }
}

int Psyz_AudioInit(void)
{
    if (sAudioMutex)
        return 0;

    Psyz_SpuInit();
    sAudioMutex = xSemaphoreCreateRecursiveMutex();
    if (!sAudioMutex)
        return -1;

    rg_task_create("sotn_spu", audioTask, NULL, 4096, 1, RG_TASK_PRIORITY_6, 1);
    return 0;
}
