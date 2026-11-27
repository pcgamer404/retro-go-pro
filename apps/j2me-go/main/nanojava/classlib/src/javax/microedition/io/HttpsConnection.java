package javax.microedition.io;

/**
 * Secure HTTP connection. This build has no network stack, so
 * {@link Connector#open} reports {@link ConnectionNotFoundException} for
 * https URLs; the interface exists so classes that refer to it (The Sims 3,
 * several Gameloft titles) still load.
 */
public interface HttpsConnection extends HttpConnection {
}
