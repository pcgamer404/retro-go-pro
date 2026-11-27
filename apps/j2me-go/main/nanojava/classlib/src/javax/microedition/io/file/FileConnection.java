package javax.microedition.io.file;

import java.io.IOException;
import javax.microedition.io.StreamConnection;

/**
 * A file connection (JSR-75). The device has no writable filesystem exposed
 * to Java, so file URLs are resolved against the resources packaged in the
 * jar: reading a bundled file works, every mutation reports an IOException.
 */
public interface FileConnection extends StreamConnection {
    boolean isHidden() throws IOException;

    boolean canRead();

    boolean canWrite();

    boolean isDirectory();

    boolean exists();

    long lastModified();

    long fileSize() throws IOException;

    String getName();

    String getPath();

    String getURL();

    java.util.Enumeration list() throws IOException;

    java.util.Enumeration list(String filter, boolean includeHidden)
            throws IOException;

    void mkdir() throws IOException;

    void mkdirs() throws IOException;

    void create() throws IOException;

    void delete() throws IOException;

    void rename(String newName) throws IOException;

    void truncate(long byteOffset) throws IOException;

    void setReadOnly() throws IOException;

    void setWritable(boolean writable) throws IOException;

    void setHidden(boolean hidden) throws IOException;

    long directorySize(boolean includeSubDirs) throws IOException;

    long availableSize();

    long totalSize();

    long usedSize();

    void setFileConnection(String fileName) throws IOException;
}
