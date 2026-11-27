package javax.microedition.media.control;

import javax.microedition.media.Control;
import javax.microedition.media.MediaException;

/**
 * MIDI control (JSR-135). Present so titles that query it at startup (for
 * example Gameloft's Assassin's Creed III) can load. The calls are accepted
 * and ignored; the built-in synth plays the song regardless.
 */
public interface MIDIControl extends Control {
    boolean isBankQuerySupported();

    int[] getProgram(int channel) throws MediaException;

    int getChannelVolume(int channel);

    void setProgram(int channel, int bank, int program);

    void setChannelVolume(int channel, int volume);

    int[] getBankList(boolean custom) throws MediaException;

    int[] getProgramList(int bank) throws MediaException;

    void shortMidiEvent(int type, int data1, int data2);

    int longMidiEvent(byte[] data, int offset, int length);
}
