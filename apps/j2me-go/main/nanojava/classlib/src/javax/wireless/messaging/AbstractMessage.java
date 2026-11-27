package javax.wireless.messaging;

import java.util.Date;

/** Shared state for the SMS/MMS message implementations. */
abstract class AbstractMessage implements Message {
    private String address;
    private final Date timestamp = new Date();

    public String getAddress() {
        return address;
    }

    public void setAddress(String addr) {
        address = addr;
    }

    public Date getTimestamp() {
        return timestamp;
    }
}
