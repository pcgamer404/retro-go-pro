#ifndef OSWAN_CONFIG_H
#define OSWAN_CONFIG_H

/* This handheld has no audio output (Retro-Go reports sink='Dummy').
 * 1 = do not synthesize or submit audio. Measured: the Dummy sink's wait was
 *     ~16 ms of dead time per frame, serialized with emulation, and the mixer
 *     (WsWaveSet) runs 636 times per frame for sound nobody hears.
 *     Emulated hardware state is unaffected (mixer output only feeds the ring).
 * 0 = original behaviour (synthesize + rg_audio_submit). */
#define OSWAN_NO_AUDIO 1

#endif
