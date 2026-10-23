// Separate Linux/macOS launcher: every measurement uses a fresh working process.
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Result {
    int concurrency;
    std::array<long long, 3> signs;
    long long total, peak_kib;
    double time_ms;
};

// Quote each argument for the POSIX shell, including paths containing spaces/apostrophes.
std::string quote(const std::string& value) {
    std::string result = "'";
    for (char c : value) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
}

Result measure(const fs::path& executable, const std::string& task, int setting, const fs::path& input) {
    const std::string command = quote(executable.string()) + " " + quote(task) + " " +
                                std::to_string(setting) + " " + quote(input.string()) + " 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) throw std::runtime_error("Cannot launch working");
    std::string output;
    std::array<char, 1024> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) output += buffer.data();
    if (pclose(pipe) != 0) throw std::runtime_error("Measurement failed:\n" + output);

    std::smatch header, counts, time, memory;
    if (!std::regex_search(output, header, std::regex(R"(Task \S+, (\d+) )")) ||
        !std::regex_search(output, counts, std::regex(R"(Negative: (\d+) Zero: (\d+) Positive: (\d+) Total: (\d+))")) ||
        !std::regex_search(output, time, std::regex(R"(Time: ([0-9.]+) ms)")) ||
        !std::regex_search(output, memory, std::regex(R"(Peak resident memory: (\d+) KiB)")))
        throw std::runtime_error("Cannot read working's results:\n" + output);
    return {std::stoi(header[1]), {std::stoll(counts[1]), std::stoll(counts[2]), std::stoll(counts[3])},
            std::stoll(counts[4]), std::stoll(memory[1]), std::stod(time[1])};
}

