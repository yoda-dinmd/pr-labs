# Lab 1: Concurrency in Practice

Count negative, zero, and positive values in a dataset of 50 million integers
and compare how thread count, synchronization, and storage affect time and
memory use.

| Task | Approach |
| --- | --- |
| 1a | Divide the file into line-aligned ranges; each thread reads and counts its own range |
| 1b | Compare unsynchronized shared counters, a mutex, and local counters |
| 1c | One reader feeds batches of 10,000 lines to consumers through a bounded queue |
| 2 | Divide eight files on a USB drive among threads; read each file into a byte buffer |

## Build

Requirements: a C++23 compiler and CMake 3.20 or newer. The benchmark launcher
uses POSIX process APIs and supports Linux/macOS.

All commands below run from the repository root.

```bash
cmake -S lab1 -B lab1/build -DCMAKE_BUILD_TYPE=Release
cmake --build lab1/build --parallel
```

This builds `working`, the task implementation, and `benchmark`, the measurement
launcher on Linux/macOS.

## Prepare the data

Generate the dataset once and use it for every task:

```bash
mkdir -p lab1/data lab1/results
./lab1/build/working generate lab1/data/numbers.txt > lab1/results/reference.txt
```

The generator uses seed 42 and produces integers between −1,000,000 and
1,000,000. It prints the three sign counts; keep these as the correctness
reference. Correct implementations must match each count and total 50 million.

For Task 2, split the dataset into eight consecutive files:

```bash
./lab1/build/working split lab1/data/numbers.txt lab1/data/split
```

Copy `split_1.txt` through `split_8.txt` to a directory on the USB drive.

## Run benchmarks

Choose a task:

```bash
./lab1/build/benchmark 1a lab1/data/numbers.txt lab1/results/1a.csv
./lab1/build/benchmark 1b lab1/data/numbers.txt lab1/results/1b.csv
./lab1/build/benchmark 1c lab1/data/numbers.txt lab1/results/1c.csv
```

Tasks 1a and 1c measure 1, 2, 4, 8, 16, 32, and 64 threads/workers, with three
runs per configuration. Task 1b uses the logical processor count and runs the
local and mutex versions once each, followed by three unsynchronized runs.
The unsynchronized version intentionally has a C++ data race and may return
incorrect counts.

For Task 2, replace the example path with the directory on your USB drive:

```bash
./lab1/build/benchmark 2 /actual/usb/mount/lab1 lab1/results/2.csv
```

The launcher measures 1, 2, and 4 threads. Before each run, follow its prompt to
eject, unplug, reconnect, and mount the drive. On macOS, disable Spotlight
indexing for that volume. Avoid reading the files between reconnecting and
starting the measurement.

See [BENCHMARKING.md](BENCHMARKING.md) for plotting, repeat counts, cache policy,
and measurement interpretation.

## Run a single configuration

The task executable can also be called directly:

```bash
./lab1/build/working 1a 4 lab1/data/numbers.txt
./lab1/build/working 1b 3 lab1/data/numbers.txt
./lab1/build/working 1c 4 lab1/data/numbers.txt
./lab1/build/working 2 2 /actual/usb/mount/lab1
```

The number selects threads for 1a and 2, consumers for 1c, or the counter version
for 1b: `1` unsynchronized, `2` mutex, `3` local. Direct runs print time, peak
memory, and sign counts. They do not perform the launcher's cache warm-up or USB
reconnect prompts.

## Source and report

- [src/main.cpp](src/main.cpp): data generation, splitting, and task implementations.
- [src/benchmark.cpp](src/benchmark.cpp): repeated measurements and CSV output.
- [plot.py](plot.py): charts from saved CSV files.
- [lab1.tex](lab1.tex): report source.
- [Report PDF](../build/lab1.pdf): results and analysis, including the additional six-reader experiment.

## Tests

```bash
ctest --test-dir lab1/build --output-on-failure
```

The tests generate a full-size temporary dataset and check counting, splitting,
and benchmark output. They need roughly 750 MB of temporary storage. Task 2 is
tested using local files; its USB performance must be measured separately.
