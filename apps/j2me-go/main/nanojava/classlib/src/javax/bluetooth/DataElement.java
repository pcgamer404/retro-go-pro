package javax.bluetooth;

import java.util.Vector;

/**
 * Bluetooth data element (JSR-82). Holds the value and answers the accessor
 * the games use; sequence elements are kept in a Vector.
 */
public class DataElement {
    public static final int NULL = 0;
    public static final int U_INT_1 = 8;
    public static final int U_INT_2 = 9;
    public static final int U_INT_4 = 10;
    public static final int U_INT_8 = 11;
    public static final int U_INT_16 = 12;
    public static final int INT_1 = 16;
    public static final int INT_2 = 17;
    public static final int INT_4 = 18;
    public static final int INT_8 = 19;
    public static final int INT_16 = 20;
    public static final int URL = 64;
    public static final int UUID = 24;
    public static final int BOOL = 40;
    public static final int STRING = 32;
    public static final int DATSEQ = 48;
    public static final int DATALT = 56;

    private int type;
    private Object value;

    public DataElement(int valueType) {
        if (valueType == NULL) {
            type = NULL;
            value = null;
        } else if (valueType == DATSEQ || valueType == DATALT) {
            type = valueType;
            value = new Vector();
        } else {
            throw new IllegalArgumentException("invalid valueType: " + valueType);
        }
    }

    public DataElement(boolean bool) {
        type = BOOL;
        value = new Boolean(bool);
    }

    public DataElement(int valueType, long value) {
        type = valueType;
        this.value = new Long(value);
    }

    public DataElement(int valueType, Object value) {
        if (value == null) {
            throw new IllegalArgumentException("null value");
        }
        type = valueType;
        this.value = value;
    }

    /** Convenience for the common STRING element. */
    public DataElement(String value) {
        this(STRING, value);
    }

    /** Convenience for the common UUID element. */
    public DataElement(UUID value) {
        this(UUID, value);
    }

    public void addElement(DataElement elem) {
        seq().addElement(elem);
    }

    public void insertElementAt(DataElement elem, int index) {
        seq().insertElementAt(elem, index);
    }

    public void removeElementAt(int index) {
        seq().removeElementAt(index);
    }

    public int getSize() {
        return seq().size();
    }

    public DataElement getElementAt(int index) {
        return (DataElement) seq().elementAt(index);
    }

    public int getDataType() {
        return type;
    }

    public boolean getBoolean() {
        return value instanceof Boolean && ((Boolean) value).booleanValue();
    }

    public int getInt() {
        return value instanceof Number ? ((Number) value).intValue() : 0;
    }

    public long getLong() {
        return value instanceof Number ? ((Number) value).longValue() : 0;
    }

    public Object getValue() {
        return value;
    }

    public void setValue(int value) {
        type = INT_4;
        this.value = new Long(value);
    }

    public void setValue(long value) {
        type = INT_8;
        this.value = new Long(value);
    }

    public void setValue(boolean value) {
        type = BOOL;
        this.value = new Boolean(value);
    }

    public void setValue(Object value) {
        this.value = value;
    }

    private Vector seq() {
        if (type != DATSEQ && type != DATALT) {
            throw new IllegalArgumentException("not a sequence");
        }
        return (Vector) value;
    }
}
