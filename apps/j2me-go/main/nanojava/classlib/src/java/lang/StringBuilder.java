package java.lang;

/*
 * Small compatibility shim for class libraries compiled with javac 8+.
 * Nano Java's original build uses javac 8 with source/target 1.3, which
 * normally emits StringBuffer calls for concatenation. Modern javac emits
 * StringBuilder calls even when targeting classfile version 52.
 */
public final class StringBuilder {
    private StringBuffer buffer = new StringBuffer();
    public StringBuilder() {}
    public StringBuilder(int capacity) {}
    public StringBuilder(String s) { buffer.append(s); }
    public StringBuilder append(Object v) { buffer.append(v); return this; }
    public StringBuilder append(String v) { buffer.append(v); return this; }
    public StringBuilder append(char v) { buffer.append(v); return this; }
    public StringBuilder append(char[] v) { buffer.append(v); return this; }
    public StringBuilder append(char[] v, int off, int len) { buffer.append(v, off, len); return this; }
    public StringBuilder append(boolean v) { buffer.append(v); return this; }
    public StringBuilder append(int v) { buffer.append(v); return this; }
    public StringBuilder append(long v) { buffer.append(v); return this; }
    public StringBuilder append(float v) { buffer.append(v); return this; }
    public StringBuilder append(double v) { buffer.append(v); return this; }
    public int length() { return buffer.length(); }
    public char charAt(int i) { return buffer.charAt(i); }
    public String toString() { return buffer.toString(); }
}
