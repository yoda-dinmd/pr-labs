package chat;

import tools.jackson.databind.JsonNode;
import tools.jackson.databind.ObjectMapper;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;

public final class Protocol {
    public static final int MAX_MESSAGE_CHARS = 500;
    public static final int MAX_SENDER_CHARS = 64;
    public static final int MAX_DATAGRAM_BYTES = 4096;

    public static final String TYPE_JOIN = "join";
    public static final String TYPE_JOIN_ACK = "join_ack";
    public static final String TYPE_JOIN_REJECT = "join_reject";
    public static final String TYPE_HEARTBEAT = "heartbeat";
    public static final String TYPE_LEAVE = "leave";
    public static final String TYPE_CHAT_PUBLIC = "chat_public";
    public static final String TYPE_CHAT_PRIVATE = "chat_private";

    private static final ObjectMapper MAPPER = new ObjectMapper();

    private Protocol() {}

    // Outgoing wire records matching the cross-language JSON contract exactly.
    public record ControlWire(String type, String sender) {}
    public record PublicChatWire(String type, String sender, String text) {}
    public record PrivateChatWire(String type, String sender, String payload, String iv) {}

    // Parsed & validated incoming messages.
    public sealed interface Message permits Join, JoinAck, JoinReject, Heartbeat, Leave, ChatPublic, ChatPrivate {
        String type();
        String sender();
    }

    public record Join(String sender) implements Message {
        @Override public String type() { return TYPE_JOIN; }
    }

    public record JoinAck(String sender) implements Message {
        @Override public String type() { return TYPE_JOIN_ACK; }
    }

    public record JoinReject(String sender) implements Message {
        @Override public String type() { return TYPE_JOIN_REJECT; }
    }

    public record Heartbeat(String sender) implements Message {
        @Override public String type() { return TYPE_HEARTBEAT; }
    }

    public record Leave(String sender) implements Message {
        @Override public String type() { return TYPE_LEAVE; }
    }

    public record ChatPublic(String sender, String text) implements Message {
        @Override public String type() { return TYPE_CHAT_PUBLIC; }
    }

    public record ChatPrivate(String sender, String payload, String iv) implements Message {
        @Override public String type() { return TYPE_CHAT_PRIVATE; }
    }

    public sealed interface ParseResult permits ParseResult.Ok, ParseResult.Invalid {
        record Ok(Message message) implements ParseResult {}
        record Invalid(String reason) implements ParseResult {}
    }

    public static byte[] encodeJoin(String sender) {
        return serialize(new ControlWire(TYPE_JOIN, sender));
    }

    public static byte[] encodeJoinAck(String sender) {
        return serialize(new ControlWire(TYPE_JOIN_ACK, sender));
    }

    public static byte[] encodeJoinReject(String sender) {
        return serialize(new ControlWire(TYPE_JOIN_REJECT, sender));
    }

    public static byte[] encodeHeartbeat(String sender) {
        return serialize(new ControlWire(TYPE_HEARTBEAT, sender));
    }

    public static byte[] encodeLeave(String sender) {
        return serialize(new ControlWire(TYPE_LEAVE, sender));
    }

    public static byte[] encodePublicChat(String sender, String text) {
        return serialize(new PublicChatWire(TYPE_CHAT_PUBLIC, sender, text));
    }

    public static byte[] encodePrivateChat(String sender, String payloadBase64, String ivBase64) {
        return serialize(new PrivateChatWire(TYPE_CHAT_PRIVATE, sender, payloadBase64, ivBase64));
    }

    private static byte[] serialize(Object wireObject) {
        return MAPPER.writeValueAsString(wireObject).getBytes(StandardCharsets.UTF_8);
    }

