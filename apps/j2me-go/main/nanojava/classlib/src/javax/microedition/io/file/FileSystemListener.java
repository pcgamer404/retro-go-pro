package javax.microedition.io.file;

/** Notification of filesystem roots appearing or disappearing (JSR-75). */
public interface FileSystemListener {
    int ROOT_ADDED = 0;
    int ROOT_REMOVED = 1;

    void rootChanged(int state, String rootName);
}
