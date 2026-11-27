package javax.bluetooth;

/**
 * A service record (JSR-82). There is no Bluetooth stack, so records are
 * empty placeholders; titles that link their optional multiplayer code at
 * load time can still resolve the type and get harmless answers.
 */
public class ServiceRecord {
    public ServiceRecord() {
    }

    public String getConnectionURL(int requiredSecurity, boolean mustBeMaster) {
        return null;
    }

    public RemoteDevice getHostDevice() {
        return null;
    }

    public int[] getAttributeIDs() {
        return new int[0];
    }

    public DataElement getAttributeValue(int attrID) {
        return null;
    }

    public boolean populateRecord(int[] attrIDs) {
        return false;
    }

    public void setDeviceServiceClasses(int classes) {
    }

    public boolean setAttributeValue(int attrID, DataElement attrValue) {
        return false;
    }

    public String toString() {
        return "ServiceRecord";
    }
}
