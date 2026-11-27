package javax.bluetooth;

import java.io.IOException;

/** A remote Bluetooth device (JSR-82). */
public class RemoteDevice {
    private final String address;

    protected RemoteDevice(String address) {
        this.address = address == null ? "000000000000" : address;
    }

    public String getBluetoothAddress() {
        return address;
    }

    public String getFriendlyName(boolean alwaysAsk) throws IOException {
        return address;
    }

    public boolean isTrustedDevice() {
        return false;
    }

    public boolean authenticate() throws IOException {
        return false;
    }

    public boolean authorize(String string) throws IOException {
        return false;
    }

    public boolean encrypt(boolean value) throws IOException {
        return false;
    }

    public boolean isAuthenticated() {
        return false;
    }

    public boolean isEncrypted() {
        return false;
    }

    public boolean equals(Object other) {
        if (!(other instanceof RemoteDevice)) {
            return false;
        }
        return address.equals(((RemoteDevice) other).address);
    }

    public int hashCode() {
        return address.hashCode();
    }
}