    public static boolean isValidUsername(String name) {
        if (name == null || name.isEmpty() || name.length() > MAX_SENDER_CHARS) {
            return false;
        }
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (Character.isWhitespace(c) || Character.isISOControl(c)) {
                return false;
            }
        }
        return true;
    }

    public static ParseResult parse(byte[] data, int length, boolean fromBroadcastSocket) {
        if (length <= 0) {
            return new ParseResult.Invalid("empty_datagram");
        }
        if (length > MAX_DATAGRAM_BYTES) {
            return new ParseResult.Invalid("too_long (exceeds " + MAX_DATAGRAM_BYTES + " bytes)");
        }

        String json;
        try {
            json = decodeUtf8Strict(data, length);
        } catch (CharacterCodingException e) {
            return new ParseResult.Invalid("invalid_utf8");
        }

        JsonNode root;
        try {
            root = MAPPER.readTree(json);
        } catch (Exception e) {
            return new ParseResult.Invalid("invalid_json");
        }

        if (root == null || !root.isObject()) {
            return new ParseResult.Invalid("invalid_json (expected object)");
        }

        JsonNode typeNode = root.get("type");
        if (typeNode == null || !typeNode.isString()) {
            return new ParseResult.Invalid("missing_field (type)");
        }
        String type = typeNode.asString();

        JsonNode senderNode = root.get("sender");
        if (senderNode == null || !senderNode.isString()) {
            return new ParseResult.Invalid("missing_field (sender)");
        }
        String sender = senderNode.asString();
        if (!isValidUsername(sender)) {
            return new ParseResult.Invalid("invalid_sender");
        }

        return switch (type) {
            case TYPE_JOIN -> fromBroadcastSocket
                    ? new ParseResult.Ok(new Join(sender))
                    : new ParseResult.Invalid("wrong_socket (join on personal socket)");
            case TYPE_HEARTBEAT -> fromBroadcastSocket
                    ? new ParseResult.Ok(new Heartbeat(sender))
                    : new ParseResult.Invalid("wrong_socket (heartbeat on personal socket)");
            case TYPE_LEAVE -> fromBroadcastSocket
                    ? new ParseResult.Ok(new Leave(sender))
                    : new ParseResult.Invalid("wrong_socket (leave on personal socket)");
            case TYPE_CHAT_PUBLIC -> {
                if (!fromBroadcastSocket) {
                    yield new ParseResult.Invalid("wrong_socket (chat_public on personal socket)");
                }
                JsonNode textNode = root.get("text");
                if (textNode == null || !textNode.isString()) {
                    yield new ParseResult.Invalid("missing_field (text)");
                }
                String text = textNode.asString();
                if (text.length() > MAX_MESSAGE_CHARS) {
                    yield new ParseResult.Invalid("too_long (text > " + MAX_MESSAGE_CHARS + " chars)");
                }
                yield new ParseResult.Ok(new ChatPublic(sender, text));
            }
            case TYPE_JOIN_ACK -> !fromBroadcastSocket
                    ? new ParseResult.Ok(new JoinAck(sender))
                    : new ParseResult.Invalid("wrong_socket (join_ack on broadcast socket)");
            case TYPE_JOIN_REJECT -> !fromBroadcastSocket
                    ? new ParseResult.Ok(new JoinReject(sender))
                    : new ParseResult.Invalid("wrong_socket (join_reject on broadcast socket)");
            case TYPE_CHAT_PRIVATE -> {
                if (fromBroadcastSocket) {
                    yield new ParseResult.Invalid("wrong_socket (chat_private on broadcast socket)");
                }
                JsonNode payloadNode = root.get("payload");
                JsonNode ivNode = root.get("iv");
                if (payloadNode == null || !payloadNode.isString() || payloadNode.asString().isEmpty()) {
                    yield new ParseResult.Invalid("missing_field (payload)");
                }
                if (ivNode == null || !ivNode.isString() || ivNode.asString().isEmpty()) {
                    yield new ParseResult.Invalid("missing_field (iv)");
                }
                yield new ParseResult.Ok(new ChatPrivate(sender, payloadNode.asString(), ivNode.asString()));
            }
            default -> new ParseResult.Invalid("unknown_type (" + sanitizeForLog(type) + ")");
        };
    }

    public static String decodeUtf8Strict(byte[] bytes, int length) throws CharacterCodingException {
        return StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes, 0, length))
                .toString();
    }

    private static String sanitizeForLog(String value) {
        String clean = value.replaceAll("[\\r\\n\\t]", " ");
        return clean.length() > 32 ? clean.substring(0, 32) + "..." : clean;
    }
}
