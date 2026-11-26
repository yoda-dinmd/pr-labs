#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace chat {

enum class MessageType {
    Join,
    JoinAck,
    JoinReject,
    Heartbeat,
    Leave,
    ChatPublic,
    ChatPrivate,
};

struct ProtocolMessage {
    MessageType type{MessageType::Join};
    std::string sender{};
    std::string text{};
    std::string payload{};
    std::string iv{};
};

struct ParseResult {
    std::optional<ProtocolMessage> message{};
    std::string error{};
};

[[nodiscard]] std::string_view message_type_to_string(MessageType type) noexcept;
[[nodiscard]] std::optional<MessageType> message_type_from_string(std::string_view value) noexcept;

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;
[[nodiscard]] std::optional<std::size_t> utf8_char_count(std::string_view text) noexcept;
[[nodiscard]] bool is_valid_username(std::string_view username) noexcept;

[[nodiscard]] std::string serialize_message(const ProtocolMessage& message);
[[nodiscard]] ParseResult deserialize_message(std::string_view raw_datagram);

} // namespace chat
