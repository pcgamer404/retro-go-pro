package javax.bluetooth;

/**
 * Local Bluetooth device (JSR-82). This build has no radio, but the object
 * and its queries behave as an "adapter present, nothing to find" device.
 */
public class LocalDevice {
    private static LocalDevice instance;

    private final DiscoveryAgent agent = new Agent();

    protected LocalDevice() {
    }

    public static LocalDevice getLocalDevice() throws BluetoothStateException {
        synchronized (LocalDevice.class) {
            if (instance == null) {
                instance = new LocalDevice();
            }
            return instance;
        }
    }

    public static boolean isPowerOn() {
        return true;
    }

    public static String getProperty(String property) {
        if (property == null) {
            throw new IllegalArgumentException("null property");
        }
        if (property.equals("bluetooth.api.version")) {
            return "1.1";
        }
        if (property.equals("bluetooth.sd.trans.max")) {
            return "1";
        }
        if (property.equals("bluetooth.sd.attr.retrievable.max")) {
            return "32";
        }
        if (property.equals("bluetooth.connected.devices.max")) {
            return "7";
        }
        if (property.equals("bluetooth.connected.inquiry")) {
            return "true";
        }
        if (property.equals("bluetooth.connected.page")) {
            return "true";
        }
        if (property.equals("bluetooth.connected.page.scan")) {
            return "true";
        }
        if (property.equals("bluetooth.connected.inquiry.scan")) {
            return "true";
        }
        if (property.equals("bluetooth.l2cap.receiveMTU.max")) {
            return "672";
        }
        return null;
    }

    public String getBluetoothAddress() {
        return "000000000000";
    }

    public String getFriendlyName() {
        return "nanojava";
    }

    public DeviceClass getDeviceClass() {
        return new DeviceClass(0x000100);
    }

    public int getDiscoverable() {
        return 0x00;
    }

    public boolean setDiscoverable(int mode) throws BluetoothStateException {
        return true;
    }

    public DiscoveryAgent getDiscoveryAgent() {
        return agent;
    }

    public boolean setDeviceServiceClasses(int classes) {
        return true;
    }

    public void updateRecord(ServiceRecord srvRecord)
            throws ServiceRegistrationException {
    }

    public ServiceRecord getRecord(javax.microedition.io.Connection c) {
        return null;
    }

    /** Discovery that finds nothing. */
    private static final class Agent extends DiscoveryAgent {
        public RemoteDevice[] retrieveDevices(int option) {
            return null;
        }

        public boolean startInquiry(int accessCode, DiscoveryListener listener)
                throws BluetoothStateException {
            if (listener == null) {
                throw new IllegalArgumentException("null listener");
            }
            return false;
        }

        public boolean cancelInquiry(DiscoveryListener listener) {
            return false;
        }

        public int searchServices(int[] attrSet, UUID[] uuidSet,
                RemoteDevice btDev, DiscoveryListener discListener)
                throws BluetoothStateException {
            throw new BluetoothStateException("Bluetooth is not available");
        }

        public boolean cancelServiceSearch(int transID) {
            return false;
        }

        public String selectService(UUID uuid, int security, boolean master)
                throws BluetoothStateException {
            return null;
        }
    }
}
