package javax.wireless.messaging;

import java.util.Date;

/**
 * Base interface of a wireless message (JSR-120). There is no radio in this
 * build: messages can be created and filled in, and sending succeeds as a
 * no-op so titles that use SMS for optional features keep running.
 */
public interface Message {
    String getAddress();

    void setAddress(String addr) throws java.io.IOException;

    Date getTimestamp();
}
