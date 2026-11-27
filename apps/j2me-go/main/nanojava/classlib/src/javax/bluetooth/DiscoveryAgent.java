package javax.bluetooth;

/**
 * Device/service discovery agent (JSR-82). The implementation in
 * {@link LocalDevice} reports "not available" instead of pretending to find
 * devices, so games fall back to their single-player paths.
 */
public abstract class DiscoveryAgent {
    public static final int GIAC = 0x9E8B33;
    public static final int LIAC = 0x9E8B00;
    public static final int CACHED = 0x01;
    public static final int PREKNOWN = 0x02;

    protected DiscoveryAgent() {
    }

    public abstract RemoteDevice[] retrieveDevices(int option);

    public abstract boolean startInquiry(int accessCode,
            DiscoveryListener listener) throws BluetoothStateException;

    public abstract boolean cancelInquiry(DiscoveryListener listener);

    public abstract int searchServices(int[] attrSet, UUID[] uuidSet,
            RemoteDevice btDev, DiscoveryListener discListener)
            throws BluetoothStateException;

    public abstract boolean cancelServiceSearch(int transID);

    public abstract String selectService(UUID uuid, int security,
            boolean master) throws BluetoothStateException;
}
