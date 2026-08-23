#include "SDL_audio.h"
#include "rg_system.h"
#include "rg_audio.h"
#include "rg_utils.h"
#include <string.h>
#include <stdlib.h>

SDL_AudioSpec as;
bool paused = true;
bool locked = false;

static int16_t *mono_buffer = NULL;
static rg_audio_frame_t *stereo_buffer = NULL;
static TaskHandle_t audio_task_handle = NULL;
static bool audio_task_running = false;

IRAM_ATTR void updateTask(void *arg)
{
    audio_task_running = true;
    mono_buffer = rg_alloc(SAMPLECOUNT * sizeof(int16_t), MEM_SLOW);
    stereo_buffer = rg_alloc(SAMPLECOUNT * sizeof(rg_audio_frame_t), MEM_SLOW);

    while (audio_task_running)
    {
        if (!paused && !locked && as.callback)
        {
            memset(mono_buffer, 0, SAMPLECOUNT * sizeof(int16_t));
            // Tyrian's mixer renders directly to a 16-bit mono buffer via
            // this SDL1 callback (see loudness.c / jukebox.c).
            (*as.callback)(as.userdata, (uint8_t *)mono_buffer, SAMPLECOUNT * sizeof(int16_t));

            for (int i = 0; i < SAMPLECOUNT; i++)
            {
                stereo_buffer[i].left = mono_buffer[i];
                stereo_buffer[i].right = mono_buffer[i];
            }

            rg_audio_submit(stereo_buffer, SAMPLECOUNT);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    free(mono_buffer);
    free(stereo_buffer);
    mono_buffer = NULL;
    stereo_buffer = NULL;
    audio_task_handle = NULL;
    vTaskDelete(NULL);
}

void SDL_AudioInit()
{
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    SDL_AudioInit();
    memset(obtained, 0, sizeof(SDL_AudioSpec));
    obtained->freq = SAMPLERATE;
    obtained->format = 16;
    obtained->channels = 1;
    obtained->samples = SAMPLECOUNT;
    obtained->callback = desired->callback;
    obtained->userdata = desired->userdata;
    memcpy(&as, obtained, sizeof(SDL_AudioSpec));

    xTaskCreatePinnedToCore(&updateTask, "tyrianAudio", 4096, NULL, 5, &audio_task_handle, 1);
    printf("Tyrian audio task started (%d Hz, mono -> stereo via rg_audio)\n", SAMPLERATE);
    return 0;
}

void SDL_PauseAudio(int pause_on)
{
    paused = pause_on;
}

void SDL_CloseAudio(void)
{
    if (audio_task_running)
    {
        audio_task_running = false;
        int retry = 500;
        while (audio_task_handle != NULL && retry-- > 0)
            vTaskDelay(pdMS_TO_TICKS(1));
    }
}

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, Uint16 src_format, Uint8 src_channels, int src_rate, Uint16 dst_format, Uint8 dst_channels, int dst_rate)
{
    cvt->len_mult = 1;
    return 0;
}

int SDL_ConvertAudio(SDL_AudioCVT *cvt)
{
    // No differential-DAC conversion needed -- rg_audio takes normal
    // signed 16-bit PCM, so this is now a no-op.
    return 0;
}

void SDL_LockAudio(void)
{
    locked = true;
}

void SDL_UnlockAudio(void)
{
    locked = false;
}
