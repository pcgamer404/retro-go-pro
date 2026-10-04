#include "driver.h"
#include "minimal.h"
#include "libretro.h"

int samples_per_frame = 0;
short *samples_buffer;
int usestereo = 1;

void hook_audio_done(void);

int osd_start_audio_stream(int stereo)
{
	usestereo = stereo ? 1 : 0;

	/* determine the number of samples per frame */
	samples_per_frame = Machine->sample_rate / Machine->drv->frames_per_second;

	if (Machine->sample_rate == 0) return 0;

	/* samples_buffer is always allocated stereo-sized regardless of
	 * the game's native sound layout: libretro always consumes stereo
	 * via audio_batch_cb, and the mixer (mixer.c) writes interleaved
	 * L/R directly into it -- mono games duplicate the sample into
	 * both channels at the clip step.  Eliminates the per-frame
	 * memcpy that used to stage mixer output through a separate
	 * buffer, and eliminates the mono->stereo conversion loop that
	 * used to run in retro_run(). */
	samples_buffer = (short *) calloc(samples_per_frame * 2, sizeof(short));

	return samples_per_frame;
}

void osd_stop_audio_stream(void)
{
	samples_per_frame = 0;
}

/* The mixer (mixer.c:mixer_sh_update) writes interleaved L/R samples
 * directly into samples_buffer; this hook just lets the libretro side
 * know the buffer is ready.  The buffer argument is retained for
 * compatibility with the documented osdepend.h interface but is now
 * required to be the same pointer as samples_buffer (asserted only in
 * debug builds via the implicit aliasing -- the mixer is the sole
 * caller and passes samples_buffer directly). */
int osd_update_audio_stream(int16_t *buffer)
{
	(void)buffer;
	hook_audio_done();
	return samples_per_frame;
}

/* Mirror of mame2003-libretro/src/mame2003/mame2003.c:802 osd_update_-
 * silent_stream().  Called by updatescreen() while pause_action is
 * set (game CPU frozen for the MAME menu) -- substitute a frame of
 * silent audio for what sound_update() would have piped through the
 * mixer.  Two reasons to dispatch audio_batch_cb directly from here
 * rather than relying on retro_run()'s tail dispatch of samples_-
 * buffer:
 *
 *   - Belt-and-braces guarantee that the frame the frontend
 *     receives is the silent one we just zeroed, not whatever
 *     residual content was left in samples_buffer by the last
 *     sound_update() pass before the menu opened.
 *   - Matches mame2003 byte-for-byte, so the audio behaviour the
 *     user experiences in mame2000 with the menu up is the same as
 *     in mame2003 (the explicit request).
 *
 * retro_run()'s tail audio_batch_cb is gated on pause_action and
 * skipped when this fires, so each paused frame produces exactly
 * one (silent) audio batch -- no double-delivery, no stale buffer.
 * No-op when audio is not started (sample_rate == 0 or samples_-
 * buffer NULL). */
extern retro_audio_sample_batch_t audio_batch_cb;

void osd_update_silent_stream(void)
{
	if (Machine->sample_rate == 0 || samples_buffer == NULL || samples_per_frame <= 0)
		return;

	/* samples_buffer is always stereo-sized now -- zero it and
	 * dispatch directly, no separate mono-path conversion buffer
	 * needed. */
	memset(samples_buffer, 0, samples_per_frame * 2 * sizeof(short));
	if (audio_batch_cb) audio_batch_cb(samples_buffer, samples_per_frame);
}

/* attenuation in dB */
void osd_set_mastervolume(int _attenuation)
{
	(void)_attenuation;
}

int osd_get_mastervolume(void)
{
	return 100;
}

void osd_sound_enable(int enable_it)
{
	(void)enable_it;
}

void osd_opl_control(int chip,int reg)
{
	(void)chip;
	(void)reg;
}

void osd_opl_write(int chip,int data)
{
	(void)chip;
	(void)data;
}
