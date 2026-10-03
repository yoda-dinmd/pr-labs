#include <iostream>
#include <fstream>
#include <random>
#include <chrono>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <sstream>
#include <optional>
#include <functional>
#include <iosfwd>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif


void print_memory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
    std::cout << "Peak working set size: " << pmc.PeakWorkingSetSize / 1024 << " KB\n";
#endif
}

// --- Utilities ---
void print_progress(int current, int total, int dots = 50) {
    int threshold = total / dots;
    if (threshold == 0) threshold = 1;
    if (current % threshold == 0) {
        std::cout << "." << std::flush;
    }
}

void print_thread_progress(int& local_counter, int threshold = 1000000) {
    if (++local_counter % threshold == 0) {
        std::cout << "." << std::flush;
    }
}

void generate_numbers(const std::string& filename) {
    std::random_device rd;
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::seed_seq seq{
        static_cast<unsigned int>(now),
        rd(), rd(), rd(), rd(), rd(), rd(), rd()
    };
    std::mt19937 gen(seq);
    std::uniform_int_distribution<int> dist(-1000000, 1000000);
    
    std::cout << "Generating 50,000,000 numbers to " << filename << " [";
    std::ofstream out(filename);
    int total = 50000000;
    for (int i = 0; i < total; ++i) {
        out << dist(gen) << '\n';
        print_progress(i, total);
    }
    std::cout << "] Done!\n";
}

struct Counts {
    long long neg = 0;
    long long zero = 0;
    long long pos = 0;
    void add(long long n, long long z, long long p) { neg += n; zero += z; pos += p; }
};

// --- Task 1a ---
void parse_chunk(const std::string& filename, std::streampos start, std::streampos end, Counts& counts) {
    std::ifstream file(filename, std::ios::binary);
    file.seekg(start);
    long long neg = 0, zero = 0, pos = 0;
    std::string line;
    int local_progress = 0;
    while (file.tellg() < end && std::getline(file, line)) {
        if (line.empty() || line == "\r") continue;
        int val = std::stoi(line);
        if (val < 0) neg++;
        else if (val == 0) zero++;
        else pos++;
        print_thread_progress(local_progress);
    }
    counts.neg = neg; counts.zero = zero; counts.pos = pos;
}

void task1a(int num_threads, const std::string& filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) return;
    std::streampos size = file.tellg();
    std::vector<std::streampos> bounds = {0};
    std::streampos chunk = size / num_threads;
    for (int i = 1; i < num_threads; ++i) {
        file.seekg(i * chunk);
        std::string dummy;
        std::getline(file, dummy);
        bounds.push_back(file.tellg());
    }
    bounds.push_back(size);
    std::vector<Counts> t_counts(num_threads);
    std::vector<std::thread> threads;
    std::cout << "Task 1a counting [";
    auto start_time = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_threads; ++i)
        threads.emplace_back(parse_chunk, filename, bounds[i], bounds[i+1], std::ref(t_counts[i]));
    for (auto& t : threads) t.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    std::cout << "] Done!\n";
    Counts total;
    for (const auto& c : t_counts) total.add(c.neg, c.zero, c.pos);
    std::cout << "Task 1a (" << num_threads << " threads) - Neg: " << total.neg << " Zero: " << total.zero << " Pos: " << total.pos << "\n";
    std::cout << "Time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() << " ms\n";
    print_memory();
}

