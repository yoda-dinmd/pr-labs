#include "Protocol.h"
#include "Types.h"

#include <nlohmann/json.hpp>

#include <cstdint>

namespace chat {

std::string_view message_type_to_string(MessageType type) noexcept {
    switch (type) {
        case MessageType::Join:
            return "join";
        case MessageType::JoinAck:
            return "join_ack";
        case MessageType::JoinReject:
            return "join_reject";
        case MessageType::Heartbeat:
            return "heartbeat";
        case MessageType::Leave:
            return "leave";
        case MessageType::ChatPublic:
            return "chat_public";
        case MessageType::ChatPrivate:
            return "chat_private";
    }
    return "unknown";
}

std::optional<MessageType> message_type_from_string(std::string_view value) noexcept {
    if (value == "join") {
        return MessageType::Join;
    }
    if (value == "join_ack") {
        return MessageType::JoinAck;
    }
    if (value == "join_reject") {
        return MessageType::JoinReject;
    }
    if (value == "heartbeat") {
        return MessageType::Heartbeat;
    }
    if (value == "leave") {
        return MessageType::Leave;
    }
    if (value == "chat_public") {
        return MessageType::ChatPublic;
    }
    if (value == "chat_private") {
        return MessageType::ChatPrivate;
    }
    return std::nullopt;
}

std::optional<std::size_t> utf8_char_count(std::string_view text) noexcept {
    std::size_t count{0};
    std::size_t i{0};
    const std::size_t len{text.size()};

    while (i < len) {
        const auto b0{static_cast<std::uint8_t>(text[i])};
        if (b0 <= 0x7F) {
            i += 1;
        } else if (b0 >= 0xC2 && b0 <= 0xDF) {
            if (i + 1 >= len) {
                return std::nullopt;
            }
            const auto b1{static_cast<std::uint8_t>(text[i + 1])};
            if ((b1 & 0xC0) != 0x80) {
                return std::nullopt;
            }
            i += 2;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            if (i + 2 >= len) {
                return std::nullopt;
            }
            const auto b1{static_cast<std::uint8_t>(text[i + 1])};
            const auto b2{static_cast<std::uint8_t>(text[i + 2])};
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
                return std::nullopt;
            }
            if (b0 == 0xE0 && b1 < 0xA0) {
                return std::nullopt;
            }
            if (b0 == 0xED && b1 >= 0xA0) {
                return std::nullopt;
            }
            i += 3;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            if (i + 3 >= len) {
                return std::nullopt;
            }
            const auto b1{static_cast<std::uint8_t>(text[i + 1])};
            const auto b2{static_cast<std::uint8_t>(text[i + 2])};
            const auto b3{static_cast<std::uint8_t>(text[i + 3])};
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
                return std::nullopt;
            }
            if (b0 == 0xF0 && b1 < 0x90) {
                return std::nullopt;
            }
            if (b0 == 0xF4 && b1 > 0x8F) {
                return std::nullopt;
            }
            i += 4;
        } else {
            return std::nullopt;
        }
        ++count;
    }

    return count;
}

bool is_valid_utf8(std::string_view text) noexcept {
    return utf8_char_count(text).has_value();
}

bool is_valid_username(std::string_view username) noexcept {
    if (username.empty()) {
        return false;
    }
    const auto chars{utf8_char_count(username)};
    if (!chars || *chars > kMaxUsernameChars) {
        return false;
    }
    for (const char ch : username) {
        const auto byte{static_cast<std::uint8_t>(ch)};
        if (byte <= 0x20 || byte == 0x7F || ch == '/' || ch == '\\') {
            return false;
        }
    }
    return true;
}

std::string serialize_message(const ProtocolMessage& message) {
    nlohmann::json obj{};
    obj["type"] = message_type_to_string(message.type);
    obj["sender"] = message.sender;

    if (message.type == MessageType::ChatPublic) {
        obj["text"] = message.text;
    } else if (message.type == MessageType::ChatPrivate) {
        obj["payload"] = message.payload;
        obj["iv"] = message.iv;
    }

    return obj.dump();
}

ParseResult deserialize_message(std::string_view raw_datagram) {
    if (raw_datagram.empty()) {
        return ParseResult{std::nullopt, "empty datagram"};
    }
    if (raw_datagram.size() > kMaxDatagramBytes) {
        return ParseResult{std::nullopt, "datagram exceeds size limit"};
    }
    if (!is_valid_utf8(raw_datagram)) {
        return ParseResult{std::nullopt, "invalid UTF-8"};
    }

    nlohmann::json parsed{};
    parsed = nlohmann::json::parse(raw_datagram.begin(), raw_datagram.end(), nullptr, false);
    if (parsed.is_discarded()) {
        return ParseResult{std::nullopt, "malformed JSON"};
    }
    if (!parsed.is_object()) {
        return ParseResult{std::nullopt, "JSON root is not an object"};
    }

    if (!parsed.contains("type") || !parsed["type"].is_string()) {
        return ParseResult{std::nullopt, "missing or invalid 'type' field"};
    }
    if (!parsed.contains("sender") || !parsed["sender"].is_string()) {
        return ParseResult{std::nullopt, "missing or invalid 'sender' field"};
    }

    const auto& type_str{parsed["type"].get_ref<const std::string&>()};
    const auto msg_type{message_type_from_string(type_str)};
    if (!msg_type) {
        return ParseResult{std::nullopt, "unknown message type"};
    }

    const auto& sender{parsed["sender"].get_ref<const std::string&>()};
    if (!is_valid_username(sender)) {
        return ParseResult{std::nullopt, "invalid sender name"};
    }

    ProtocolMessage msg{};
    msg.type = *msg_type;
    msg.sender = sender;

    if (msg.type == MessageType::ChatPublic) {
        if (!parsed.contains("text") || !parsed["text"].is_string()) {
            return ParseResult{std::nullopt, "missing or invalid 'text' field"};
        }
        const auto& text{parsed["text"].get_ref<const std::string&>()};
        const auto char_count{utf8_char_count(text)};
        if (!char_count) {
            return ParseResult{std::nullopt, "invalid UTF-8 in text"};
        }
        if (*char_count > kMaxMessageChars) {
            return ParseResult{std::nullopt, "message exceeds 500 characters"};
        }
        msg.text = text;
    } else if (msg.type == MessageType::ChatPrivate) {
        if (!parsed.contains("payload") || !parsed["payload"].is_string()) {
            return ParseResult{std::nullopt, "missing or invalid 'payload' field"};
        }
        if (!parsed.contains("iv") || !parsed["iv"].is_string()) {
            return ParseResult{std::nullopt, "missing or invalid 'iv' field"};
        }
        const auto& payload{parsed["payload"].get_ref<const std::string&>()};
        const auto& iv{parsed["iv"].get_ref<const std::string&>()};
        if (payload.empty() || iv.empty()) {
            return ParseResult{std::nullopt, "empty 'payload' or 'iv' field"};
        }
        msg.payload = payload;
        msg.iv = iv;
    }

    return ParseResult{std::move(msg), {}};
}

} // namespace chat
