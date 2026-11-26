#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace chat {

inline constexpr std::size_t kMaxMessageChars{500};
inline constexpr std::size_t kMaxUsernameChars{64};
inline constexpr std::size_t kMaxDatagramBytes{1400};

inline constexpr std::chrono::milliseconds kDefaultJoinWindow{500};
inline constexpr std::chrono::milliseconds kDefaultHeartbeatInterval{5000};
inline constexpr std::chrono::milliseconds kDefaultPeerTimeout{15000};
inline constexpr int kPollIntervalMs{100};

inline constexpr std::string_view kDefaultBroadcastIp{"255.255.255.255"};
inline constexpr std::string_view kLoopbackBroadcastIp{"127.255.255.255"};

struct Endpoint {
    std::string ip{};
    std::uint16_t port{0};

    [[nodiscard]] std::string to_string() const {
        return ip + ":" + std::to_string(port);
    }

    bool operator==(const Endpoint&) const = default;
};

struct ChatConfig {
    std::string username{};
    std::uint16_t chat_port{0};
    std::string passphrase{};
    std::string broadcast_ip{std::string{kDefaultBroadcastIp}};
    std::string log_file_path{};
    std::chrono::milliseconds join_window{kDefaultJoinWindow};
    std::chrono::milliseconds heartbeat_interval{kDefaultHeartbeatInterval};
    std::chrono::milliseconds peer_timeout{kDefaultPeerTimeout};
};

} // namespace chat