// --- Task 1b ---
void task1b(int version, int num_threads, const std::string& filename) {
    std::vector<int> numbers;
    numbers.reserve(50000000);
    std::ifstream file(filename);
    std::string line;
    std::cout << "Loading file into memory [";
    int current_line = 0;
    int total_lines = 50000000;
    while (std::getline(file, line)) {
        if (!line.empty() && line != "\r") numbers.push_back(std::stoi(line));
        print_progress(current_line++, total_lines);
    }
    std::cout << "] Done!\n";
    long long shared_neg = 0, shared_zero = 0, shared_pos = 0;
    std::mutex mtx;
    std::vector<Counts> t_counts(num_threads);
    auto worker = [&](int id, size_t start, size_t end) {
        long long neg = 0, zero = 0, pos = 0;
        int local_progress = 0;
        for (size_t i = start; i < end; ++i) {
            int val = numbers[i];
            if (version == 1) { // Unsynchronized shared
                if (val < 0) shared_neg++; else if (val == 0) shared_zero++; else shared_pos++;
            } else if (version == 2) { // Mutex shared
                std::lock_guard<std::mutex> lock(mtx);
                if (val < 0) shared_neg++; else if (val == 0) shared_zero++; else shared_pos++;
            } else { // Local
                if (val < 0) neg++; else if (val == 0) zero++; else pos++;
            }
            print_thread_progress(local_progress);
        }
        if (version == 3) {
            t_counts[id].neg = neg; t_counts[id].zero = zero; t_counts[id].pos = pos;
        }
    };
    size_t chunk = numbers.size() / num_threads;
    std::vector<std::thread> threads;
    std::cout << "Task 1b v" << version << " counting [";
    auto start_time = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_threads; ++i) {
        size_t start = i * chunk;
        size_t end = (i == num_threads - 1) ? numbers.size() : (i + 1) * chunk;
        threads.emplace_back(worker, i, start, end);
    }
    for (auto& t : threads) t.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    std::cout << "] Done!\n";
    Counts total;
    if (version == 1 || version == 2) {
        total.neg = shared_neg; total.zero = shared_zero; total.pos = shared_pos;
    } else {
        for (const auto& c : t_counts) total.add(c.neg, c.zero, c.pos);
    }
    std::cout << "Task 1b v" << version << " (" << num_threads << " threads) - Neg: " << total.neg << " Zero: " << total.zero << " Pos: " << total.pos << "\n";
    std::cout << "Time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() << " ms\n";
    print_memory();
}

// --- Task 1c ---
void task1c(int num_threads, const std::string& filename) {
    std::queue<std::vector<int>> queue;
    std::mutex q_mtx;
    std::condition_variable q_not_full, q_not_empty;
    bool done = false;
    auto reader = [&]() {
        std::ifstream file(filename);
        std::string line;
        std::vector<int> batch;
        batch.reserve(10000);
        while (std::getline(file, line)) {
            if (line.empty() || line == "\r") continue;
            batch.push_back(std::stoi(line));
            if (batch.size() == 10000) {
                std::unique_lock<std::mutex> lock(q_mtx);
                q_not_full.wait(lock, [&]() { return queue.size() < 100; });
                queue.push(std::move(batch));
                lock.unlock();
                q_not_empty.notify_one();
                batch = std::vector<int>();
                batch.reserve(10000);
            }
        }
        if (!batch.empty()) {
            std::unique_lock<std::mutex> lock(q_mtx);
            q_not_full.wait(lock, [&]() { return queue.size() < 100; });
            queue.push(std::move(batch));
        }
        {
            std::lock_guard<std::mutex> lock(q_mtx);
            done = true;
        }
        q_not_empty.notify_all();
    };

    std::vector<Counts> t_counts(num_threads);
    auto worker = [&](int id) {
        long long neg = 0, zero = 0, pos = 0;
        int local_progress = 0;
        while (true) {
            std::vector<int> batch;
            {
                std::unique_lock<std::mutex> lock(q_mtx);
                q_not_empty.wait(lock, [&]() { return !queue.empty() || done; });
                if (queue.empty() && done) break;
                batch = std::move(queue.front());
                queue.pop();
            }
            q_not_full.notify_one();
            for (int val : batch) {
                if (val < 0) neg++; else if (val == 0) zero++; else pos++;
                print_thread_progress(local_progress);
            }
        }
        t_counts[id].neg = neg; t_counts[id].zero = zero; t_counts[id].pos = pos;
    };

    std::cout << "Task 1c counting [";
    auto start_time = std::chrono::high_resolution_clock::now();
    std::thread r_thread(reader);
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) threads.emplace_back(worker, i);
    r_thread.join();
    for (auto& t : threads) t.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    std::cout << "] Done!\n";

    Counts total;
    for (const auto& c : t_counts) total.add(c.neg, c.zero, c.pos);
    std::cout << "Task 1c (" << num_threads << " workers) - Neg: " << total.neg << " Zero: " << total.zero << " Pos: " << total.pos << "\n";
    std::cout << "Time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() << " ms\n";
    print_memory();
}

