/* Silent audio entry points for builds without the game audio engine. */
#include <ultra64.h>
#include "audio/external.h"
#include "audio_rg.h"

#if !PORT_ENABLE_AUDIO

/* Engine entry points that the unused N64 thread helpers still reference. */
void audio_init(void)
{
}

void func_800C5CB8(void)
{
}

struct SPTask *create_next_audio_frame_task(void)
{
    return NULL;
}

/* Retro-Go audio output hooks used by the platform AI shims. */
void mk64_rg_audio_set_enabled(bool enabled)
{
    (void)enabled;
}

bool mk64_rg_audio_requested(void)
{
    return false;
}

void mk64_rg_audio_ensure_init(void)
{
}

void mk64_rg_audio_shutdown(void)
{
}

void mk64_rg_audio_pause(bool paused)
{
    (void)paused;
}

void port_audio_out_set_rate(uint32_t rate)
{
    (void)rate;
}

void port_audio_out_push(const int16_t *samples, uint32_t bytes)
{
    (void)samples;
    (void)bytes;
}

uint32_t port_audio_out_queued_bytes(void)
{
    return 0;
}
void begin_losing_ceremony_sequence(void)
{
}

void func_800C3448(u32 a0)
{
    (void)a0;
}

void func_800C8F44(u8 a0)
{
    (void)a0;
}

void func_800C8F80(u8 a0, u32 a1)
{
    (void)a0;
    (void)a1;
}

void func_800C9018(u8 a0, u32 a1)
{
    (void)a0;
    (void)a1;
}

void func_800C9060(u8 a0, u32 a1)
{
    (void)a0;
    (void)a1;
}

void func_800C90F4(u8 a0, u32 a1)
{
    (void)a0;
    (void)a1;
}

void func_800C9250(u8 a0)
{
    (void)a0;
}

void func_800C92CC(u8 a0, u32 a1)
{
    (void)a0;
    (void)a1;
}

void func_800C94A4(u8 a0)
{
    (void)a0;
}

void func_800C97C4(u8 a0)
{
    (void)a0;
}

void func_800C98B8(Vec3f a0, Vec3f a1, u32 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
}

void func_800C99E0(Vec3f a0, s32 a1)
{
    (void)a0;
    (void)a1;
}

void func_800C9A88(u8 a0)
{
    (void)a0;
}

void func_800C9D0C(u8 a0)
{
    (void)a0;
}

void func_800C9D80(Vec3f a0, Vec3f a1, u32 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
}

void func_800C9EF4(Vec3f a0, u32 a1)
{
    (void)a0;
    (void)a1;
}

void func_800C9F90(u8 a0)
{
    (void)a0;
}

void func_800CA008(u8 a0, u8 a1)
{
    (void)a0;
    (void)a1;
}

void func_800CA0A0(void)
{
}

void func_800CA0B8(void)
{
}

void func_800CA0CC(void)
{
}

void func_800CA118(u8 a0)
{
    (void)a0;
}

void func_800CA24C(u8 a0)
{
    (void)a0;
}

void func_800CA270(void)
{
}

void func_800CA288(u8 a0, s8 a1)
{
    (void)a0;
    (void)a1;
}

void func_800CA2B8(u8 a0)
{
    (void)a0;
}

void func_800CA2E4(u8 a0, s8 a1)
{
    (void)a0;
    (void)a1;
}

void func_800CA30C(u8 a0)
{
    (void)a0;
}

void func_800CA330(u8 a0)
{
    (void)a0;
}

void func_800CA388(u8 a0)
{
    (void)a0;
}

void func_800CA49C(u8 a0)
{
    (void)a0;
}

void func_800CA59C(u8 a0)
{
    (void)a0;
}

void func_800CA730(u8 a0)
{
    (void)a0;
}

void func_800CA984(u8 a0)
{
    (void)a0;
}

void func_800CAACC(u8 a0)
{
    (void)a0;
}

void func_800CAB4C(u8 a0)
{
    (void)a0;
}

void func_800CAC60(u8 a0)
{
    (void)a0;
}

void func_800CAD40(s32 a0)
{
    (void)a0;
}

void func_800CADD0(u8 a0, f32 a1)
{
    (void)a0;
    (void)a1;
}

void func_800CAEC4(u8 a0, f32 a1)
{
    (void)a0;
    (void)a1;
}

void func_800CAFC0(u8 a0)
{
    (void)a0;
}

void func_800CB064(u8 a0)
{
    (void)a0;
}

void func_800CB134(void)
{
}

void func_800CB2C4(void)
{
}

void play_sequence(u16 a0)
{
    (void)a0;
}

void play_sequence2(u16 a0)
{
    (void)a0;
}

void play_sound2(s32 a0)
{
    (void)a0;
}

#endif
