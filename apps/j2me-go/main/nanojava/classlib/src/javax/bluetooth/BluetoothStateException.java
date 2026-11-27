package javax.bluetooth;

import java.io.IOException;

/** Generic Bluetooth error (JSR-82). */
public class BluetoothStateException extends IOException {
    public BluetoothStateException() {
        super();
    }

    public BluetoothStateException(String msg) {
        super(msg);
    }
}
