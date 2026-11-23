package javax.wireless.messaging;

public class TextMessageImpl extends AbstractMessage implements TextMessage {
    private String payload = "";

    public String getPayloadText() {
        return payload;
    }

    public void setPayloadText(String data) {
        payload = data == null ? "" : data;
    }
}
