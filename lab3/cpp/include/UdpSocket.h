#pragma once

#include "Types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace chat {

struct ReceivedDatagram {
    std::string data{};
    Endpoint sender{};
    std::size_t actual_size{0};
    bool truncated{false};
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    bool open();
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] int fd() const noexcept;

    bool enable_reuse_addr();
    bool enable_reuse_port();
    bool enable_broadcast();

    bool bind_any(std::uint16_t port);
    [[nodiscard]] std::optional<std::uint16_t> local_port() const;

    bool send_to(std::string_view data, const Endpoint& destination, int* out_errno = nullptr) const;
    [[nodiscard]] std::optional<ReceivedDatagram> recv_from(std::size_t max_bytes) const;

    [[nodiscard]] static std::unordered_set<std::string> local_ipv4_addresses();
    [[nodiscard]] static bool is_valid_ipv4(std::string_view ip);

private:
    int fd_{-1};
};

} // namespace chat
