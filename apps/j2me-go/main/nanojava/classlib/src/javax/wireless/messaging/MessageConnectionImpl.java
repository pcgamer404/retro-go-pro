package javax.wireless.messaging;

import java.io.IOException;
import java.util.Vector;

/**
 * A message connection with no radio behind it. {@code send} succeeds and
 * discards the message, which is what titles that offer optional "send score"
 * or "tell a friend" features expect; {@code receive} blocks forever because
 * nothing can arrive. Creating and filling messages works normally.
 */
public class MessageConnectionImpl implements MessageConnection {
    private final String address;
    private final Vector inbox = new Vector();
    private MessageListener listener;
    private boolean closed;

    public MessageConnectionImpl(String address) {
        this.address = address;
    }

    public Message newMessage(String type) {
        if (type == null) {
            throw new IllegalArgumentException("null message type");
        }
        if (TEXT_MESSAGE.equals(type)) {
            return new TextMessageImpl();
        }
        if (BINARY_MESSAGE.equals(type)) {
            return new BinaryMessageImpl();
        }
        throw new IllegalArgumentException("unsupported message type: " + type);
    }

    public Message newMessage(String type, String address) {
        Message m = newMessage(type);
        try {
            m.setAddress(address);
        } catch (IOException e) {
            /* Cannot happen for our in-memory message. */
        }
        return m;
    }

    public int numberOfSegments(Message msg) {
        return 1;
    }

    public Message receive() throws IOException {
        synchronized (inbox) {
            for (;;) {
                if (closed) {
                    throw new IOException("connection closed");
                }
                if (inbox.size() > 0) {
                    Message m = (Message) inbox.elementAt(0);
                    inbox.removeElementAt(0);
                    return m;
                }
                try {
                    inbox.wait();
                } catch (InterruptedException e) {
                    throw new InterruptedIOException();
                }
            }
        }
    }

    public void send(Message msg) throws IOException {
        if (closed) {
            throw new IOException("connection closed");
        }
        if (msg == null) {
            throw new IllegalArgumentException("null message");
        }
        /* No radio: the message is accepted and dropped. */
    }

    public void setMessageListener(MessageListener l) throws IOException {
        if (closed) {
            throw new IOException("connection closed");
        }
        listener = l;
    }

    public MessageListener getMessageListener() throws IOException {
        if (closed) {
            throw new IOException("connection closed");
        }
        return listener;
    }

    public String getAddress() {
        return address;
    }

    public void close() {
        synchronized (inbox) {
            closed = true;
            inbox.notifyAll();
        }
    }

    /** java.io.InterruptedIOException, kept local to avoid the import. */
    private static final class InterruptedIOException extends IOException {
        InterruptedIOException() {
            super("interrupted");
        }
    }
}
