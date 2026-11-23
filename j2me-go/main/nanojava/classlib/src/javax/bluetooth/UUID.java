package javax.bluetooth;

/**
 * Bluetooth service UUID (JSR-82). The stack is not present, but the type is
 * needed by titles that link their (optional) multiplayer code at load time.
 */
public class UUID {
    private final String text;

    public UUID(long uuidValue) {
        this.text = base(hex(uuidValue & 0xffffffffL, 8));
    }

    public UUID(String uuidValue, boolean shortUUID) {
        if (uuidValue == null) {
            throw new NullPointerException();
        }
        String s = uuidValue.toUpperCase();
        if (shortUUID) {
            if (s.length() > 8) {
                throw new IllegalArgumentException("short UUID too long");
            }
            while (s.length() < 8) {
                s = "0" + s;
            }
            this.text = s + "-0000-1000-8000-00805F9B34FB";
        } else {
            StringBuffer b = new StringBuffer();
            for (int i = 0; i < s.length(); i++) {
                char c = s.charAt(i);
                if (c != '-') {
                    b.append(c);
                }
            }
            if (b.length() != 32) {
                throw new IllegalArgumentException("invalid UUID");
            }
            String u = b.toString();
            this.text = u.substring(0, 8) + "-" + u.substring(8, 12) + "-"
                    + u.substring(12, 16) + "-" + u.substring(16, 20) + "-"
                    + u.substring(20, 32);
        }
    }

    private static String base(String shortPart) {
        return shortPart + "-0000-1000-8000-00805F9B34FB";
    }

    private static String hex(long v, int digits) {
        char[] c = new char[digits];
        for (int i = digits - 1; i >= 0; i--) {
            int d = (int) (v & 15);
            c[i] = (char) (d < 10 ? '0' + d : 'A' + d - 10);
            v >>= 4;
        }
        return new String(c);
    }

    public boolean equals(Object other) {
        if (!(other instanceof UUID)) {
            return false;
        }
        return text.equals(((UUID) other).text);
    }

    public int hashCode() {
        return text.hashCode();
    }

    public String toString() {
        return text;
    }
}
