#pragma once

#include "Types.h"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chat {

struct PeerInfo {
    Endpoint endpoint{};
    std::chrono::steady_clock::time_point last_seen{};
};

class PeerRegistry {
public:
    using Clock = std::chrono::steady_clock;

    // Adds a new peer or updates an existing one. Returns true if newly added.
    bool upsert(const std::string& name,
                const Endpoint& endpoint,
                Clock::time_point now,
                bool overwrite_different_endpoint = true);

    bool remove(std::string_view name);

    [[nodiscard]] std::optional<Endpoint> find_endpoint(std::string_view name) const;
    [[nodiscard]] bool contains(std::string_view name) const;
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] std::vector<std::pair<std::string, Endpoint>> list_peers() const;

    // Removes peers silent for at least `timeout` and returns their names.
    [[nodiscard]] std::vector<std::string> remove_expired(Clock::time_point now,
                                                          std::chrono::milliseconds timeout);

private:
    std::map<std::string, PeerInfo, std::less<>> peers_{};
};

} // namespace chat
