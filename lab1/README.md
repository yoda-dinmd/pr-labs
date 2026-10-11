# Laboratory 1: Concurrency in Practice

A single C++ program for the four experiments. It assumes the valid text file
produced by `generate`: 50,000,000 integers, one per line. There are no reference
files, dataset-size options, or preliminary verification commands.

## Build

From the repository root:

```bash
cmake -S lab1 -B lab1/build -DCMAKE_BUILD_TYPE=Release
cmake --build lab1/build --parallel
```

The executable is `lab1/build/working`. CMake exports the C++23 compilation
settings for clangd. If VS Code shows stale errors, run **clangd: Restart language
server** from the Command Palette. Use a Release build for measurements.

## Benchmark runner and graphs

On Linux/macOS, the build also produces a separate `benchmark` launcher.
Select a task to print a results table and save its measurements to CSV:

```bash
./lab1/build/benchmark 1a lab1/data/numbers.txt lab1/results/1a.csv
./lab1/build/benchmark 1b lab1/data/numbers.txt lab1/results/1b.csv
./lab1/build/benchmark 1c lab1/data/numbers.txt lab1/results/1c.csv
```

Task 2 is a separate command and prompts for eject/unplug/reconnect before each run:

```bash
./lab1/build/benchmark 2 /actual/usb/mount/lab1 lab1/results/2.csv
```

With Matplotlib installed, plot a saved CSV (PNG and PDF):

```bash
python3 lab1/plot.py lab1/results/1a.csv
```

See [BENCHMARKING.md](BENCHMARKING.md) for repeats, cache policy, Python setup,
combined plots, CSV fields, and the USB procedure. The manual commands below
remain available. All task implementations stay in `main.cpp`; the runner
is in `src/benchmark.cpp`, and plotting is in `plot.py`.

## Generate and split

```bash
mkdir -p lab1/data lab1/results
./lab1/build/working generate lab1/data/numbers.txt > lab1/results/reference.txt
./lab1/build/working split lab1/data/numbers.txt lab1/data/split
```

Generation uses seed 42 and prints the negative, zero, and positive counts.
**Save those three counts and compare every correct task's output against them.**
Each task also checks that its total is 50,000,000. The intentional race is the
exception: its counts may be wrong. If you already have the valid lab dataset,
you can use Task 1a with one thread to obtain the reference counts.

Splitting produces `split_1.txt` through `split_8.txt`, in order, with 6,250,000
numbers each. Preparation overwrites its output files; use a dedicated split
directory. Old `.counts` files from the previous implementation are unused.

## Task 1a: independent file partitions

```bash
for threads in 1 2 4 8 16 32 64; do
    ./lab1/build/working 1a "$threads" lab1/data/numbers.txt \
        > "lab1/results/1a-${threads}.txt" || break
done
```

The main thread finds byte boundaries at line endings. Each worker opens the
file, seeks to its own start, parses only its range, and counts locally. After
joining, the main thread adds the local counters. It never preloads the entire
file on one thread.

## Task 1b: shared counters versus local counters

```bash
for run in 1 2 3; do
    ./lab1/build/working 1b 1 lab1/data/numbers.txt \
        > "lab1/results/1b-v1-run${run}.txt" || break
done
./lab1/build/working 1b 2 lab1/data/numbers.txt > lab1/results/1b-v2.txt
./lab1/build/working 1b 3 lab1/data/numbers.txt > lab1/results/1b-v3.txt
```

The thread count is automatically `std::thread::hardware_concurrency()` and is
printed in the output. **The old `<threads|auto>` argument has been removed.**

| Version | Counting method |
| --- | --- |
| 1 | Unsynchronized shared counters: intentional data race |
| 2 | Shared counters with a mutex locked for each update |
| 3 | Local counters per worker, combined after joining |

All numbers are loaded into an integer array before the timer starts. Versions
2 and 3 must match the reference. Version 1 has undefined behavior in C++; neither
wrong totals nor different totals across runs are guaranteed. Keep this race for
the experiment rather than replacing it with atomic counters.

## Task 1c: producer and consumers

```bash
for workers in 1 2 4 8 16 32 64; do
    ./lab1/build/working 1c "$workers" lab1/data/numbers.txt \
        > "lab1/results/1c-${workers}.txt" || break
done
```

One extra reader thread puts batches of 10,000 **raw lines** into a queue holding
at most 100 batches. Workers take batches, parse them, and count locally. A mutex
protects the queue; condition variables make the reader wait when it is full and
workers wait when it is empty. At EOF, the reader sets `done` and wakes all
workers. Workers stop only after the remaining queue is empty.

## Task 2: USB files

Copy the eight split files to the USB. No metadata file is required. Before
**each** run, eject/unmount, unplug, and reconnect the drive. On macOS, also
disable Spotlight indexing on that volume, as the lab requires. Do not read the
data after reconnecting until you run the measured task.

Replace the directory below with the real USB mount. Run these commands
separately, reconnecting the USB between them:

```bash
./lab1/build/working 2 1 /actual/usb/mount/lab1 > lab1/results/2-1.txt
# Eject, unplug, reconnect.
./lab1/build/working 2 2 /actual/usb/mount/lab1 > lab1/results/2-2.txt
# Eject, unplug, reconnect.
./lab1/build/working 2 4 /actual/usb/mount/lab1 > lab1/results/2-4.txt
```

The directory argument is required; ensure it points to the USB. Each worker
gets 8, 4, or 2 whole files respectively. It reads each file with one `read()`
call into one byte buffer and parses directly from that buffer. It does not copy
that file into a string or a list of lines. The library/device may internally
split the read request into smaller transfers.

## Measurements and report

Every counting command prints sign counts, their sum, elapsed milliseconds,
and peak resident memory in KiB. Run one configuration per process.

- Timing uses `steady_clock` and includes task setup, thread creation, work,
  joining, and combining counters. Task 1b's array loading is excluded.
- There is no console output inside the timed counting loops.
- Memory is the process-lifetime peak: Linux/macOS peak RSS or Windows peak
  working set. Task 1b's array is included in memory even though its loading is
  excluded from time. These are resident pages, not reserved virtual memory.
- Use the same input throughout. Keep Task 1's cache conditions consistent and
  describe them; a cold first run followed by warm runs can distort speed-up.
- Calculate `speed-up(p) = time(1) / time(p)`. For Task 1c, `p` means workers,
  with one reader also running. Use your measurements to answer the report's
  scaling, memory, locking, and CPU/I/O-bound questions.

Record CPU model, physical cores, logical processors, RAM, main disk type,
USB model/version/connection, OS, compiler, and runtime version. Useful Linux
commands are `lscpu`, `free -h`, `lsblk -o NAME,MODEL,TRAN,ROTA,SIZE,MOUNTPOINTS`,
`lsusb`, `lsusb -t`, `uname -a`, `cat /etc/os-release`, `c++ --version`, and
`ldd lab1/build/working`. If virtualized or CPU-restricted, record that too and
check that Task 1b's reported logical count matches the intended environment.

## Optional correctness check

With Python 3 installed, run:

```bash
ctest --test-dir lab1/build --output-on-failure
```

This generates a temporary full-size dataset, checks split sizes/order, compares
all correct configurations to generation's three sign counts, and runs the race
three times. It needs about 750 MB for temporary data and can take a few minutes.
The files are removed afterward. It tests Task 2 using **local storage**, so it
is not the required USB benchmark. Linux was tested; Windows/macOS execution was
not tested.

See [FIXES.md](FIXES.md) for why the implementation was simplified and which
original correctness fixes remain.
