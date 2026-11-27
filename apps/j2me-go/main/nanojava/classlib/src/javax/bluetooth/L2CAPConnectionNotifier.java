package javax.bluetooth;

import java.io.IOException;
import javax.microedition.io.Connection;

/** L2CAP server connection (JSR-82). */
public interface L2CAPConnectionNotifier extends Connection {
    L2CAPConnection acceptAndOpen() throws IOException;
}
