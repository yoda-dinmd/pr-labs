#include "Logger.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace chat {

Logger::Logger(const std::string& file_path) {
    (void)open(file_path);
}

bool Logger::open(const std::string& file_path) {
    std::lock_guard lock{mutex_};
    if (stream_.is_open()) {
        stream_.close();
    }
    file_path_ = file_path;
    stream_.open(file_path_, std::ios::out | std::ios::app);
    return stream_.is_open();
}

bool Logger::is_open() const {
    std::lock_guard lock{mutex_};
    return stream_.is_open();
}

const std::string& Logger::path() const noexcept {
    return file_path_;
}

void Logger::log_datagram(DatagramDirection direction,
                          const Endpoint& endpoint,
                          std::string_view kind,
                          std::size_t size_bytes) {
    std::lock_guard lock{mutex_};
    if (!stream_.is_open()) {
        return;
    }

    stream_ << current_timestamp()
            << " | " << direction_label(direction)
            << " | " << endpoint.to_string()
            << " | " << kind
            << " | " << size_bytes << " bytes\n";
    stream_.flush();
}

std::string Logger::current_timestamp() {
    const auto now{std::chrono::system_clock::now()};
    const auto ms{std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000};
    const std::time_t raw_time{std::chrono::system_clock::to_time_t(now)};

    std::tm local_tm{};
    localtime_r(&raw_time, &local_tm);

    std::array<char, 32> date_buf{};
    (void)std::strftime(date_buf.data(), date_buf.size(), "%Y-%m-%d %H:%M:%S", &local_tm);

    std::array<char, 40> full_buf{};
    (void)std::snprintf(
        full_buf.data(),
        full_buf.size(),
        "%s.%03lld",
        date_buf.data(),
        static_cast<long long>(ms.count())
    );
    return std::string{full_buf.data()};
}

std::string_view Logger::direction_label(DatagramDirection direction) noexcept {
    switch (direction) {
        case DatagramDirection::Send:
            return "SEND";
        case DatagramDirection::Recv:
            return "RECV";
        case DatagramDirection::Drop:
            return "DROP";
    }
    return "UNKNOWN";
}

} // namespace chat
