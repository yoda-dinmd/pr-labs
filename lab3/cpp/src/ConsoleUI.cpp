#include "ConsoleUI.h"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>

namespace chat {

ConsoleUI::ConsoleUI(int input_fd, std::ostream& out, bool enable_raw_mode)
    : input_fd_{input_fd},
      out_{out},
      interactive_{enable_raw_mode && input_fd_ >= 0 && ::isatty(input_fd_) == 1} {
    if (interactive_ && ::tcgetattr(input_fd_, &original_termios_) == 0) {
        termios raw{original_termios_};
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(input_fd_, TCSANOW, &raw) == 0) {
            raw_mode_active_ = true;
        } else {
            interactive_ = false;
        }
    } else {
        interactive_ = false;
    }
}

ConsoleUI::~ConsoleUI() {
    restore_terminal();
}

int ConsoleUI::input_fd() const noexcept {
    return input_fd_;
}

bool ConsoleUI::is_interactive() const noexcept {
    return interactive_;
}

void ConsoleUI::print_line(std::string_view line) {
    if (interactive_) {
        out_ << "\r\033[2K" << line << "\n> " << input_buffer_;
    } else {
        out_ << line << '\n';
    }
    out_.flush();
}

void ConsoleUI::show_prompt() {
    if (interactive_) {
        out_ << "\r\033[2K> " << input_buffer_;
        out_.flush();
    }
}

void ConsoleUI::clear_prompt() {
    if (interactive_) {
        out_ << "\r\033[2K";
        out_.flush();
    }
}

InputPollResult ConsoleUI::read_available() {
    InputPollResult result{};
    if (input_fd_ < 0) {
        result.eof = true;
        return result;
    }

    std::array<char, 256> chunk{};
    const ssize_t n{::read(input_fd_, chunk.data(), chunk.size())};
    if (n == 0) {
        if (!interactive_ && !input_buffer_.empty()) {
            if (input_buffer_.back() == '\r') {
                input_buffer_.pop_back();
            }
            result.lines.push_back(std::move(input_buffer_));
            input_buffer_.clear();
        }
        result.eof = true;
        return result;
    }
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return result;
        }
        result.eof = true;
        return result;
    }

    if (!interactive_) {
        input_buffer_.append(chunk.data(), static_cast<std::size_t>(n));
        while (true) {
            const std::size_t nl{input_buffer_.find('\n')};
            if (nl == std::string::npos) {
                break;
            }
            std::string line{input_buffer_.substr(0, nl)};
            input_buffer_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            result.lines.push_back(std::move(line));
        }
        return result;
    }

    for (std::size_t i{0}; i < static_cast<std::size_t>(n); ++i) {
        const char ch{chunk[i]};
        const auto byte{static_cast<std::uint8_t>(ch)};

        if (escape_seq_) {
            if (!csi_seq_) {
                if (ch == '[') {
                    csi_seq_ = true;
                } else {
                    escape_seq_ = false;
                }
            } else if (byte >= 0x40 && byte <= 0x7E) {
                escape_seq_ = false;
                csi_seq_ = false;
            }
            continue;
        }

        if (byte == 0x1B) {
            escape_seq_ = true;
            csi_seq_ = false;
            continue;
        }

        if (byte == 0x04) {
            if (input_buffer_.empty()) {
                clear_prompt();
                result.eof = true;
                break;
            }
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            clear_prompt();
            result.lines.push_back(std::move(input_buffer_));
            input_buffer_.clear();
            show_prompt();
            continue;
        }

        if (byte == 0x7F || byte == 0x08) {
            if (!input_buffer_.empty()) {
                pop_utf8_char();
                show_prompt();
            }
            continue;
        }

        if (byte >= 0x20) {
            input_buffer_.push_back(ch);
            out_ << ch;
            out_.flush();
        }
    }

    return result;
}

void ConsoleUI::restore_terminal() noexcept {
    if (raw_mode_active_ && input_fd_ >= 0) {
        (void)::tcsetattr(input_fd_, TCSANOW, &original_termios_);
        raw_mode_active_ = false;
    }
}

void ConsoleUI::pop_utf8_char() {
    while (!input_buffer_.empty()) {
        const auto last{static_cast<std::uint8_t>(input_buffer_.back())};
        input_buffer_.pop_back();
        if ((last & 0xC0) != 0x80) {
            break;
        }
    }
}

} // namespace chat
