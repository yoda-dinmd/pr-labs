#include "UdpSocket.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <utility>

namespace chat {

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : fd_{std::exchange(other.fd_, -1)} {}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

bool UdpSocket::open() {
    close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    return fd_ >= 0;
}

void UdpSocket::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool UdpSocket::is_open() const noexcept {
    return fd_ >= 0;
}

int UdpSocket::fd() const noexcept {
    return fd_;
}

bool UdpSocket::enable_reuse_addr() {
    if (fd_ < 0) {
        return false;
    }
    const int opt{1};
    return ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0;
}

bool UdpSocket::enable_reuse_port() {
    if (fd_ < 0) {
        return false;
    }
#ifdef SO_REUSEPORT
    const int opt{1};
    return ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == 0;
#else
    return true;
#endif
}

bool UdpSocket::enable_broadcast() {
    if (fd_ < 0) {
        return false;
    }
    const int opt{1};
    return ::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)) == 0;
}

bool UdpSocket::bind_any(std::uint16_t port) {
    if (fd_ < 0) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    return ::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
}

std::optional<std::uint16_t> UdpSocket::local_port() const {
    if (fd_ < 0) {
        return std::nullopt;
    }
    sockaddr_in addr{};
    socklen_t len{sizeof(addr)};
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return std::nullopt;
    }
    return ntohs(addr.sin_port);
}

bool UdpSocket::send_to(std::string_view data, const Endpoint& destination, int* out_errno) const {
    if (fd_ < 0) {
        if (out_errno != nullptr) {
            *out_errno = EBADF;
        }
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(destination.port);
    if (::inet_pton(AF_INET, destination.ip.c_str(), &addr.sin_addr) != 1) {
        if (out_errno != nullptr) {
            *out_errno = EINVAL;
        }
        return false;
    }

    while (true) {
        const ssize_t sent{::sendto(
            fd_,
            data.data(),
            data.size(),
            0,
            reinterpret_cast<const sockaddr*>(&addr),
            sizeof(addr)
        )};
        if (sent >= 0) {
            return static_cast<std::size_t>(sent) == data.size();
        }
        if (errno == EINTR) {
            continue;
        }
        if (out_errno != nullptr) {
            *out_errno = errno;
        }
        return false;
    }
}

std::optional<ReceivedDatagram> UdpSocket::recv_from(std::size_t max_bytes) const {
    if (fd_ < 0) {
        return std::nullopt;
    }

    std::string buffer{};
    buffer.resize(max_bytes);

    sockaddr_in src_addr{};
    socklen_t src_len{sizeof(src_addr)};

    while (true) {
        const ssize_t received{::recvfrom(
            fd_,
            buffer.data(),
            buffer.size(),
            MSG_TRUNC,
            reinterpret_cast<sockaddr*>(&src_addr),
            &src_len
        )};
        if (received >= 0) {
            const auto actual_size{static_cast<std::size_t>(received)};
            const bool truncated{actual_size > max_bytes};
            buffer.resize(std::min(actual_size, max_bytes));

            std::array<char, INET_ADDRSTRLEN> ip_buf{};
            if (::inet_ntop(AF_INET, &src_addr.sin_addr, ip_buf.data(), ip_buf.size()) == nullptr) {
                return ReceivedDatagram{
                    std::move(buffer),
                    Endpoint{"unknown", ntohs(src_addr.sin_port)},
                    actual_size,
                    truncated
                };
            }
            return ReceivedDatagram{
                std::move(buffer),
                Endpoint{std::string{ip_buf.data()}, ntohs(src_addr.sin_port)},
                actual_size,
                truncated
            };
        }
        if (errno == EINTR) {
            continue;
        }
        return std::nullopt;
    }
}

std::unordered_set<std::string> UdpSocket::local_ipv4_addresses() {
    std::unordered_set<std::string> addresses{"127.0.0.1"};

    ifaddrs* ifaddr{nullptr};
    if (::getifaddrs(&ifaddr) != 0 || ifaddr == nullptr) {
        return addresses;
    }

    for (const ifaddrs* ifa{ifaddr}; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto* addr_in{reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr)};
        std::array<char, INET_ADDRSTRLEN> ip_buf{};
        if (::inet_ntop(AF_INET, &addr_in->sin_addr, ip_buf.data(), ip_buf.size()) != nullptr) {
            addresses.insert(std::string{ip_buf.data()});
        }
    }

    ::freeifaddrs(ifaddr);
    return addresses;
}

bool UdpSocket::is_valid_ipv4(std::string_view ip) {
    const std::string ip_str{ip};
    in_addr addr{};
    return ::inet_pton(AF_INET, ip_str.c_str(), &addr) == 1;
}

} // namespace chat
