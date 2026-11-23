package javax.microedition.io.file;

import java.util.Enumeration;
import java.util.Vector;

/** Registry of mounted filesystem roots (JSR-75). */
public class FileSystemRegistry {
    private static final Vector listeners = new Vector();

    private FileSystemRegistry() {
    }

    public static Enumeration listRoots() {
        Vector roots = new Vector();
        roots.addElement("file:///");
        return roots.elements();
    }

    public static boolean addFileSystemListener(FileSystemListener listener) {
        if (listener == null) {
            throw new NullPointerException("null listener");
        }
        synchronized (listeners) {
            if (!listeners.contains(listener)) {
                listeners.addElement(listener);
            }
        }
        return true;
    }

    public static boolean removeFileSystemListener(FileSystemListener listener) {
        if (listener == null) {
            throw new NullPointerException("null listener");
        }
        synchronized (listeners) {
            return listeners.removeElement(listener);
        }
    }
}
