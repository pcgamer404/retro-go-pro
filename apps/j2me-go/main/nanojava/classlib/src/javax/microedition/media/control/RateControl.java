package javax.microedition.media.control;

import javax.microedition.media.Control;

/**
 * Playback rate control (JSR-135). NanoJava has no real audio clock, so the
 * rate is stored and reported but does not change the simulated media time.
 */
public interface RateControl extends Control {
    int setRate(int millirate);

    int getRate();
}
