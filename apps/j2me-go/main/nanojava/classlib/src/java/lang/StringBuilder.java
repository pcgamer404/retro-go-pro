package java.lang;

/*
 * Small compatibility shim for class libraries compiled with javac 8+.
 * Nano Java's original build uses javac 8 with source/target 1.3, which
 * normally emits StringBuffer calls for concatenation. Modern javac emits
 * StringBuilder calls even when targeting classfile version 52.
 */
public final class StringBuilder {
    private StringBuffer buffer;

    public StringBuilder() {
        buffer = new StringBuffer();
    }

    public StringBuilder(int capacity) {
        buffer = new StringBuffer(capacity);
    }

    public StringBuilder(String s) {
        buffer = new StringBuffer(s != null ? s : "null");
    }

    public StringBuilder append(Object v) { buffer.append(v); return this; }
    public StringBuilder append(String v) { buffer.append(v); return this; }
    public StringBuilder append(StringBuffer sb) { buffer.append(sb != null ? sb.toString() : "null"); return this; }
    public StringBuilder append(char v) { buffer.append(v); return this; }
    public StringBuilder append(char[] v) { buffer.append(v); return this; }
    public StringBuilder append(char[] v, int off, int len) { buffer.append(v, off, len); return this; }
    public StringBuilder append(boolean v) { buffer.append(v); return this; }
    public StringBuilder append(int v) { buffer.append(v); return this; }
    public StringBuilder append(long v) { buffer.append(v); return this; }
    public StringBuilder append(float v) { buffer.append(v); return this; }
    public StringBuilder append(double v) { buffer.append(v); return this; }

    public StringBuilder delete(int start, int end) { buffer.delete(start, end); return this; }
    public StringBuilder deleteCharAt(int index) { buffer.deleteCharAt(index); return this; }
    public StringBuilder reverse() { buffer.reverse(); return this; }

    public StringBuilder insert(int offset, Object v) { buffer.insert(offset, v); return this; }
    public StringBuilder insert(int offset, String v) { buffer.insert(offset, v); return this; }
    public StringBuilder insert(int offset, char[] v) { buffer.insert(offset, v); return this; }
    public StringBuilder insert(int offset, boolean v) { buffer.insert(offset, v); return this; }
    public StringBuilder insert(int offset, char v) { buffer.insert(offset, v); return this; }
    public StringBuilder insert(int offset, int v) { buffer.insert(offset, v); return this; }
    public StringBuilder insert(int offset, long v) { buffer.insert(offset, v); return this; }
    public StringBuilder insert(int offset, float v) { buffer.insert(offset, v); return this; }
    public StringBuilder insert(int offset, double v) { buffer.insert(offset, v); return this; }

    public void setLength(int newLength) { buffer.setLength(newLength); }
    public void ensureCapacity(int minimumCapacity) { buffer.ensureCapacity(minimumCapacity); }
    public void setCharAt(int index, char ch) { buffer.setCharAt(index, ch); }

    public int length() { return buffer.length(); }
    public int capacity() { return buffer.capacity(); }
    public char charAt(int i) { return buffer.charAt(i); }
    public String toString() { return buffer.toString(); }
}
