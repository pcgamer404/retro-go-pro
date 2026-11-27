package javax.wireless.messaging;

public class BinaryMessageImpl extends AbstractMessage implements BinaryMessage {
    private byte[] payload = new byte[0];

    public byte[] getPayloadData() {
        return payload;
    }

    public void setPayloadData(byte[] data) {
        payload = data == null ? new byte[0] : data;
    }
}
