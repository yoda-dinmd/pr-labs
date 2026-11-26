#pragma once

#include "ConsoleUI.h"
#include "Crypto.h"
#include "Logger.h"
#include "PeerRegistry.h"
#include "Protocol.h"
#include "Types.h"
#include "UdpSocket.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace chat {

enum class RunStatus {
    ExitedNormally,
    NameTaken,
    InitError,
};

class ChatPeer {
public:
    ChatPeer(ChatConfig config,
             int input_fd = 0,
             std::ostream& out = std::cout,
             bool enable_raw_mode = true);

    bool initialize();
    RunStatus join_network();
    void run_event_loop(const std::atomic<bool>& stop_flag);
    void send_leave();

    // High-level entry point: initialize -> join -> event loop -> leave.
    RunStatus run(const std::atomic<bool>& stop_flag);

    // Public helpers for testing and programmatic interaction.
    void process_user_line(std::string_view line, bool& should_quit);
    void poll_once(int timeout_ms, bool& should_quit, bool poll_stdin = true);
    void tick_maintenance();

    [[nodiscard]] std::uint16_t personal_port() const noexcept;
    [[nodiscard]] const std::string& log_path() const noexcept;
    [[nodiscard]] const PeerRegistry& peers() const noexcept;
    [[nodiscard]] const std::string& effective_broadcast_ip() const noexcept;

private:
    bool send_broadcast(const ProtocolMessage& message);
    bool send_unicast(const ProtocolMessage& message, const Endpoint& destination);

    void handle_broadcast_socket(bool joining, bool& rejected);
    void handle_personal_socket(bool joining, bool& rejected);

    void handle_public_command(std::string_view text);
    void handle_private_command(std::string_view args);
    void handle_users_command();

    [[nodiscard]] bool is_self_endpoint(const Endpoint& endpoint) const;
    [[nodiscard]] std::string make_default_log_path() const;

    ChatConfig config_{};
    Crypto crypto_;
    Logger logger_{};
    UdpSocket listener_sock_{};
    UdpSocket personal_sock_{};
    PeerRegistry peers_{};
    ConsoleUI ui_;

    std::uint16_t personal_port_{0};
    std::string effective_broadcast_ip_{};
    bool using_default_broadcast_{false};
    std::unordered_set<std::string> local_ips_{};
    std::chrono::steady_clock::time_point last_heartbeat_sent_{};
    bool initialized_{false};
    bool joined_{false};
};

} // namespace chat
