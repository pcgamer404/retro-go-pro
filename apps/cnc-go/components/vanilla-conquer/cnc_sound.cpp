/**
 * @file cnc_sound.cpp
 * @brief Sound backend of the ESP32-S3 Command & Conquer port (replaces
 * soundio_openal.cpp).
 *
 * soundio_common.cpp decodes the game sounds and music and queues PCM buffers
 * on "sample trackers" (the OpenAL sources of the PC version). Here, a mixer
 * task on core 0 plays the queued buffers of all the trackers (resampled,
 * with their volume) through I2S to the ES8311 codec.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <rg_system.h>
#include <rg_audio.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "soundio_imp.h"

#include "board_config.h"
#include "cnc_platform.h"

#define TRACKER_BUFFER_COUNT    4           // queued buffers per tracker
#define TRACKER_BUFFER_SIZE     8192        // soundio_common.cpp BUFFER_CHUNK_SIZE
#define FILL_BUFFERS_MAX        2           // free buffers reported at once (as OpenAL)
#define MAX_TRACKERS            8
#define MIX_FRAMES              256         // output frames per mixing pass (11.6 ms)
#define MIXER_TASK_PRIORITY     (configMAX_PRIORITIES - 3)
#define MIXER_TASK_CORE         0
#define MASTER_SHIFT            1           // headroom: the output is attenuated by 6 dB

static const char* TAG = "sound";

struct SampleTrackerTypeImp
{
    uint8_t* Buffers[TRACKER_BUFFER_COUNT];
    int Length[TRACKER_BUFFER_COUNT];
    int Head;               // buffer being played
    int Count;              // queued buffers
    uint32_t Position;      // in the head buffer, in frames, 16.16 fixed point
    uint32_t Step;          // source frames per output frame, 16.16 fixed point
    int Bits;
    bool Stereo;
    int Rate;
    int Gain;               // 0..256
    bool Started;
    bool Used;
};

static SampleTrackerTypeImp trackers[MAX_TRACKERS];
static SemaphoreHandle_t lock;

static volatile bool paused;
static volatile bool shutdownRequested;
static volatile uint32_t underruns;

static void setFormat(SampleTrackerTypeImp* st, int bits, bool stereo, int rate)
{
    st->Bits = bits;
    st->Stereo = stereo;
    st->Rate = rate;
    st->Step = (uint32_t)(((uint64_t)rate << 16) / AUDIO_SAMPLE_RATE);
}

static inline int frameSize(const SampleTrackerTypeImp* st)
{
    return (st->Bits / 8) * (st->Stereo ? 2 : 1);
}

// Mixes one tracker into acc[] (stereo). Called with the lock held.
static void mixTracker(SampleTrackerTypeImp* st, int32_t* acc, int frames)
{
    int fs = frameSize(st);
    for (int i = 0; i < frames && st->Count; i++) {
        uint32_t idx = st->Position >> 16;
        int bufFrames = st->Length[st->Head] / fs;
        if (idx >= (uint32_t)bufFrames) {
            // next buffer
            st->Position -= bufFrames << 16;
            st->Head = (st->Head + 1) % TRACKER_BUFFER_COUNT;
            st->Count--;
            i--;
            continue;
        }
        const uint8_t* p = st->Buffers[st->Head] + idx * fs;
        int l, r;
        if (st->Bits == 16) {
            l = (int16_t)(p[0] | (p[1] << 8));
            r = st->Stereo ? (int16_t)(p[2] | (p[3] << 8)) : l;
        } else {
            l = (p[0] - 128) << 8;
            r = st->Stereo ? (p[1] - 128) << 8 : l;
        }
        acc[2 * i] += l * st->Gain;
        acc[2 * i + 1] += r * st->Gain;
        st->Position += st->Step;
    }
}

static void mixerTask(void* arg)
{
    static int32_t acc[MIX_FRAMES * 2];
    static int16_t out[MIX_FRAMES * 2];
    while (!shutdownRequested) {
        memset(acc, 0, sizeof(acc));
        if (!paused) {
            xSemaphoreTake(lock, portMAX_DELAY);
            for (int t = 0; t < MAX_TRACKERS; t++) {
                SampleTrackerTypeImp* st = &trackers[t];
                if (st->Used && st->Started) {
                    if (st->Count) {
                        mixTracker(st, acc, MIX_FRAMES);
                        if (!st->Count) {
                            underruns++;
                        }
                    }
                }
            }
            xSemaphoreGive(lock);
        }
        if (shutdownRequested) {
            break;
        }
        for (int i = 0; i < MIX_FRAMES * 2; i++) {
            int32_t v = acc[i] >> (8 + MASTER_SHIFT);
            out[i] = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
        }
        rg_audio_submit((const rg_audio_frame_t*)out, MIX_FRAMES);
    }
    vTaskDelete(NULL);
}

extern "C" void soundShutdown(void)
{
    paused = true;
    shutdownRequested = true;
}

bool SoundImp_Init(int bits_per_sample, bool stereo, int rate, bool reverse_channels)
{
    if (lock) {
        return true;
    }
    lock = xSemaphoreCreateMutex();
    rg_audio_set_sample_rate(AUDIO_SAMPLE_RATE);
    xTaskCreatePinnedToCore(mixerTask, "mixer", 3072, nullptr, MIXER_TASK_PRIORITY, nullptr, MIXER_TASK_CORE);
    ESP_LOGI(TAG, "Audio initialized via Retro-Go audio");
    return true;
}

void SoundImp_Shutdown()
{
    paused = true;
}

void SoundImp_PauseSound()
{
    paused = true;
}

bool SoundImp_ResumeSound()
{
    paused = false;
    return lock != nullptr;
}

SampleTrackerTypeImp* SoundImp_Init_Sample(int bits_per_sample, bool stereo, int rate)
{
    for (int t = 0; t < MAX_TRACKERS; t++) {
        SampleTrackerTypeImp* st = &trackers[t];
        if (!st->Used) {
            for (int i = 0; i < TRACKER_BUFFER_COUNT; i++) {
                st->Buffers[i] = (uint8_t*)heap_caps_malloc(TRACKER_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
                if (!st->Buffers[i]) {
                    return nullptr;
                }
            }
            setFormat(st, bits_per_sample, stereo, rate);
            st->Gain = 256;
            st->Head = st->Count = 0;
            st->Position = 0;
            st->Started = false;
            st->Used = true;
            return st;
        }
    }
    return nullptr;
}

void SoundImp_Shutdown_Sample(SampleTrackerTypeImp* st)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    st->Used = false;
    xSemaphoreGive(lock);
    for (int i = 0; i < TRACKER_BUFFER_COUNT; i++) {
        free(st->Buffers[i]);
        st->Buffers[i] = nullptr;
    }
}

void SoundImp_Set_Sample_Attributes(SampleTrackerTypeImp* st, int bits_per_sample, bool stereo, int rate)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    setFormat(st, bits_per_sample, stereo, rate);
    xSemaphoreGive(lock);
}

void SoundImp_Set_Sample_Volume(SampleTrackerTypeImp* st, unsigned int volume)
{
    // volume: 0..255 * 255 (OpenAL gain = volume / 65536)
    st->Gain = volume >> 8;
}

void SoundImp_Buffer_Sample_Data(SampleTrackerTypeImp* st, const void* data, size_t datalen)
{
    if (datalen > TRACKER_BUFFER_SIZE) {
        datalen = TRACKER_BUFFER_SIZE;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (st->Count < TRACKER_BUFFER_COUNT) {
        int slot = (st->Head + st->Count) % TRACKER_BUFFER_COUNT;
        memcpy(st->Buffers[slot], data, datalen);
        st->Length[slot] = datalen;
        if (!st->Count) {
            st->Position = 0;
        }
        st->Count++;
    }
    xSemaphoreGive(lock);
}

int SoundImp_Get_Sample_Free_Buffer_Count(SampleTrackerTypeImp* st)
{
    // soundio_common.cpp fills this many chunks in one go; it assumes at most 2 (the
    // OpenAL backend): a streamed song (only one compressed block queued ahead) ends
    // its chunk short when asked for more, which is taken for the end of the song.
    // Before the start, the queue is filled until no buffer is free: 2 at most there too.
    if (!st->Started) {
        return st->Count < FILL_BUFFERS_MAX ? FILL_BUFFERS_MAX - st->Count : 0;
    }
    int free = TRACKER_BUFFER_COUNT - st->Count;
    return free < FILL_BUFFERS_MAX ? free : FILL_BUFFERS_MAX;
}

// Buffers queued (the movie player keeps two, cnc_vqaaudio.cpp)
int SoundImp_Get_Sample_Queued(SampleTrackerTypeImp* st)
{
    return st->Count;
}

void SoundImp_Start_Sample(SampleTrackerTypeImp* st)
{
    st->Started = true;
}

void SoundImp_Stop_Sample(SampleTrackerTypeImp* st)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    st->Started = false;
    st->Count = 0;
    st->Head = 0;
    st->Position = 0;
    xSemaphoreGive(lock);
}

bool SoundImp_Sample_Status(SampleTrackerTypeImp* st)
{
    // playing while it has data queued (as an OpenAL source stops when its queue runs dry)
    return st->Started && st->Count > 0;
}

extern "C" uint32_t soundGetUnderruns(void)
{
    return underruns;
}
