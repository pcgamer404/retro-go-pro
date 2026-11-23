package javax.bluetooth;

/** Device class record (JSR-82). */
public class DeviceClass {
    private final int record;

    public DeviceClass(int record) {
        this.record = record;
    }

    public int getMajorDeviceClass() {
        return (record >> 8) & 0x1f;
    }

    public int getMinorDeviceClass() {
        return (record >> 2) & 0x3f;
    }

    public int getServiceClasses() {
        return record & 0xffe00000;
    }

    public int getDeviceClass() {
        return record;
    }
}
