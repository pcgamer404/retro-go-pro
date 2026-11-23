package javax.wireless.messaging;

import java.io.IOException;

public interface BinaryMessage extends Message {
    byte[] getPayloadData();

    void setPayloadData(byte[] data) throws IOException;
}