// Task 1a/1c use the same warm-cache procedure before EVERY measured run.
// The small buffer keeps the launcher's own memory footprint low.
void warm_cache(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open " + path.string());
    std::array<char, 64 * 1024> buffer{};
    while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()))) {}
    if (input.bad()) throw std::runtime_error("Warm-up read failed");
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 4 || argc > 5) {
            std::cout << "Usage (select ONE task):\n"
                      << "  benchmark 1a <numbers.txt> <output.csv> [repeats=3]\n"
                      << "  benchmark 1b <numbers.txt> <output.csv>\n"
                      << "  benchmark 1c <numbers.txt> <output.csv> [repeats=3]\n"
                      << "  benchmark 2 <USB-directory> <output.csv>\n"
                      << "Task 2 always prompts for a physical reconnect before each run.\n";
            return 1;
        }
        const std::string task = argv[1];
        if (task != "1a" && task != "1b" && task != "1c" && task != "2")
            throw std::runtime_error("Select 1a, 1b, 1c, or 2; there is no combined/all command");
        if (argc == 5 && task != "1a" && task != "1c")
            throw std::runtime_error("Repeats are configurable only for 1a and 1c");
        const int repeats = argc == 5 ? std::stoi(argv[4]) : 3;
        if (repeats < 1) throw std::runtime_error("Repeats must be positive");
        const fs::path input = fs::absolute(argv[2]);
        const fs::path output_path = fs::absolute(argv[3]);
        const fs::path executable = fs::absolute(argv[0]).parent_path() / "working";
        if (!fs::exists(executable)) throw std::runtime_error("Build working beside benchmark first");
        if (fs::exists(output_path) && fs::equivalent(input, output_path))
            throw std::runtime_error("CSV output cannot overwrite the input");
        fs::create_directories(output_path.parent_path());
        std::ofstream csv(output_path);
        csv.exceptions(std::ios::failbit | std::ios::badbit);
        csv << "task,version,threads,workers,run,time_ms,peak_kib,negative,zero,positive,total,counts_match,cache_policy\n";
        csv << std::fixed << std::setprecision(3);

        const bool scaling = task == "1a" || task == "1c";
        const std::string policy = scaling ? "warm" : (task == "1b" ? "array-loaded" : "manual-reconnect");
        std::cout << "Task " << task << " | CSV: " << output_path << '\n';
        if (scaling) std::cout << "Warm-cache read before each run; " << repeats << " runs per configuration.\n";
        if (task == "1b") std::cout << "Local and mutex once each; unsynchronized counters three times.\n";
        if (task == "2") std::cout << "USB only: no warm-up or extra baseline run. Keep the CSV on local storage.\n";
        std::cout << std::left << std::setw(15) << "Configuration" << std::right
                  << std::setw(10) << (task == "1c" ? "Workers" : "Threads") << std::setw(6) << "Run"
                  << std::setw(13) << "Time (ms)" << std::setw(13) << "Peak (MiB)"
                  << std::setw(13) << "Total" << "  Check\n" << std::string(90, '-') << '\n';

        // Start 1b with local counters to obtain a correct comparison for the race.
        const std::vector<int> settings = task == "1b" ? std::vector<int>{3, 2, 1} :
                                          task == "2" ? std::vector<int>{1, 2, 4} :
                                                        std::vector<int>{1, 2, 4, 8, 16, 32, 64};
        std::optional<std::array<long long, 3>> reference;
        for (int setting : settings) {
            const int runs = scaling ? repeats : (task == "1b" && setting == 1 ? 3 : 1);
            const bool race = task == "1b" && setting == 1;
            const std::string label = task == "1b" ? (race ? "v1 race" : setting == 2 ? "v2 mutex" : "v3 local") : task;
            for (int run = 1; run <= runs; ++run) {
                if (task == "2") {
                    std::cout << "\nBefore " << setting << " threads: eject/unmount the USB, unplug it, reconnect it,\n"
                              << "and confirm it is mounted at " << input << ".\n"
                              << "On macOS, disable Spotlight indexing. Do not open/read the data files.\n"
                              << "Press Enter when ready (Ctrl-D cancels): " << std::flush;
                    std::string confirmation;
                    if (!std::getline(std::cin, confirmation)) throw std::runtime_error("USB benchmark cancelled");
                }
                std::cout << "Running " << label << ", setting " << setting << ", run " << run << "...\r" << std::flush;
                if (scaling) warm_cache(input);
                const Result result = measure(executable, task, setting, input);
                if (!reference) reference = result.signs;
                const bool match = result.signs == *reference && result.total == 50'000'000;
                const int workers = task == "1c" ? result.concurrency : 0;
                const int threads = result.concurrency + (task == "1c" ? 1 : 0);
                csv << task << ',' << (task == "1b" ? setting : 0) << ',' << threads << ',' << workers << ',' << run
                    << ',' << result.time_ms << ',' << result.peak_kib << ',' << result.signs[0] << ',' << result.signs[1]
                    << ',' << result.signs[2] << ',' << result.total << ',' << (match ? "true" : "false") << ',' << policy << '\n';
                csv.flush(); // Completed measurements survive a later failure or cancelled USB prompt.
                const std::string check = race ? (match ? "race: match" : "race: mismatch") : (match ? "OK" : "FAILED");
                std::cout << std::left << std::setw(15) << label << std::right << std::setw(10) << result.concurrency
                          << std::setw(6) << run << std::fixed << std::setprecision(3) << std::setw(13) << result.time_ms
                          << std::setprecision(2) << std::setw(13) << static_cast<double>(result.peak_kib) / 1024.0
                          << std::setw(13) << result.total << "  " << check << '\n' << std::flush;
                if (!match && !race) throw std::runtime_error("Sign counts differ; discard the failed measurement");
            }
        }
        std::cout << "Reference counts: negative=" << (*reference)[0] << ", zero=" << (*reference)[1]
                  << ", positive=" << (*reference)[2] << "\nSaved " << output_path
                  << "\nPlot with: python3 lab1/plot.py " << quote(output_path.string()) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "\nError: " << error.what() << '\n';
        return 1;
    }
}
