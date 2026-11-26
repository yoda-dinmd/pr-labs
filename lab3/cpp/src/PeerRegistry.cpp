#include "PeerRegistry.h"

namespace chat {

bool PeerRegistry::upsert(const std::string& name,
                          const Endpoint& endpoint,
                          Clock::time_point now,
                          bool overwrite_different_endpoint) {
    if (const auto it{peers_.find(name)}; it != peers_.end()) {
        if (it->second.endpoint == endpoint || overwrite_different_endpoint) {
            it->second.endpoint = endpoint;
            it->second.last_seen = now;
        }
        return false;
    }
    peers_.emplace(name, PeerInfo{endpoint, now});
    return true;
}

bool PeerRegistry::remove(std::string_view name) {
    if (const auto it{peers_.find(name)}; it != peers_.end()) {
        peers_.erase(it);
        return true;
    }
    return false;
}

std::optional<Endpoint> PeerRegistry::find_endpoint(std::string_view name) const {
    if (const auto it{peers_.find(name)}; it != peers_.end()) {
        return it->second.endpoint;
    }
    return std::nullopt;
}

bool PeerRegistry::contains(std::string_view name) const {
    return peers_.find(name) != peers_.end();
}

std::size_t PeerRegistry::size() const noexcept {
    return peers_.size();
}

std::vector<std::pair<std::string, Endpoint>> PeerRegistry::list_peers() const {
    std::vector<std::pair<std::string, Endpoint>> result{};
    result.reserve(peers_.size());
    for (const auto& [name, info] : peers_) {
        result.emplace_back(name, info.endpoint);
    }
    return result;
}

std::vector<std::string> PeerRegistry::remove_expired(Clock::time_point now,
                                                      std::chrono::milliseconds timeout) {
    std::vector<std::string> expired{};
    for (auto it{peers_.begin()}; it != peers_.end();) {
        if (now - it->second.last_seen >= timeout) {
            expired.push_back(it->first);
            it = peers_.erase(it);
        } else {
            ++it;
        }
    }
    return expired;
}

} // namespace chat
