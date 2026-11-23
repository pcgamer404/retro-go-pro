package nanojava;

import java.util.Hashtable;

/**
 * Application properties from the JAD (if the platform supplied one) and
 * the jar manifest. JAD values take precedence, as on real devices.
 */
public final class AppProperties {
    private static Hashtable props;

    private AppProperties() {
    }

    public static synchronized String get(String key) {
        if (props == null) {
            props = new Hashtable();
            byte[] manifest = Resources.read("META-INF/MANIFEST.MF");
            if (manifest != null) {
                parse(manifest, true);
            }
            byte[] jad = jad();
            if (jad != null) {
                parse(jad, false);
            }
        }
        String val = (String) props.get(key);
        if (val == null && key != null) {
            /* Glu titles read their runtime configuration from the JAD.  A jar
             * copied to a memory card without its JAD (the normal case here)
             * leaves those keys null, and the Glu launcher then calls
             * String.equals on the null and dies before the first frame.  The
             * values below are the neutral standalone configuration: no demo,
             * no upsell, no WAP portal.  Supplying them only fills gaps; a JAD
             * or manifest value always wins. */
            if ("Glu-Wap-Type".equalsIgnoreCase(key))          return "0";
            if ("Glu-Upsell-Enabled".equalsIgnoreCase(key))    return "false";
            if ("Glu-Demo-Enabled".equalsIgnoreCase(key))      return "false";
            if ("Glu-Cheats-Enable".equalsIgnoreCase(key))     return "false";
            if ("Glu-Ignore-Notify".equalsIgnoreCase(key))     return "true";
            if ("Glu-Use-Double-Buffer".equalsIgnoreCase(key)) return "true";
            if ("Glu-Softkey-Reverse".equalsIgnoreCase(key))   return "false";
            if ("Glu-Locale".equalsIgnoreCase(key))            return "en";
            if ("Glu-Canvas-Variance".equalsIgnoreCase(key))   return "0";
            if ("Glu-Idle-Threshold".equalsIgnoreCase(key))    return "0";
            if ("Glu-Demo-Game-Limit".equalsIgnoreCase(key))   return "0";
            if ("Glu-Demo-Time-Limit".equalsIgnoreCase(key))   return "0";
            if ("Glu-Upsell-URL".equalsIgnoreCase(key))        return "";
            if ("Glu-Demo-URL".equalsIgnoreCase(key))          return "";
            if ("Glu-Wap-Menu".equalsIgnoreCase(key))          return "";
            if ("Glu-Logo-Enabled".equalsIgnoreCase(key))      return "false";
            if ("HAS-BLOOD".equalsIgnoreCase(key))             return "1";

            /* Gameloft titles expect these JAD properties and often crash with
             * NullPointerException if getAppProperty returns null (e.g. Gangstar 2
             * calls .trim() on Blood-Censor and Cop-Censor when starting a new game). */
            if ("Blood-Censor".equalsIgnoreCase(key))          return "0";
            if ("Cop-Censor".equalsIgnoreCase(key))            return "0";
            if ("URL-TEMPLATE-GAME".equalsIgnoreCase(key))     return "";
            if ("URL-GLIVE".equalsIgnoreCase(key))             return "";
            if ("URL-PT".equalsIgnoreCase(key))                return "";
            if ("URL-PROMO".equalsIgnoreCase(key))             return "";
            if ("IGP-CATEGORIES".equalsIgnoreCase(key))        return "";
            if (key != null && (key.startsWith("URL-") || key.startsWith("url-"))) return "";
        }
        return val;
    }

    /* Returns the JAD file contents, or null. */
    private static native byte[] jad();

    /* Parses "Key: value" lines; manifest lines may continue on the next
     * line when it starts with a space. JAD files are often UTF-8. */
    private static void parse(byte[] data, boolean manifest) {
        String text;
        try {
            text = new String(data, "UTF-8");
        } catch (java.io.UnsupportedEncodingException e) {
            text = new String(data);
        }
        String key = null;
        StringBuffer value = null;
        int pos = 0;
        int n = text.length();
        while (pos <= n) {
            int end = text.indexOf('\n', pos);
            if (end < 0) {
                end = n;
            }
            String line = text.substring(pos, end);
            if (line.endsWith("\r")) {
                line = line.substring(0, line.length() - 1);
            }
            pos = end + 1;
            if (manifest && line.startsWith(" ") && key != null) {
                value.append(line.substring(1));
                continue;
            }
            if (key != null) {
                props.put(key, value.toString().trim());
                key = null;
            }
            int colon = line.indexOf(':');
            if (colon > 0) {
                key = line.substring(0, colon).trim();
                value = new StringBuffer(line.substring(colon + 1));
            }
        }
        if (key != null) {
            props.put(key, value.toString().trim());
        }
    }
}
