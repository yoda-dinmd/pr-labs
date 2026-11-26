#include "ChatPeer.h"

#include <poll.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

namespace chat {

ChatPeer::ChatPeer(ChatConfig config, int input_fd, std::ostream& out, bool enable_raw_mode)
    : config_{std::move(config)},
      crypto_{config_.passphrase},
      ui_{input_fd, out, enable_raw_mode} {}

bool ChatPeer::initialize() {
    if (!is_valid_username(config_.username)) {
        ui_.print_line("Error: invalid username (must be 1-64 non-whitespace UTF-8 characters without slashes)");
        return false;
    }
    if (config_.chat_port == 0) {
        ui_.print_line("Error: port must be between 1 and 65535");
        return false;
    }
    if (config_.passphrase.empty()) {
        ui_.print_line("Error: passphrase cannot be empty");
        return false;
    }
    if (!UdpSocket::is_valid_ipv4(config_.broadcast_ip)) {
        ui_.print_line("Error: invalid broadcast IPv4 address: " + config_.broadcast_ip);
        return false;
    }

    // Socket 1: Listener for broadcast messages on the shared chat port.
    if (!listener_sock_.open() || !listener_sock_.enable_reuse_addr()) {
        ui_.print_line("Error: failed to create listener socket");
        return false;
    }
    (void)listener_sock_.enable_reuse_port();
    if (!listener_sock_.bind_any(config_.chat_port)) {
        ui_.print_line("Error: failed to bind listener socket to port " + std::to_string(config_.chat_port) +
                       " (" + std::strerror(errno) + ")");
        return false;
    }

    // Socket 2: Personal ephemeral socket for all outgoing datagrams and incoming unicasts.
    if (!personal_sock_.open() || !personal_sock_.enable_broadcast() || !personal_sock_.bind_any(0)) {
        ui_.print_line("Error: failed to create/bind personal socket");
        return false;
    }

    const auto port{personal_sock_.local_port()};
    if (!port) {
        ui_.print_line("Error: failed to determine personal socket port");
        return false;
    }
    personal_port_ = *port;
    local_ips_ = UdpSocket::local_ipv4_addresses();
    effective_broadcast_ip_ = config_.broadcast_ip;
    using_default_broadcast_ = (config_.broadcast_ip == kDefaultBroadcastIp);

    const std::string chosen_log_path{
        config_.log_file_path.empty() ? make_default_log_path() : config_.log_file_path
    };
    if (!logger_.open(chosen_log_path)) {
        ui_.print_line("Error: failed to open log file: " + chosen_log_path);
        return false;
    }

    initialized_ = true;
    return true;
}

RunStatus ChatPeer::join_network() {
    if (!initialized_) {
        return RunStatus::InitError;
    }

    const ProtocolMessage join_msg{MessageType::Join, config_.username, {}, {}, {}};
    if (!send_broadcast(join_msg)) {
        ui_.print_line("Error: failed to broadcast join message");
        return RunStatus::InitError;
    }

    const auto deadline{std::chrono::steady_clock::now() + config_.join_window};
    bool rejected{false};

    while (!rejected) {
        const auto now{std::chrono::steady_clock::now()};
        if (now >= deadline) {
            break;
        }
        const auto remaining_ms{
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()
        };
        const int wait_ms{static_cast<int>(std::max<std::chrono::milliseconds::rep>(1, remaining_ms))};

        std::array<pollfd, 2> pfds{{
            pollfd{listener_sock_.fd(), POLLIN, 0},
            pollfd{personal_sock_.fd(), POLLIN, 0},
        }};

        const int ready{::poll(pfds.data(), pfds.size(), wait_ms)};
        if (ready <= 0) {
            continue;
        }
        if ((pfds[0].revents & POLLIN) != 0) {
            handle_broadcast_socket(true, rejected);
        }
        if ((pfds[1].revents & POLLIN) != 0) {
            handle_personal_socket(true, rejected);
        }
    }

    if (rejected) {
        ui_.print_line("Error: username \"" + config_.username + "\" is already in use.");
        return RunStatus::NameTaken;
    }

    joined_ = true;
    last_heartbeat_sent_ = std::chrono::steady_clock::now();
    ui_.print_line(
        "Joined chat as " + config_.username +
        " (port " + std::to_string(config_.chat_port) +
        ", personal port " + std::to_string(personal_port_) +
        ", log: " + logger_.path() + ")"
    );
    ui_.show_prompt();
    return RunStatus::ExitedNormally;
}

void ChatPeer::run_event_loop(const std::atomic<bool>& stop_flag) {
    bool should_quit{false};
    while (!should_quit && !stop_flag.load()) {
        poll_once(kPollIntervalMs, should_quit, true);
        tick_maintenance();
    }
    ui_.clear_prompt();
}

void ChatPeer::send_leave() {
    if (!initialized_ || !joined_) {
        return;
    }
    const ProtocolMessage leave_msg{MessageType::Leave, config_.username, {}, {}, {}};
    (void)send_broadcast(leave_msg);
    joined_ = false;
}

RunStatus ChatPeer::run(const std::atomic<bool>& stop_flag) {
    if (!initialize()) {
        return RunStatus::InitError;
    }
    const RunStatus join_status{join_network()};
    if (join_status != RunStatus::ExitedNormally) {
        return join_status;
    }
    run_event_loop(stop_flag);
    send_leave();
    return RunStatus::ExitedNormally;
}

void ChatPeer::poll_once(int timeout_ms, bool& should_quit, bool poll_stdin) {
    std::array<pollfd, 3> pfds{{
        pollfd{listener_sock_.fd(), POLLIN, 0},
        pollfd{personal_sock_.fd(), POLLIN, 0},
        pollfd{poll_stdin ? ui_.input_fd() : -1, POLLIN, 0},
    }};
    const nfds_t count{(poll_stdin && ui_.input_fd() >= 0) ? 3U : 2U};

    const int ready{::poll(pfds.data(), count, timeout_ms)};
    if (ready <= 0) {
        return;
    }

    bool ignored_reject{false};
    if ((pfds[0].revents & POLLIN) != 0) {
        handle_broadcast_socket(false, ignored_reject);
    }
    if ((pfds[1].revents & POLLIN) != 0) {
        handle_personal_socket(false, ignored_reject);
    }
    if (count == 3U && (pfds[2].revents & (POLLIN | POLLHUP)) != 0) {
        const auto input{ui_.read_available()};
        for (const auto& line : input.lines) {
            process_user_line(line, should_quit);
            if (should_quit) {
                break;
            }
        }
        if (input.eof) {
            should_quit = true;
        }
    }
}

void ChatPeer::tick_maintenance() {
    if (!joined_) {
        return;
    }
    const auto now{std::chrono::steady_clock::now()};
    if (now - last_heartbeat_sent_ >= config_.heartbeat_interval) {
        const ProtocolMessage hb{MessageType::Heartbeat, config_.username, {}, {}, {}};
        (void)send_broadcast(hb);
        last_heartbeat_sent_ = now;
    }

    const auto expired{peers_.remove_expired(now, config_.peer_timeout)};
    for (const auto& name : expired) {
        ui_.print_line("* " + name + " left the chat");
    }
}

void ChatPeer::process_user_line(std::string_view line, bool& should_quit) {
    if (line.empty()) {
        return;
    }

    if (line.front() != '/') {
        handle_public_command(line);
        return;
    }

    if (line == "/quit") {
        should_quit = true;
        return;
    }
    if (line == "/users") {
        handle_users_command();
        return;
    }
    if (line == "/msg" || line.starts_with("/msg ")) {
        const std::string_view args{line.size() > 4 ? line.substr(5) : std::string_view{}};
        handle_private_command(args);
        return;
    }

    ui_.print_line("Unknown command. Available commands: /msg <name> <text>, /users, /quit");
}

std::uint16_t ChatPeer::personal_port() const noexcept {
    return personal_port_;
}

const std::string& ChatPeer::log_path() const noexcept {
    return logger_.path();
}

const PeerRegistry& ChatPeer::peers() const noexcept {
    return peers_;
}

const std::string& ChatPeer::effective_broadcast_ip() const noexcept {
    return effective_broadcast_ip_;
}

bool ChatPeer::send_broadcast(const ProtocolMessage& message) {
    const std::string payload{serialize_message(message)};
    if (payload.size() > kMaxDatagramBytes) {
        return false;
    }

    Endpoint dest{effective_broadcast_ip_, config_.chat_port};
    int send_errno{0};
    bool ok{personal_sock_.send_to(payload, dest, &send_errno)};

    if (!ok && send_errno == ENETUNREACH && using_default_broadcast_ &&
        effective_broadcast_ip_ != kLoopbackBroadcastIp) {
        effective_broadcast_ip_ = std::string{kLoopbackBroadcastIp};
        dest.ip = effective_broadcast_ip_;
        ok = personal_sock_.send_to(payload, dest, &send_errno);
    }

    if (ok) {
        logger_.log_datagram(
            DatagramDirection::Send,
            dest,
            message_type_to_string(message.type),
            payload.size()
        );
    }
    return ok;
}

bool ChatPeer::send_unicast(const ProtocolMessage& message, const Endpoint& destination) {
    const std::string payload{serialize_message(message)};
    if (payload.size() > kMaxDatagramBytes) {
        return false;
    }

    const bool ok{personal_sock_.send_to(payload, destination)};
    if (ok) {
        logger_.log_datagram(
            DatagramDirection::Send,
            destination,
            message_type_to_string(message.type),
            payload.size()
        );
    }
    return ok;
}

void ChatPeer::handle_broadcast_socket(bool joining, bool& rejected) {
    (void)rejected;
    const auto dg{listener_sock_.recv_from(kMaxDatagramBytes)};
    if (!dg) {
        return;
    }

    if (dg->truncated || dg->actual_size == 0) {
        logger_.log_datagram(
            DatagramDirection::Drop,
            dg->sender,
            dg->truncated ? "invalid (datagram too long)" : "invalid (empty datagram)",
            dg->actual_size
        );
        return;
    }

    const auto parsed{deserialize_message(dg->data)};
    if (!parsed.message) {
        logger_.log_datagram(
            DatagramDirection::Drop,
            dg->sender,
            "invalid (" + parsed.error + ")",
            dg->actual_size
        );
        return;
    }

    const auto& msg{*parsed.message};
    const bool is_broadcast_type{
        msg.type == MessageType::Join ||
        msg.type == MessageType::Heartbeat ||
        msg.type == MessageType::Leave ||
        msg.type == MessageType::ChatPublic
    };
    if (!is_broadcast_type) {
        logger_.log_datagram(
            DatagramDirection::Drop,
            dg->sender,
            "invalid (unicast type on broadcast socket)",
            dg->actual_size
        );
        return;
    }

    logger_.log_datagram(
        DatagramDirection::Recv,
        dg->sender,
        message_type_to_string(msg.type),
        dg->actual_size
    );

    if (is_self_endpoint(dg->sender)) {
        return;
    }

    const auto now{std::chrono::steady_clock::now()};
    switch (msg.type) {
        case MessageType::Join: {
            if (msg.sender == config_.username) {
                const ProtocolMessage reject_msg{MessageType::JoinReject, config_.username, {}, {}, {}};
                (void)send_unicast(reject_msg, dg->sender);
                return;
            }
            const bool added{peers_.upsert(msg.sender, dg->sender, now, false)};
            const ProtocolMessage ack_msg{MessageType::JoinAck, config_.username, {}, {}, {}};
            (void)send_unicast(ack_msg, dg->sender);
            if (added && !joining) {
                ui_.print_line("* " + msg.sender + " joined the chat");
            }
            break;
        }
        case MessageType::Heartbeat: {
            if (msg.sender == config_.username) {
                return;
            }
            const bool added{peers_.upsert(msg.sender, dg->sender, now, true)};
            if (added && !joining) {
                ui_.print_line("* " + msg.sender + " joined the chat");
            }
            break;
        }
        case MessageType::Leave: {
            if (msg.sender == config_.username) {
                return;
            }
            if (peers_.remove(msg.sender) && !joining) {
                ui_.print_line("* " + msg.sender + " left the chat");
            }
            break;
        }
        case MessageType::ChatPublic: {
            if (msg.sender != config_.username) {
                const bool added{peers_.upsert(msg.sender, dg->sender, now, true)};
                if (added && !joining) {
                    ui_.print_line("* " + msg.sender + " joined the chat");
                }
            }
            ui_.print_line("[general] " + msg.sender + ": " + msg.text);
            break;
        }
        default:
            break;
    }
}

void ChatPeer::handle_personal_socket(bool joining, bool& rejected) {
    const auto dg{personal_sock_.recv_from(kMaxDatagramBytes)};
    if (!dg) {
        return;
    }

    if (dg->truncated || dg->actual_size == 0) {
        logger_.log_datagram(
            DatagramDirection::Drop,
            dg->sender,
            dg->truncated ? "invalid (datagram too long)" : "invalid (empty datagram)",
            dg->actual_size
        );
        return;
    }

    const auto parsed{deserialize_message(dg->data)};
    if (!parsed.message) {
        logger_.log_datagram(
            DatagramDirection::Drop,
            dg->sender,
            "invalid (" + parsed.error + ")",
            dg->actual_size
        );
        return;
    }

    const auto& msg{*parsed.message};
    const auto now{std::chrono::steady_clock::now()};

    switch (msg.type) {
        case MessageType::JoinAck: {
            logger_.log_datagram(DatagramDirection::Recv, dg->sender, "join_ack", dg->actual_size);
            if (!is_self_endpoint(dg->sender) && msg.sender != config_.username) {
                (void)peers_.upsert(msg.sender, dg->sender, now, true);
            }
            break;
        }
        case MessageType::JoinReject: {
            logger_.log_datagram(DatagramDirection::Recv, dg->sender, "join_reject", dg->actual_size);
            if (joining && !is_self_endpoint(dg->sender)) {
                rejected = true;
            }
            break;
        }
        case MessageType::ChatPrivate: {
            const auto plaintext{crypto_.decrypt(msg.payload, msg.iv)};
            if (!plaintext) {
                logger_.log_datagram(
                    DatagramDirection::Drop,
                    dg->sender,
                    "invalid (decryption failed)",
                    dg->actual_size
                );
                return;
            }

            const auto char_count{utf8_char_count(*plaintext)};
            if (!char_count || *char_count > kMaxMessageChars) {
                logger_.log_datagram(
                    DatagramDirection::Drop,
                    dg->sender,
                    "invalid (decrypted message too long or invalid UTF-8)",
                    dg->actual_size
                );
                return;
            }

            logger_.log_datagram(DatagramDirection::Recv, dg->sender, "chat_private", dg->actual_size);
            if (msg.sender != config_.username && peers_.contains(msg.sender)) {
                (void)peers_.upsert(msg.sender, dg->sender, now, true);
            }
            ui_.print_line("[private] " + msg.sender + ": " + *plaintext);
            break;
        }
        default: {
            logger_.log_datagram(
                DatagramDirection::Drop,
                dg->sender,
                "invalid (unexpected message type on personal socket)",
                dg->actual_size
            );
            break;
        }
    }
}

void ChatPeer::handle_public_command(std::string_view text) {
    const auto char_count{utf8_char_count(text)};
    if (!char_count) {
        ui_.print_line("Error: message contains invalid UTF-8");
        return;
    }
    if (*char_count > kMaxMessageChars) {
        ui_.print_line("Error: message exceeds " + std::to_string(kMaxMessageChars) + " characters");
        return;
    }

    const ProtocolMessage msg{MessageType::ChatPublic, config_.username, std::string{text}, {}, {}};
    if (!send_broadcast(msg)) {
        ui_.print_line("Error: failed to send general message (datagram too large or network error)");
        return;
    }
    ui_.print_line("[general] " + config_.username + ": " + std::string{text});
}

void ChatPeer::handle_private_command(std::string_view args) {
    while (!args.empty() && args.front() == ' ') {
        args.remove_prefix(1);
    }
    const std::size_t sep{args.find(' ')};
    if (sep == std::string_view::npos) {
        ui_.print_line("Usage: /msg <name> <text>");
        return;
    }

    const std::string_view target{args.substr(0, sep)};
    const std::string_view text{args.substr(sep + 1)};
    if (target.empty() || text.find_first_not_of(" \t") == std::string_view::npos) {
        ui_.print_line("Usage: /msg <name> <text>");
        return;
    }

    const auto char_count{utf8_char_count(text)};
    if (!char_count) {
        ui_.print_line("Error: message contains invalid UTF-8");
        return;
    }
    if (*char_count > kMaxMessageChars) {
        ui_.print_line("Error: message exceeds " + std::to_string(kMaxMessageChars) + " characters");
        return;
    }

    Endpoint destination{};
    if (target == config_.username) {
        destination = Endpoint{"127.0.0.1", personal_port_};
    } else {
        const auto found{peers_.find_endpoint(target)};
        if (!found) {
            ui_.print_line("Error: user \"" + std::string{target} + "\" is not online");
            return;
        }
        destination = *found;
    }

    const auto encrypted{crypto_.encrypt(text)};
    if (!encrypted) {
        ui_.print_line("Error: failed to encrypt private message");
        return;
    }

    const ProtocolMessage msg{
        MessageType::ChatPrivate,
        config_.username,
        {},
        encrypted->payload_b64,
        encrypted->iv_b64
    };
    if (!send_unicast(msg, destination)) {
        ui_.print_line("Error: failed to send private message (datagram too large or network error)");
        return;
    }
}

void ChatPeer::handle_users_command() {
    const auto online{peers_.list_peers()};
    ui_.print_line("Online users (" + std::to_string(online.size() + 1) + "):");
    ui_.print_line("  " + config_.username + " (you) [127.0.0.1:" + std::to_string(personal_port_) + "]");
    for (const auto& [name, endpoint] : online) {
        ui_.print_line("  " + name + " [" + endpoint.to_string() + "]");
    }
}

bool ChatPeer::is_self_endpoint(const Endpoint& endpoint) const {
    return endpoint.port == personal_port_ && local_ips_.contains(endpoint.ip);
}

std::string ChatPeer::make_default_log_path() const {
    return "chat_" + config_.username + "_" + std::to_string(personal_port_) + ".log";
}

} // namespace chat
