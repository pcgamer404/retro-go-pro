package javax.microedition.io.file;

import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Enumeration;
import java.util.Vector;
import javax.microedition.io.Connector;

/**
 * FileConnection over the jar resource table. Paths are matched against
 * bundled entries; a file that is not packaged cannot be opened. All write
 * operations fail because the filesystem is read-only from Java.
 */
public class FileConnectionImpl implements FileConnection {
    private final String url;
    private final int mode;
    private String path;
    private boolean closed;
    private boolean known;
    private byte[] cache;

    public FileConnectionImpl(String url, int mode) {
        this.url = url;
        this.mode = mode;
        this.path = normalize(url);
    }

    private static String normalize(String url) {
        String p = url;
        if (p.startsWith("file://")) {
            p = p.substring("file://".length());
        } else if (p.startsWith("file:")) {
            p = p.substring("file:".length());
        }
        if (p.startsWith("localhost/")) {
            p = p.substring("localhost".length());
        }
        while (p.startsWith("/")) {
            p = p.substring(1);
        }
        return p;
    }

    private byte[] data() {
        if (!known) {
            cache = nanojava.Resources.read(path);
            known = true;
        }
        return cache;
    }

    private static IOException readOnly() {
        return new IOException("filesystem is read-only");
    }

    public InputStream openInputStream() throws IOException {
        checkOpen();
        if ((mode & Connector.READ) == 0) {
            throw new IOException("not opened for reading");
        }
        byte[] d = data();
        if (d == null) {
            throw new IOException("no such file: " + path);
        }
        return new ByteArrayInputStream(d);
    }

    public DataInputStream openDataInputStream() throws IOException {
        return new DataInputStream(openInputStream());
    }

    public OutputStream openOutputStream() throws IOException {
        throw readOnly();
    }

    public DataOutputStream openDataOutputStream() throws IOException {
        throw readOnly();
    }

    public boolean isHidden() {
        return false;
    }

    public boolean canRead() {
        return true;
    }

    public boolean canWrite() {
        return false;
    }

    public boolean isDirectory() {
        return false;
    }

    public boolean exists() {
        return data() != null;
    }

    public long lastModified() {
        return 0;
    }

    public long fileSize() throws IOException {
        byte[] d = data();
        if (d == null) {
            throw new IOException("no such file: " + path);
        }
        return d.length;
    }

    public String getName() {
        int slash = path.lastIndexOf('/');
        return slash < 0 ? path : path.substring(slash + 1);
    }

    public String getPath() {
        return "/" + path;
    }

    public String getURL() {
        return url;
    }

    public Enumeration list() throws IOException {
        return new Vector().elements();
    }

    public Enumeration list(String filter, boolean includeHidden)
            throws IOException {
        if (filter == null) {
            throw new NullPointerException("null filter");
        }
        return new Vector().elements();
    }

    public void mkdir() throws IOException {
        throw readOnly();
    }

    public void mkdirs() throws IOException {
        throw readOnly();
    }

    public void create() throws IOException {
        throw readOnly();
    }

    public void delete() throws IOException {
        throw readOnly();
    }

    public void rename(String newName) throws IOException {
        throw readOnly();
    }

    public void truncate(long byteOffset) throws IOException {
        throw readOnly();
    }

    public void setReadOnly() throws IOException {
        throw readOnly();
    }

    public void setWritable(boolean writable) throws IOException {
        if (writable) {
            throw readOnly();
        }
    }

    public void setHidden(boolean hidden) throws IOException {
        throw readOnly();
    }

    public long directorySize(boolean includeSubDirs) throws IOException {
        return 0;
    }

    public long availableSize() {
        return 0;
    }

    public long totalSize() {
        return 0;
    }

    public long usedSize() {
        return 0;
    }

    public void setFileConnection(String fileName) throws IOException {
        checkOpen();
        if (fileName == null) {
            throw new NullPointerException("null name");
        }
        if (fileName.equals("..")) {
            int slash = path.lastIndexOf('/');
            path = slash <= 0 ? "" : path.substring(0, slash);
        } else if (!fileName.equals(".")) {
            path = path.length() == 0 ? fileName : path + "/" + fileName;
        }
        known = false;
        cache = null;
    }

    public void close() {
        closed = true;
        cache = null;
    }

    private void checkOpen() throws IOException {
        if (closed) {
            throw new IOException("connection closed");
        }
    }
}
