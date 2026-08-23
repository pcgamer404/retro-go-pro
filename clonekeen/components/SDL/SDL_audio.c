// SDL_audio.c -- retro-go backend
//
// Original implementation drove the ESP32 built-in 8-bit DAC via I2S
// directly (audioToOdroidGoFormat + i2s_write). This version instead
// converts keen's 8-bit unsigned mono callback output to standard 16-bit
// signed stereo PCM and hands it to retro-go's rg_audio_submit(), which
// already owns the audio backend (I2S / DAC / whatever the board uses).
//
// SDL_BuildAudioCVT / SDL_ConvertStereo / SDL_ConvertAudio / SDL_Lock-
// /UnlockAudio are pure software and hardware-agnostic, so they are
// unchanged from the original file.

#include "SDL_audio.h"
#include <rg_system.h>

SDL_AudioSpec as;
unsigned char *sdl_buffer;
int16_t *stereo_buffer;
void *user_data;
bool paused = true;
bool locked = false;
SemaphoreHandle_t xSemaphoreAudio = NULL;

char global_volume;
char volumeLevel[] = {0, 15, 45, 75, 100};

static rg_task_t *audio_task_handle = NULL;

// Convert keen's 8-bit unsigned mono PCM (centered at 128, same layout
// the original audioToOdroidGoFormat assumed) into 16-bit signed stereo,
// which is what rg_audio_submit() expects (see other retro-go emulator
// cores' rg_audio_submit calls).
IRAM_ATTR static void convertToStereoPCM(const unsigned char *buf, int16_t *out, int samples)
{
    for (int i = 0; i < samples; i++)
    {
        int16_t sample = (int16_t)(((int)buf[i] - 128) * global_volume / 100) << 8;
        out[i * 2 + 0] = sample; // left
        out[i * 2 + 1] = sample; // right
    }
}

IRAM_ATTR static void audioTask(void *arg)
{
    rg_task_msg_t msg;
    while (rg_task_receive(&msg, -1))
    {
        if (msg.type == RG_TASK_MSG_STOP)
            break;

        if (paused)
        {
            rg_task_delay(5);
            continue;
        }

        memset(sdl_buffer, 0, SAMPLECOUNT * SAMPLESIZE);

        SDL_LockAudio();
        (*as.callback)(NULL, sdl_buffer, SAMPLECOUNT * SAMPLESIZE);
        SDL_UnlockAudio();

        convertToStereoPCM(sdl_buffer, stereo_buffer, SAMPLECOUNT);
        rg_audio_submit((rg_audio_sample_t *)stereo_buffer, SAMPLECOUNT);
    }
}

void SDL_AudioInit(void)
{
    global_volume = volumeLevel[1];
    sdl_buffer = heap_caps_malloc(SAMPLECOUNT * SAMPLESIZE, MALLOC_CAP_8BIT);
    stereo_buffer = heap_caps_malloc(SAMPLECOUNT * sizeof(int16_t) * 2, MALLOC_CAP_8BIT);
    // rg_audio is already initialized as part of rg_system_init() -- no
    // I2S/DAC driver setup needed here.
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    SDL_AudioInit();
    if (obtained == NULL)
        obtained = malloc(sizeof(SDL_AudioSpec));
    memset(obtained, 0, sizeof(SDL_AudioSpec));
    obtained->freq = SAMPLERATE;
    obtained->format = desired->format;
    obtained->channels = desired->channels;
    obtained->samples = SAMPLECOUNT * SAMPLESIZE;
    obtained->callback = desired->callback;
    obtained->size = SAMPLECOUNT * SAMPLESIZE;
    memcpy(&as, obtained, sizeof(SDL_AudioSpec));

    paused = false;
    audio_task_handle = rg_task_create("keen_audio", &audioTask, NULL, 4096, 1, RG_TASK_PRIORITY_5, 1);
    printf("audio task started (retro-go backend)\n");
    return 0;
}

void SDL_PauseAudio(int pause_on)
{
    paused = pause_on;
}

void SDL_CloseAudio(void)
{
    if (audio_task_handle)
    {
        rg_task_msg_t msg = {.type = RG_TASK_MSG_STOP};
        rg_task_send(audio_task_handle, &msg, -1);
        audio_task_handle = NULL;
    }
    free(sdl_buffer);
    free(stereo_buffer);
}

/* Duplicate a mono channel to both stereo channels */
IRAM_ATTR void SDLCALL SDL_ConvertStereo(SDL_AudioCVT *cvt, Uint16 format)
{
	int i;
	if ( (format & 0xFF) == 16 ) {
		Uint16 *src, *dst;

		src = (Uint16 *)(cvt->buf+cvt->len_cvt);
		dst = (Uint16 *)(cvt->buf+cvt->len_cvt*2);
		for ( i=cvt->len_cvt/2; i; --i ) {
			dst -= 2;
			src -= 1;
			dst[0] = src[0];
			dst[1] = src[0];
		}
	} else {
		Uint8 *src, *dst;

		src = cvt->buf+cvt->len_cvt;
		dst = cvt->buf+cvt->len_cvt*2;
		for ( i=cvt->len_cvt; i; --i ) {
			dst -= 2;
			src -= 1;
			dst[0] = src[0];
			dst[1] = src[0];
		}
	}
	cvt->len_cvt *= 2;
	if ( cvt->filters[++cvt->filter_index] ) {
		cvt->filters[cvt->filter_index](cvt, format);
	}
}

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, Uint16 src_format, Uint8 src_channels, int src_rate, Uint16 dst_format, Uint8 dst_channels, int dst_rate)
{
	cvt->len_mult = 1;
	cvt->len = SAMPLECOUNT*SAMPLESIZE*2;

	cvt->needed = 0;
	cvt->filter_index = 0;
	cvt->filters[0] = NULL;
	cvt->len_ratio = 1.0;

	/* Last filter:  Mono/Stereo conversion */
	if ( src_channels != dst_channels ) {
		if ( (src_channels == 1) && (dst_channels > 1) ) {
			cvt->filters[cvt->filter_index++] =
					SDL_ConvertStereo;
			cvt->len_mult *= 2;
			src_channels = 2;
			cvt->len_ratio *= 2;
		}
		while ( (src_channels*2) <= dst_channels ) {
			cvt->filters[cvt->filter_index++] =
					SDL_ConvertStereo;
			cvt->len_mult *= 2;
			src_channels *= 2;
			cvt->len_ratio *= 2;
		}
		if ( src_channels != dst_channels ) {
			/* Uh oh.. */;
		}
	}

	/* Set up the filter information */
	if ( cvt->filter_index != 0 ) {
		cvt->needed = 1;
		cvt->src_format = src_format;
		cvt->dst_format = dst_format;
		cvt->len = 0;
		cvt->buf = NULL;
		cvt->filters[cvt->filter_index] = NULL;
	}
	return(cvt->needed);
}

IRAM_ATTR int SDL_ConvertAudio(SDL_AudioCVT *cvt)
{
	return 0;
}

void SDL_LockAudio(void)
{
    if (xSemaphoreAudio == NULL)
    {
        printf("Creating audio mutex.\n");
        xSemaphoreAudio = xSemaphoreCreateMutex();
        if (!xSemaphoreAudio)
            abort();
    }

    if (!xSemaphoreTake(xSemaphoreAudio, 5000 / portTICK_PERIOD_MS))
    {
        printf("Timeout waiting for audio lock.\n");
        abort();
    }
}

void SDL_UnlockAudio(void)
{
    if (!xSemaphoreAudio)
        abort();
    if (!xSemaphoreGive(xSemaphoreAudio))
        abort();
}