// --- Task 2 Split ---
void split_files(const std::string& filename) {
    std::ifstream file(filename);
    std::string line;
    std::cout << "Splitting into 8 files [";
    int total_lines = 50000000;
    int current_line = 0;
    for (int i = 0; i < 8; ++i) {
        std::ofstream out("split_" + std::to_string(i+1) + ".txt");
        for (int j = 0; j < 6250000 && std::getline(file, line); ++j) {
            out << line << '\n';
            print_progress(current_line++, total_lines);
        }
    }
    std::cout << "] Done!\n";
}

// --- Task 2 Read ---
void task2(int num_threads) {
    std::vector<std::string> files;
    for (int i = 1; i <= 8; ++i) files.push_back("split_" + std::to_string(i) + ".txt");
    
    std::vector<Counts> t_counts(num_threads);
    auto worker = [&](int id) {
        long long neg = 0, zero = 0, pos = 0;
        int local_progress = 0;
        for (size_t i = id; i < files.size(); i += num_threads) {
            std::ifstream file(files[i], std::ios::binary | std::ios::ate);
            if (!file) continue;
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            std::vector<char> buffer(size);
            if (file.read(buffer.data(), size)) {
                // Parse from memory buffer
                std::string str(buffer.begin(), buffer.end());
                std::istringstream iss(str);
                std::string line;
                while (std::getline(iss, line)) {
                    if (line.empty() || line == "\r") continue;
                    int val = std::stoi(line);
                    if (val < 0) neg++; else if (val == 0) zero++; else pos++;
                    print_thread_progress(local_progress);
                }
            }
        }
        t_counts[id].neg = neg; t_counts[id].zero = zero; t_counts[id].pos = pos;
    };
    
    std::cout << "Task 2 counting [";
    auto start_time = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    std::cout << "] Done!\n";
    
    Counts total;
    for (const auto& c : t_counts) total.add(c.neg, c.zero, c.pos);
    std::cout << "Task 2 (" << num_threads << " threads) - Neg: " << total.neg << " Zero: " << total.zero << " Pos: " << total.pos << "\n";
    std::cout << "Time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() << " ms\n";
    print_memory();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage:\n";
        std::cout << "  generate\n";
        std::cout << "  1a <threads> <file>\n";
        std::cout << "  1b <version(1|2|3)> <threads> <file>\n";
        std::cout << "  1c <threads> <file>\n";
        std::cout << "  split <file>\n";
        std::cout << "  2 <threads>\n";
        return 1;
    }
    
    std::string task = argv[1];
    if (task == "generate") {
        generate_numbers("numbers.txt");
    } else if (task == "1a" && argc >= 4) {
        task1a(std::stoi(argv[2]), argv[3]);
    } else if (task == "1b" && argc >= 5) {
        task1b(std::stoi(argv[2]), std::stoi(argv[3]), argv[4]);
    } else if (task == "1c" && argc >= 4) {
        task1c(std::stoi(argv[2]), argv[3]);
    } else if (task == "split" && argc >= 3) {
        split_files(argv[2]);
    } else if (task == "2" && argc >= 3) {
        task2(std::stoi(argv[2]));
    } else {
        std::cerr << "Invalid arguments.\n";
    }
    return 0;
}
