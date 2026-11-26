#pragma once

#include "Types.h"

#include <cstddef>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace chat {

enum class DatagramDirection {
    Send,
    Recv,
    Drop,
};

class Logger {
public:
    Logger() = default;
    explicit Logger(const std::string& file_path);

    bool open(const std::string& file_path);
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] const std::string& path() const noexcept;

    void log_datagram(DatagramDirection direction,
                      const Endpoint& endpoint,
                      std::string_view kind,
                      std::size_t size_bytes);

private:
    static std::string current_timestamp();
    static std::string_view direction_label(DatagramDirection direction) noexcept;

    mutable std::mutex mutex_{};
    std::ofstream stream_{};
    std::string file_path_{};
};

} // namespace chat
