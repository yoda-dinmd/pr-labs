#include <ThreadPool.h>
#include <protocol.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr std::uint16_t DEFAULT_PORT = 8000;
constexpr auto HEARTBEAT_TIMEOUT = std::chrono::seconds(3);
constexpr int POLL_SLICE_MS = 500;
constexpr std::size_t RECV_CHUNK_BYTES = 4096;
constexpr std::size_t MAX_LINE_BYTES = 8192;

constexpr int COFFEE_PREP_SECONDS = 4;
constexpr int TEA_PREP_SECONDS = 3;
constexpr int CROISSANT_PREP_SECONDS = 2;
constexpr int LOW_STOCK_THRESHOLD = 5;
constexpr int INITIAL_STOCK_MIN = 10;
constexpr int INITIAL_STOCK_MAX = 50;

volatile std::sig_atomic_t g_shutdown_requested = 0;

extern "C" void handle_shutdown_signal(int /*sig*/) {
    g_shutdown_requested = 1;
}

int compute_prep_time(const protocol::Items& items) noexcept {
    return items.coffee * COFFEE_PREP_SECONDS +
           items.tea * TEA_PREP_SECONDS +
           items.croissant * CROISSANT_PREP_SECONDS;
}

std::string format_items(const protocol::Items& items) {
    return "coffee=" + std::to_string(items.coffee) +
           ", tea=" + std::to_string(items.tea) +
           ", croissant=" + std::to_string(items.croissant);
}

std::optional<std::uint16_t> parse_port(int argc, char* argv[]) {
    if (argc < 2) {
        return DEFAULT_PORT;
    }
    if (argc > 2) {
        return std::nullopt;
    }
    const std::string_view arg(argv[1]);
    std::uint32_t value = 0;
    const auto* begin = arg.data();
    const auto* end = arg.data() + arg.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value == 0 ||
        value > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

bool send_all(int fd, std::string_view data) {
    std::size_t total_sent = 0;
    while (total_sent < data.size()) {
        const ssize_t sent = send(
            fd,
            data.data() + total_sent,
            data.size() - total_sent,
            MSG_NOSIGNAL
        );
        if (sent > 0) {
            total_sent += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

struct OrderRecord {
    int order_id{};
    std::string client_ip;
    std::uint16_t client_port{};
    protocol::Items items{};
    std::stop_source stop_source{};
    // Ensures reserved products are either completed or restored to stock at most once.
    std::atomic<bool> finalized{false};
};

struct ClientSession {
    int fd{-1};
    std::string ip;
    std::uint16_t port{};
    std::string endpoint;

    std::atomic<bool> is_provider{false};

    std::mutex send_mutex;
    bool send_closed{false};

    std::mutex orders_mutex;
    bool disconnected{false};
    std::vector<std::shared_ptr<OrderRecord>> active_orders;

    ClientSession(int socket_fd, std::string client_ip, std::uint16_t client_port)
        : fd(socket_fd),
          ip(std::move(client_ip)),
          port(client_port),
          endpoint(ip + ":" + std::to_string(port)) {}

    template <typename MessageT>
    bool send_json(const MessageT& msg) {
        std::string payload = protocol::json(msg).dump() + "\n";
        std::lock_guard lock{send_mutex};
        if (send_closed || fd < 0) {
            return false;
        }
        return send_all(fd, payload);
    }

    void close_socket() {
        std::lock_guard lock{send_mutex};
        send_closed = true;
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
            fd = -1;
        }
    }
};

class CoffeeShopServer {
public:
    explicit CoffeeShopServer(std::size_t worker_count)
        : pool_(worker_count),
          stock_(generate_initial_stock()) {}

    int run(std::uint16_t port) {
        const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            std::perror("socket");
            return 1;
        }

        const int reuse = 1;
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            std::perror("setsockopt(SO_REUSEADDR)");
            close(listen_fd);
            return 1;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(listen_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
            std::perror("bind");
            close(listen_fd);
            return 1;
        }

        if (listen(listen_fd, SOMAXCONN) < 0) {
            std::perror("listen");
            close(listen_fd);
            return 1;
        }

        {
            std::lock_guard lock{stock_mutex_};
            log("server", "Listening on port " + std::to_string(port) +
                " | Initial stock: " + format_items(stock_));
        }

        struct ClientWorker {
            std::shared_ptr<std::atomic<bool>> done{std::make_shared<std::atomic<bool>>(false)};
            std::jthread thread;
        };
        std::vector<ClientWorker> client_workers;

        while (!g_shutdown_requested) {
            std::erase_if(client_workers, [](const ClientWorker& w) {
                return w.done->load();
            });

            pollfd pfd{};
            pfd.fd = listen_fd;
            pfd.events = POLLIN;
            const int ready = poll(&pfd, 1, POLL_SLICE_MS);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::perror("poll(listen)");
                break;
            }
            if (ready == 0 || (pfd.revents & POLLIN) == 0) {
                continue;
            }

            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            const int client_fd = accept(
                listen_fd,
                reinterpret_cast<sockaddr*>(&client_addr),
                &client_len
            );
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::perror("accept");
                continue;
            }

            char ip_buf[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf)) == nullptr) {
                std::snprintf(ip_buf, sizeof(ip_buf), "unknown");
            }
            const std::uint16_t client_port = ntohs(client_addr.sin_port);
            auto session = std::make_shared<ClientSession>(client_fd, std::string(ip_buf), client_port);

            {
                std::lock_guard lock{sessions_mutex_};
                sessions_.push_back(session);
            }

            log(session->endpoint, "Client connected");

            auto done_flag = std::make_shared<std::atomic<bool>>(false);
            client_workers.push_back(ClientWorker{
                done_flag,
                std::jthread([this, session, done_flag]() {
                    handle_client(session);
                    done_flag->store(true);
                })
            });
        }

        close(listen_fd);
        // Stop all active sessions on server shutdown so client and worker threads wake promptly.
        std::vector<std::shared_ptr<ClientSession>> remaining_sessions;
        {
            std::lock_guard lock{sessions_mutex_};
            for (const auto& weak : sessions_) {
                if (auto s = weak.lock()) {
                    remaining_sessions.push_back(std::move(s));
                }
            }
            sessions_.clear();
        }
        for (const auto& s : remaining_sessions) {
            disconnect_session(s, "Server shutting down");
        }
        client_workers.clear();
        log("server", "Server stopped");
        return 0;
    }

