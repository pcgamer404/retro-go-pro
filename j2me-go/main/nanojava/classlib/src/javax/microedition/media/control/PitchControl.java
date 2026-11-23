package javax.microedition.media.control;

import javax.microedition.media.Control;

/**
 * Pitch control (JSR-135). Accepted and ignored: the synth has a fixed pitch.
 */
public interface PitchControl extends Control {
    int setPitch(int millisemitones);

    int getPitch();
}
