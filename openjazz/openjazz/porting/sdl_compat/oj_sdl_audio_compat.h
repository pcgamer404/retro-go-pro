#ifndef OJ_SDL_AUDIO_COMPAT_H
#define OJ_SDL_AUDIO_COMPAT_H
/* SDL 1.2-style audio subset for the OpenJazz Retro-Go port: one 16-bit stereo
 * output, pulled through a callback by a small task that feeds rg_audio_submit(). */
#include "oj_sdl_event_compat.h"
#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_U8     0x0008
#define AUDIO_S8     0x8008
#define AUDIO_U16LSB 0x0010
#define AUDIO_S16LSB 0x8010
#define AUDIO_U16MSB 0x1010
#define AUDIO_S16MSB 0x9010
#define AUDIO_S16SYS AUDIO_S16LSB
#define SDL_MIX_MAXVOLUME 128
#define SDL_AUDIO_BITSIZE(x)    ((x) & 0xFF)
#define SDL_AUDIO_ISUNSIGNED(x) (!((x) & 0x8000))

typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);

typedef struct SDL_AudioSpec {
    int freq;
    Uint16 format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    SDL_AudioCallback callback;
    void *userdata;
} SDL_AudioSpec;

int  SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained);
void SDL_PauseAudio(int pause_on);
void SDL_CloseAudio(void);
void SDL_LockAudio(void);
void SDL_UnlockAudio(void);
void SDL_MixAudio(Uint8 *dst, const Uint8 *src, Uint32 len, int volume);

/* Port extension: silence the output while Retro-Go's own menu is open. */
void oj_sdl_audio_suspend(int on);

#ifdef __cplusplus
}
#endif
#endif
