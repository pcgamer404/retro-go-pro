package javax.wireless.messaging;

import java.io.IOException;

/**
 * Message connection (JSR-120). Obtained from
 * {@code Connector.open("sms://...")}.
 */
public interface MessageConnection extends javax.microedition.io.Connection {
    String TEXT_MESSAGE = "text";
    String BINARY_MESSAGE = "binary";

    Message newMessage(String type);

    Message newMessage(String type, String address);

    int numberOfSegments(Message msg);

    Message receive() throws IOException;

    void send(Message msg) throws IOException;

    void setMessageListener(MessageListener l) throws IOException;

    MessageListener getMessageListener() throws IOException;
}
