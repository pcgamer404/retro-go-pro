package javax.microedition.media.control;

/**
 * Tempo control (JSR-135). Used by MIDI-style players.
 */
public interface TempoControl extends RateControl {
    int setTempo(int milliTempo);

    int getTempo();
}
