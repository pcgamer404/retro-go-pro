#ifndef OSWAN_CONFIG_H
#define OSWAN_CONFIG_H

/* Audio output.
 * 0 = synthesize the PSG voices in WsWaveSet() and submit them through
 *     Retro-Go's audio driver (rg_audio_submit).  Submission also paces the
 *     emulator at the WonderSwan refresh rate, so the manual wait in main.c is
 *     not used.
 * 1 = compile the mixer and the submission out.  Only useful on a board with
 *     no working audio sink: the mixer produces 636 samples per frame for
 *     nobody and the sink's dead time slows the emulator down.
 * Emulated hardware state is unaffected either way (the mixer output only
 * feeds the audio ring). */
#define OSWAN_NO_AUDIO 0

#endif
