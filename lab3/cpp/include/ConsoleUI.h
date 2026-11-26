#pragma once

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <termios.h>
#include <vector>

namespace chat {

struct InputPollResult {
    std::vector<std::string> lines{};
    bool eof{false};
};

class ConsoleUI {
public:
    explicit ConsoleUI(int input_fd = 0, std::ostream& out = std::cout, bool enable_raw_mode = true);
    ~ConsoleUI();

    ConsoleUI(const ConsoleUI&) = delete;
    ConsoleUI& operator=(const ConsoleUI&) = delete;

    [[nodiscard]] int input_fd() const noexcept;
    [[nodiscard]] bool is_interactive() const noexcept;

    void print_line(std::string_view line);
    void show_prompt();
    void clear_prompt();

    [[nodiscard]] InputPollResult read_available();

private:
    void restore_terminal() noexcept;
    void pop_utf8_char();

    int input_fd_{0};
    std::ostream& out_;
    bool interactive_{false};
    bool raw_mode_active_{false};
    bool escape_seq_{false};
    bool csi_seq_{false};
    termios original_termios_{};
    std::string input_buffer_{};
};

} // namespace chat
