package javax.wireless.messaging;

import java.io.IOException;

public interface TextMessage extends Message {
    String getPayloadText();

    void setPayloadText(String data) throws IOException;
}