private:
    static protocol::Items generate_initial_stock() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(INITIAL_STOCK_MIN, INITIAL_STOCK_MAX);
        return protocol::Items{dist(gen), dist(gen), dist(gen)};
    }

    void log(std::string_view endpoint, std::string_view message) {
        std::lock_guard lock{log_mutex_};
        std::cout << "[" << endpoint << "] " << message << "\n";
    }

    void send_error(const std::shared_ptr<ClientSession>& session,
                    protocol::ErrorCode code,
                    std::string_view message) {
        log(session->endpoint,
            "ERROR (" + std::string(protocol::to_string(code)) + "): " + std::string(message));
        const protocol::ErrorMessage err{
            protocol::MessageType::ERROR,
            code,
            std::string(message)
        };
        session->send_json(err);
    }

    std::vector<protocol::LowStockMessage> collect_low_stock_all_locked() const {
        std::vector<protocol::LowStockMessage> alerts;
        if (stock_.coffee < LOW_STOCK_THRESHOLD) {
            alerts.push_back({protocol::MessageType::LOW_STOCK, protocol::Product::coffee, stock_.coffee});
        }
        if (stock_.tea < LOW_STOCK_THRESHOLD) {
            alerts.push_back({protocol::MessageType::LOW_STOCK, protocol::Product::tea, stock_.tea});
        }
        if (stock_.croissant < LOW_STOCK_THRESHOLD) {
            alerts.push_back({protocol::MessageType::LOW_STOCK, protocol::Product::croissant, stock_.croissant});
        }
        return alerts;
    }

    void notify_provider(const std::vector<protocol::LowStockMessage>& alerts) {
        if (alerts.empty()) {
            return;
        }
        std::shared_ptr<ClientSession> provider;
        {
            std::lock_guard lock{provider_mutex_};
            provider = active_provider_;
        }
        if (!provider) {
            return;
        }
        for (const auto& alert : alerts) {
            log(provider->endpoint,
                "LOW_STOCK alert: " + std::string(protocol::to_string(alert.product)) +
                " remaining=" + std::to_string(alert.remaining));
            provider->send_json(alert);
        }
    }

    void restore_stock(const protocol::Items& items) {
        std::lock_guard lock{stock_mutex_};
        stock_.coffee += items.coffee;
        stock_.tea += items.tea;
        stock_.croissant += items.croissant;
    }

    void handle_menu_request(const std::shared_ptr<ClientSession>& session) {
        protocol::Items snapshot{};
        {
            std::lock_guard lock{stock_mutex_};
            snapshot = stock_;
        }
        session->send_json(protocol::ItemsMessage{
            protocol::MessageType::MENU_RESPONSE,
            snapshot
        });
    }

    void handle_provider_identify(const std::shared_ptr<ClientSession>& session) {
        const bool first_time = !session->is_provider.exchange(true);
        {
            std::lock_guard lock{provider_mutex_};
            active_provider_ = session;
        }
        if (first_time) {
            log(session->endpoint, "Identified as PROVIDER");
        }

        std::vector<protocol::LowStockMessage> alerts;
        {
            std::lock_guard lock{stock_mutex_};
            alerts = collect_low_stock_all_locked();
        }
        for (const auto& alert : alerts) {
            log(session->endpoint,
                "LOW_STOCK alert on connect: " +
                std::string(protocol::to_string(alert.product)) +
                " remaining=" + std::to_string(alert.remaining));
            session->send_json(alert);
        }
    }

    void handle_delivery(const std::shared_ptr<ClientSession>& session, const protocol::Items& items) {
        if (!session->is_provider.load()) {
            send_error(session, protocol::ErrorCode::UNAUTHORIZED_OPERATION,
                "Only an identified provider connection can send deliveries");
            return;
        }

        protocol::Items updated{};
        bool overflow = false;
        {
            std::lock_guard lock{stock_mutex_};
            constexpr int max_int = std::numeric_limits<int>::max();
            if (items.coffee > max_int - stock_.coffee ||
                items.tea > max_int - stock_.tea ||
                items.croissant > max_int - stock_.croissant) {
                overflow = true;
            } else {
                stock_.coffee += items.coffee;
                stock_.tea += items.tea;
                stock_.croissant += items.croissant;
                updated = stock_;
            }
        }

        if (overflow) {
            send_error(session, protocol::ErrorCode::INVALID_DELIVERY,
                "Delivery exceeds maximum stock capacity");
            return;
        }

        log(session->endpoint,
            "DELIVERY received (" + format_items(items) + ") | Stock: " + format_items(updated));
    }

    void handle_order_request(const std::shared_ptr<ClientSession>& session, const protocol::Items& items) {
        int order_id = 0;
        std::string refusal_reason;
        std::vector<protocol::LowStockMessage> alerts;

        {
            std::lock_guard lock{stock_mutex_};
            if (stock_.coffee < items.coffee) {
                refusal_reason = "Insufficient coffee (requested " + std::to_string(items.coffee) +
                                 ", available " + std::to_string(stock_.coffee) + ")";
            } else if (stock_.tea < items.tea) {
                refusal_reason = "Insufficient tea (requested " + std::to_string(items.tea) +
                                 ", available " + std::to_string(stock_.tea) + ")";
            } else if (stock_.croissant < items.croissant) {
                refusal_reason = "Insufficient croissant (requested " + std::to_string(items.croissant) +
                                 ", available " + std::to_string(stock_.croissant) + ")";
            } else {
                stock_.coffee -= items.coffee;
                stock_.tea -= items.tea;
                stock_.croissant -= items.croissant;
                order_id = next_order_id_++;

                if (items.coffee > 0 && stock_.coffee < LOW_STOCK_THRESHOLD) {
                    alerts.push_back({protocol::MessageType::LOW_STOCK, protocol::Product::coffee, stock_.coffee});
                }
                if (items.tea > 0 && stock_.tea < LOW_STOCK_THRESHOLD) {
                    alerts.push_back({protocol::MessageType::LOW_STOCK, protocol::Product::tea, stock_.tea});
                }
                if (items.croissant > 0 && stock_.croissant < LOW_STOCK_THRESHOLD) {
                    alerts.push_back({protocol::MessageType::LOW_STOCK, protocol::Product::croissant, stock_.croissant});
                }
            }
        }

        if (!refusal_reason.empty()) {
            log(session->endpoint, "ORDER REFUSED: " + refusal_reason);
            session->send_json(protocol::RefusedMessage{
                protocol::MessageType::ORDER_REFUSED,
                refusal_reason
            });
            return;
        }

        const int prep_time = compute_prep_time(items);
        auto order = std::make_shared<OrderRecord>();
        order->order_id = order_id;
        order->client_ip = session->ip;
        order->client_port = session->port;
        order->items = items;

        bool session_alive = true;
        {
            std::lock_guard lock{session->orders_mutex};
            if (session->disconnected) {
                session_alive = false;
            } else {
                session->active_orders.push_back(order);
            }
        }
        if (!session_alive) {
            restore_stock(items);
            return;
        }

        log(session->endpoint,
            "ORDER ACCEPTED #" + std::to_string(order_id) +
            " (" + format_items(items) + "), eta=" + std::to_string(prep_time) + "s");

        session->send_json(protocol::AcceptedMessage{
            protocol::MessageType::ORDER_ACCEPTED,
            order_id,
            prep_time
        });

        notify_provider(alerts);

        auto stop_tok = order->stop_source.get_token();
        (void)pool_.submitTask([this, session, order, prep_time, stop_tok]() {
            if (stop_tok.stop_requested()) {
                if (!order->finalized.exchange(true)) {
                    restore_stock(order->items);
                }
                return;
            }

            std::mutex wait_mutex;
            std::unique_lock wait_lock{wait_mutex};
            std::condition_variable_any cv;
            const bool stopped = cv.wait_for(
                wait_lock,
                stop_tok,
                std::chrono::seconds(prep_time),
                [&stop_tok] { return stop_tok.stop_requested(); }
            );

            if (stopped || stop_tok.stop_requested()) {
                if (!order->finalized.exchange(true)) {
                    restore_stock(order->items);
                    log(order->client_ip + ":" + std::to_string(order->client_port),
                        "ORDER CANCELLED #" + std::to_string(order->order_id) +
                        " mid-preparation; stock restored (" + format_items(order->items) + ")");
                }
                return;
            }

            if (order->finalized.exchange(true)) {
                return;
            }

            {
                std::lock_guard lock{session->orders_mutex};
                std::erase(session->active_orders, order);
            }

            log(order->client_ip + ":" + std::to_string(order->client_port),
                "ORDER READY #" + std::to_string(order->order_id));

            session->send_json(protocol::OrderIdMessage{
                protocol::MessageType::ORDER_READY,
                order->order_id
            });
        });
    }

    void dispatch_line(const std::shared_ptr<ClientSession>& session,
                       const std::string& line,
                       std::chrono::steady_clock::time_point& last_heartbeat) {
        try {
            const auto j = protocol::json::parse(line);
            if (protocol::parse_message_type(j) == protocol::MessageType::DELIVERY &&
                !session->is_provider.load()) {
                send_error(session, protocol::ErrorCode::UNAUTHORIZED_OPERATION,
                    "Only an identified provider connection can send deliveries");
                return;
            }
            const auto msg = protocol::deserialize_message(j);

            std::visit([&](const auto& m) {
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, protocol::EmptyMessage>) {
                    switch (m.type) {
                        case protocol::MessageType::MENU_REQUEST:
                            handle_menu_request(session);
                            break;
                        case protocol::MessageType::PROVIDER_IDENTIFY:
                            handle_provider_identify(session);
                            break;
                        case protocol::MessageType::HEARTBEAT:
                            last_heartbeat = std::chrono::steady_clock::now();
                            break;
                        default:
                            send_error(session, protocol::ErrorCode::INVALID_MESSAGE,
                                "Unexpected message type");
                            break;
                    }
                } else if constexpr (std::is_same_v<T, protocol::ItemsMessage>) {
                    switch (m.type) {
                        case protocol::MessageType::ORDER_REQUEST:
                            handle_order_request(session, m.items);
                            break;
                        case protocol::MessageType::DELIVERY:
                            handle_delivery(session, m.items);
                            break;
                        default:
                            send_error(session, protocol::ErrorCode::INVALID_MESSAGE,
                                "Clients cannot send server response messages");
                            break;
                    }
                } else {
                    send_error(session, protocol::ErrorCode::INVALID_MESSAGE,
                        "Clients cannot send server-only message types");
                }
            }, msg);
        } catch (const protocol::ProtocolException& e) {
            send_error(session, e.code(), e.what());
        } catch (const protocol::json::parse_error& e) {
            send_error(session, protocol::ErrorCode::INVALID_MESSAGE, e.what());
        }
    }

    void disconnect_session(const std::shared_ptr<ClientSession>& session, std::string_view reason) {
        std::vector<std::shared_ptr<OrderRecord>> to_cancel;
        bool already_disconnected = false;
        {
            std::lock_guard lock{session->orders_mutex};
            already_disconnected = session->disconnected;
            session->disconnected = true;
            to_cancel = std::move(session->active_orders);
            session->active_orders.clear();
        }

        if (!already_disconnected) {
            log(session->endpoint, "Disconnected: " + std::string(reason));
        }

        protocol::Items restored_total{};
        for (const auto& order : to_cancel) {
            order->stop_source.request_stop();
            if (!order->finalized.exchange(true)) {
                restored_total.coffee += order->items.coffee;
                restored_total.tea += order->items.tea;
                restored_total.croissant += order->items.croissant;
                log(session->endpoint,
                    "ORDER CANCELLED #" + std::to_string(order->order_id) +
                    " (restored " + format_items(order->items) + ")");
            }
        }

        if (restored_total.coffee > 0 || restored_total.tea > 0 || restored_total.croissant > 0) {
            restore_stock(restored_total);
        }

        if (session->is_provider.load()) {
            std::lock_guard lock{provider_mutex_};
            if (active_provider_ == session) {
                active_provider_.reset();
            }
        }

        session->close_socket();

        {
            std::lock_guard lock{sessions_mutex_};
            std::erase_if(sessions_, [&session](const std::weak_ptr<ClientSession>& w) {
                auto locked = w.lock();
                return !locked || locked == session;
            });
        }
    }

    void handle_client(const std::shared_ptr<ClientSession>& session) {
        std::string line_buffer;
        std::array<char, RECV_CHUNK_BYTES> chunk{};
        bool discarding_oversized_line = false;
        auto last_heartbeat = std::chrono::steady_clock::now();
        std::string disconnect_reason = "Connection closed";

        while (!g_shutdown_requested) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = now - last_heartbeat;
            if (elapsed >= HEARTBEAT_TIMEOUT) {
                disconnect_reason = "Heartbeat timeout (no HEARTBEAT within 3s)";
                break;
            }

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                HEARTBEAT_TIMEOUT - elapsed
            );
            const int wait_ms = static_cast<int>(
                std::min<std::chrono::milliseconds::rep>(remaining.count(), POLL_SLICE_MS)
            );

            pollfd pfd{};
            pfd.fd = session->fd;
            pfd.events = POLLIN;

            const int ready = poll(&pfd, 1, std::max(1, wait_ms));
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                disconnect_reason = std::string("poll error: ") + std::strerror(errno);
                break;
            }
            if (ready == 0) {
                continue;
            }
            if ((pfd.revents & (POLLERR | POLLNVAL)) != 0 && (pfd.revents & POLLIN) == 0) {
                disconnect_reason = "Socket error";
                break;
            }

            const ssize_t bytes_read = recv(session->fd, chunk.data(), chunk.size(), 0);
            if (bytes_read == 0) {
                disconnect_reason = "Client closed connection";
                break;
            }
            if (bytes_read < 0) {
                if (errno == EINTR) {
                    continue;
                }
                disconnect_reason = std::string("recv error: ") + std::strerror(errno);
                break;
            }

            line_buffer.append(chunk.data(), static_cast<std::size_t>(bytes_read));

            while (true) {
                const std::size_t nl = line_buffer.find('\n');
                if (nl == std::string::npos) {
                    if (line_buffer.size() > MAX_LINE_BYTES) {
                        if (!discarding_oversized_line) {
                            send_error(session, protocol::ErrorCode::INVALID_MESSAGE,
                                "Message exceeds maximum allowed length");
                            discarding_oversized_line = true;
                        }
                        line_buffer.clear();
                    }
                    break;
                }

                if (discarding_oversized_line) {
                    line_buffer.erase(0, nl + 1);
                    discarding_oversized_line = false;
                    continue;
                }

                if (nl > MAX_LINE_BYTES) {
                    line_buffer.erase(0, nl + 1);
                    send_error(session, protocol::ErrorCode::INVALID_MESSAGE,
                        "Message exceeds maximum allowed length");
                    continue;
                }

                std::string line = line_buffer.substr(0, nl);
                line_buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                dispatch_line(session, line, last_heartbeat);
            }
        }

        if (g_shutdown_requested) {
            disconnect_reason = "Server shutting down";
        }
        disconnect_session(session, disconnect_reason);
    }

    ThreadPool pool_;

    std::mutex stock_mutex_;
    protocol::Items stock_{};
    int next_order_id_{1};

    std::mutex provider_mutex_;
    std::shared_ptr<ClientSession> active_provider_;

    std::mutex sessions_mutex_;
    std::vector<std::weak_ptr<ClientSession>> sessions_;

    std::mutex log_mutex_;
};

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

    struct sigaction sa{};
    sa.sa_handler = handle_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    const auto port = parse_port(argc, argv);
    if (!port) {
        std::cerr << "Usage: " << (argc > 0 ? argv[0] : "server") << " [port (1-65535)]\n";
        return 1;
    }

    const unsigned int hw = std::thread::hardware_concurrency();
    const std::size_t worker_count = hw > 0 ? static_cast<std::size_t>(hw) : 4;

    CoffeeShopServer server(worker_count);
    return server.run(*port);
}
