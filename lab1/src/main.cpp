#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

using Clock = std::chrono::steady_clock;
constexpr long long NUMBER_COUNT = 50'000'000;
constexpr std::size_t FILE_COUNT = 8;
constexpr std::size_t BATCH_SIZE = 10'000;
constexpr std::size_t QUEUE_LIMIT = 100;

struct Counts {
    long long negative = 0, zero = 0, positive = 0;

    void count(int value) {
        if (value < 0) ++negative;
        else if (value == 0) ++zero;
        else ++positive;
    }
    void add(const Counts& other) {
        negative += other.negative;
        zero += other.zero;
        positive += other.positive;
    }
    long long total() const { return negative + zero + positive; }
};

// Fail immediately: this lab assumes valid generated input and does not recover from errors.
[[noreturn]] void fail(const std::string& message) {
    std::cerr << "Error: " << message << '\n';
    std::exit(1);
}

std::ifstream open_input(const std::filesystem::path& path, std::ios::openmode mode = {}) {
    std::ifstream input(path, std::ios::binary | mode);
    if (!input) fail("Cannot open " + path.string());
    return input;
}

int parse(std::string_view line) {
    int value = 0;
    if (std::from_chars(line.data(), line.data() + line.size(), value).ec != std::errc{})
        fail("Invalid integer in input");
    return value;
}

Counts combine(const std::vector<Counts>& partial) {
    Counts result;
    for (const auto& counts : partial) result.add(counts);
    return result;
}

void print_counts(const Counts& counts) {
    std::cout << "Negative: " << counts.negative << " Zero: " << counts.zero
              << " Positive: " << counts.positive << " Total: " << counts.total() << '\n';
}

void print_memory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, static_cast<DWORD>(sizeof(memory))))
        fail("Cannot read peak memory");
    const auto kib = memory.PeakWorkingSetSize / 1024;
#else
    rusage memory{};
    if (getrusage(RUSAGE_SELF, &memory) != 0) fail("Cannot read peak memory");
#ifdef __APPLE__
    const auto kib = memory.ru_maxrss / 1024; // macOS reports bytes.
#else
    const auto kib = memory.ru_maxrss;        // Linux reports KiB.
#endif
#endif
    std::cout << "Peak resident memory: " << kib << " KiB\n";
}

void generate(const std::string& filename) {
    std::ofstream output(filename, std::ios::binary);
    if (!output) fail("Cannot create " + filename);
    std::mt19937 random(42); // Fixed seed makes the laboratory input repeatable.
    std::uniform_int_distribution<int> distribution(-1'000'000, 1'000'000);
    Counts counts;
    for (long long i = 0; i < NUMBER_COUNT; ++i) {
        const int value = distribution(random);
        output << value << '\n';
        counts.count(value);
    }
    output.close();
    if (!output) fail("Writing the input file failed");
    print_counts(counts); // Save these three counters to compare with every task.
}

void split(const std::string& filename, const std::filesystem::path& directory) {
    auto input = open_input(filename);
    std::filesystem::create_directories(directory);
    std::string line;
    for (std::size_t i = 1; i <= FILE_COUNT; ++i) {
        std::ofstream output(directory / ("split_" + std::to_string(i) + ".txt"), std::ios::binary);
        if (!output) fail("Cannot create split file");
        for (long long j = 0; j < NUMBER_COUNT / static_cast<long long>(FILE_COUNT); ++j) {
            if (!std::getline(input, line)) fail("Input is shorter than 50,000,000 lines");
            output << line << '\n';
        }
        output.close();
        if (!output) fail("Writing a split file failed");
    }
    if (std::getline(input, line)) fail("Input is longer than 50,000,000 lines");
    std::cout << "Created 8 files of 6,250,000 numbers in " << directory << '\n';
}

// Task 1a: the main thread finds line boundaries; each worker reads its own range.
Counts task1a(std::size_t n, const std::string& filename) {
    auto input = open_input(filename, std::ios::ate);
    const std::streamoff size = input.tellg();
    std::vector<std::streamoff> bounds{0};
    for (std::size_t i = 1; i < n; ++i) {
        input.clear();
        input.seekg((size / static_cast<std::streamoff>(n)) * static_cast<std::streamoff>(i));
        std::string remainder;
        std::getline(input, remainder); // Move the boundary to the next complete line.
        bounds.push_back(input.eof() ? size : static_cast<std::streamoff>(input.tellg()));
    }
    bounds.push_back(size);

    std::vector<Counts> partial(n);
    std::vector<std::thread> threads;
    for (std::size_t id = 0; id < n; ++id) {
        threads.emplace_back([&, id] {
            auto file = open_input(filename);
            file.seekg(bounds[id]);
            Counts local;
            std::string line;
            auto position = bounds[id];
            while (position < bounds[id + 1] && std::getline(file, line)) {
                local.count(parse(line));
                position += static_cast<std::streamoff>(line.size()) + 1; // Include the newline.
            }
            partial[id] = local;
        });
    }
    for (auto& thread : threads) thread.join();
    return combine(partial);
}

