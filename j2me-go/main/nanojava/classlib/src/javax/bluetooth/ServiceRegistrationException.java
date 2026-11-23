package javax.bluetooth;

import java.io.IOException;

/** Service registration error (JSR-82). */
public class ServiceRegistrationException extends IOException {
    public ServiceRegistrationException() {
        super();
    }

    public ServiceRegistrationException(String msg) {
        super(msg);
    }
}
