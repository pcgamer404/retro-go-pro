package javax.bluetooth;

import java.io.IOException;

/** Connection setup error (JSR-82). */
public class BluetoothConnectionException extends IOException {
    public static final int UNKNOWN = 0x0001;
    public static final int SECURITY_BLOCK = 0x0002;
    public static final int NO_RESOURCES = 0x0003;
    public static final int TIMEOUT = 0x0004;
    public static final int CONNECTION_TO_DEVICE_FAILED = 0x0005;
    public static final int CONNECTION_TO_DEVICE_NOT_ALLOWED = 0x0006;

    private final int status;

    public BluetoothConnectionException(int status) {
        super("Bluetooth connection failed: " + status);
        this.status = status;
    }

    public BluetoothConnectionException(int status, String msg) {
        super(msg);
        this.status = status;
    }

    public int getStatus() {
        return status;
    }
}
