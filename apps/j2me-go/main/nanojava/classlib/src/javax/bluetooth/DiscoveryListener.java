package javax.bluetooth;

/** Callback interface for device and service discovery (JSR-82). */
public interface DiscoveryListener {
    int INQUIRY_COMPLETED = 0;
    int INQUIRY_TERMINATED = 1;
    int INQUIRY_ERROR = 2;

    int SERVICE_SEARCH_COMPLETED = 1;
    int SERVICE_SEARCH_TERMINATED = 2;
    int SERVICE_SEARCH_ERROR = 3;
    int SERVICE_SEARCH_NO_RECORDS = 4;
    int SERVICE_SEARCH_DEVICE_NOT_REACHABLE = 6;

    void deviceDiscovered(RemoteDevice btDevice, DeviceClass cod);

    void servicesDiscovered(int transID, ServiceRecord[] servRecord);

    void serviceSearchCompleted(int transID, int respCode);

    void inquiryCompleted(int discType);
}