// Task 1b: the array is loaded in main BEFORE starting the timer.
Counts task1b(int version, std::size_t n, const std::vector<int>& numbers) {
    Counts shared;
    std::mutex mutex;
    std::vector<Counts> partial(n);
    std::vector<std::thread> threads;
    for (std::size_t id = 0; id < n; ++id) {
        threads.emplace_back([&, id] {
            const auto begin = numbers.size() * id / n;
            const auto end = numbers.size() * (id + 1) / n;
            if (version == 1) {
                // Intentional data race (undefined behavior in C++), required by the lab.
                for (auto i = begin; i < end; ++i) shared.count(numbers[i]);
            } else if (version == 2) {
                for (auto i = begin; i < end; ++i) {
                    std::lock_guard lock(mutex);
                    shared.count(numbers[i]);
                }
            } else {
                Counts local;
                for (auto i = begin; i < end; ++i) local.count(numbers[i]);
                partial[id] = local;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    return version == 3 ? combine(partial) : shared;
}

// Task 1c: one reader, n workers, at most 100 queued batches of 10,000 raw lines.
Counts task1c(std::size_t n, const std::string& filename) {
    constexpr std::size_t READER_COUNT = 1;

    // Split the file into three ranges, aligned to line boundaries.
    auto input = open_input(filename, std::ios::ate);
    const std::streamoff size = input.tellg();
    std::vector<std::streamoff> bounds{0};

    for (std::size_t i = 1; i < READER_COUNT; ++i) {
        input.clear();
        input.seekg(
            (size / static_cast<std::streamoff>(READER_COUNT)) *
            static_cast<std::streamoff>(i)
        );

        std::string remainder;
        std::getline(input, remainder);
        bounds.push_back(
            input.eof() ? size : static_cast<std::streamoff>(input.tellg())
        );
    }
    bounds.push_back(size);

    using Batch = std::vector<std::string>;
    std::queue<Batch> queue;
    std::mutex mutex;
    std::condition_variable not_empty, not_full;

    // Protected by the same mutex as the queue.
    std::size_t readers_left = READER_COUNT;
    std::vector<Counts> partial(n);

    std::vector<std::thread> readers;
    for (std::size_t id = 0; id < READER_COUNT; ++id) {
        readers.emplace_back([&, id] {
            auto file = open_input(filename);
            file.seekg(bounds[id]);

            auto position = bounds[id];
            const auto end = bounds[id + 1];

            while (position < end) {
                Batch batch;
                batch.reserve(BATCH_SIZE);
                std::string line;

                while (batch.size() < BATCH_SIZE &&
                       position < end &&
                       std::getline(file, line)) {
                    position +=
                        static_cast<std::streamoff>(line.size()) + 1;
                    batch.push_back(std::move(line));
                }

                if (batch.empty()) break;

                std::unique_lock lock(mutex);
                not_full.wait(lock, [&] {
                    return queue.size() < QUEUE_LIMIT;
                });

                queue.push(std::move(batch));
                lock.unlock();
                not_empty.notify_one();
            }

            {
                std::lock_guard lock(mutex);
                --readers_left;
            }
            not_empty.notify_all();
        });
    }

    std::vector<std::thread> workers;
    for (std::size_t id = 0; id < n; ++id) {
        workers.emplace_back([&, id] {
            Counts local;

            while (true) {
                Batch batch;

                {
                    std::unique_lock lock(mutex);
                    not_empty.wait(lock, [&] {
                        return !queue.empty() || readers_left == 0;
                    });

                    // The queue is empty and every reader has finished.
                    if (queue.empty()) break;

                    batch = std::move(queue.front());
                    queue.pop();
                }

                not_full.notify_one();

                // Parsing happens outside the queue lock.
                for (const auto& line : batch)
                    local.count(parse(line));
            }

            partial[id] = local;
        });
    }

    for (auto& reader : readers) reader.join();
    for (auto& worker : workers) worker.join();

    return combine(partial);
}

// Task 2: files are distributed round-robin; each file gets one read into one buffer.
Counts task2(std::size_t n, const std::filesystem::path& directory) {
    std::vector<Counts> partial(n);
    std::vector<std::thread> threads;
    for (std::size_t id = 0; id < n; ++id) {
        threads.emplace_back([&, id] {
            Counts local;
            for (std::size_t i = id; i < FILE_COUNT; i += n) {
                auto input = open_input(directory / ("split_" + std::to_string(i + 1) + ".txt"), std::ios::ate);
                const std::streamsize size = input.tellg();
                if (size <= 0) fail("Empty or unreadable split file");
                input.seekg(0);
                std::vector<char> buffer(static_cast<std::size_t>(size));
                if (!input.read(buffer.data(), size)) fail("Reading a split file failed");
                Counts file_counts;
                std::size_t begin = 0;
                for (std::size_t end = 0; end < buffer.size(); ++end) {
                    if (buffer[end] == '\n') {
                        file_counts.count(parse(std::string_view(buffer.data() + begin, end - begin)));
                        begin = end + 1;
                    }
                }
                if (begin < buffer.size())
                    file_counts.count(parse(std::string_view(buffer.data() + begin, buffer.size() - begin)));
                if (file_counts.total() != NUMBER_COUNT / static_cast<long long>(FILE_COUNT))
                    fail("A split file does not contain 6,250,000 numbers");
                local.add(file_counts);
            }
            partial[id] = local;
        });
    }
    for (auto& thread : threads) thread.join();
    return combine(partial);
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            std::cout << "Usage:\n"
                      << "  working generate [file=numbers.txt]\n"
                      << "  working split <file> <output-directory>\n"
                      << "  working 1a <threads> <file>\n"
                      << "  working 1b <version:1|2|3> <file>  (uses logical processor count)\n"
                      << "  working 1c <workers> <file>\n"
                      << "  working 2 <threads:1|2|4> <USB-directory>\n";
            return 1;
        }
        const std::string task = argv[1];
        if (task == "generate" && (argc == 2 || argc == 3)) {
            generate(argc == 3 ? argv[2] : "numbers.txt");
            return 0;
        }
        if (task == "split" && argc == 4) {
            split(argv[2], argv[3]);
            return 0;
        }
        if (argc != 4 || (task != "1a" && task != "1b" && task != "1c" && task != "2"))
            fail("Invalid arguments; run without arguments for usage");

        const int argument = std::stoi(argv[2]);
        if (task == "1b" ? (argument < 1 || argument > 3) : (argument < 1 || argument > 64))
            fail("Invalid version or thread count");
        if (task == "2" && argument != 1 && argument != 2 && argument != 4)
            fail("Task 2 requires 1, 2, or 4 threads");
        const std::size_t n = task == "1b" ? std::thread::hardware_concurrency() : static_cast<std::size_t>(argument);
        if (n == 0) fail("Cannot detect logical processor count");

        std::vector<int> numbers;
        if (task == "1b") {
            auto input = open_input(argv[3]);
            numbers.reserve(static_cast<std::size_t>(NUMBER_COUNT));
            std::string line;
            while (std::getline(input, line)) numbers.push_back(parse(line));
            if (numbers.size() != static_cast<std::size_t>(NUMBER_COUNT)) fail("Expected 50,000,000 numbers");
        }

        const auto start = Clock::now();
        Counts result;
        if (task == "1a") result = task1a(n, argv[3]);
        else if (task == "1b") result = task1b(argument, n, numbers);
        else if (task == "1c") result = task1c(n, argv[3]);
        else result = task2(n, argv[3]);
        const auto end = Clock::now();

        std::cout << "Task " << task << ", " << n << (task == "1c" ? " workers + 1 reader" : " threads");
        if (task == "1b") std::cout << ", version " << argument;
        std::cout << '\n';
        print_counts(result);
        std::cout << std::fixed << std::setprecision(3)
                  << "Time: " << std::chrono::duration<double, std::milli>(end - start).count() << " ms\n";
        print_memory();
        if (task == "1b" && argument == 1)
            std::cout << "Intentional data race: totals may be incorrect.\n";
        else if (result.total() != NUMBER_COUNT)
            fail("Total must be 50,000,000; discard this run");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
