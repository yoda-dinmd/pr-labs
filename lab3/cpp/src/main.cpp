#include "ChatPeer.h"
#include "Protocol.h"
#include "Types.h"
#include "UdpSocket.h"

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

std::atomic<bool> g_stop_requested{false};

extern "C" void handle_signal(int /*sig*/) {
    g_stop_requested.store(true);
}

std::optional<std::uint16_t> parse_port(std::string_view arg) {
    if (arg.empty()) {
        return std::nullopt;
    }
    std::uint32_t value{0};
    const auto [ptr, ec]{std::from_chars(arg.data(), arg.data() + arg.size(), value)};
    if (ec != std::errc{} || ptr != arg.data() + arg.size() || value == 0 ||
        value > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

void print_usage(std::string_view program_name) {
    std::cerr << "Usage: " << program_name
              << " <username> <port> <passphrase> [broadcast_ip]\n"
              << "  username     : unique non-empty name (1-64 chars, no whitespace or slashes)\n"
              << "  port         : shared chat UDP port (1-65535)\n"
              << "  passphrase   : shared secret for AES-256-GCM private messages\n"
              << "  broadcast_ip : optional IPv4 broadcast address (default: 255.255.255.255)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    const std::string_view program_name{argc > 0 ? argv[0] : "chat"};
    if (argc < 4 || argc > 5) {
        print_usage(program_name);
        return 1;
    }

    const std::string_view username{argv[1]};
    const auto port{parse_port(argv[2])};
    const std::string_view passphrase{argv[3]};
    const std::string_view broadcast_ip{argc == 5 ? argv[4] : chat::kDefaultBroadcastIp};

    if (!chat::is_valid_username(username)) {
        std::cerr << "Error: invalid username \"" << username << "\"\n";
        print_usage(program_name);
        return 1;
    }
    if (!port) {
        std::cerr << "Error: invalid port \"" << argv[2] << "\"\n";
        print_usage(program_name);
        return 1;
    }
    if (passphrase.empty()) {
        std::cerr << "Error: passphrase cannot be empty\n";
        print_usage(program_name);
        return 1;
    }
    if (!chat::UdpSocket::is_valid_ipv4(broadcast_ip)) {
        std::cerr << "Error: invalid IPv4 broadcast address \"" << broadcast_ip << "\"\n";
        print_usage(program_name);
        return 1;
    }

    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    chat::ChatConfig config{};
    config.username = std::string{username};
    config.chat_port = *port;
    config.passphrase = std::string{passphrase};
    config.broadcast_ip = std::string{broadcast_ip};

    chat::ChatPeer peer{std::move(config)};
    const chat::RunStatus status{peer.run(g_stop_requested)};
    return status == chat::RunStatus::ExitedNormally ? 0 : 1;
}
