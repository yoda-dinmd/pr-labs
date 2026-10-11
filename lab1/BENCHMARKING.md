# Benchmarks and plots

`src/benchmark.cpp` is a separate launcher. It calls the existing `working`
executable in a **fresh process for every measurement**, reads its reported time
and peak memory, displays a table, and writes a CSV. It does not time shell
startup, Python, or Task 1b's input loading. The task implementation in
`src/main.cpp` is unchanged.

The runner supports Linux/macOS (POSIX process launching). The existing counting
program retains its Windows memory implementation; the new launcher has not been
ported to Windows. Linux was tested.

## 1. Build and prepare input

From the repository root:

```bash
cmake -S lab1 -B lab1/build -DCMAKE_BUILD_TYPE=Release
cmake --build lab1/build --parallel
mkdir -p lab1/data lab1/results
```

If you do not already have the input, generate it **once**:

```bash
./lab1/build/working generate lab1/data/numbers.txt > lab1/results/reference.txt
```

Use that same input for all tasks. Generation's three counts are your reference.
Keep the hardware/runtime inventory required by the assignment with the results.
See [README.md](README.md) for the hardware commands and task explanations.

## 2. Select a task

Each command below is independent. Run only the task you want:

```bash
./lab1/build/benchmark 1a lab1/data/numbers.txt lab1/results/1a.csv
./lab1/build/benchmark 1b lab1/data/numbers.txt lab1/results/1b.csv
./lab1/build/benchmark 1c lab1/data/numbers.txt lab1/results/1c.csv
```

There is deliberately **no `all` command** and these commands never run Task 2.

| Task | Configurations | Runs |
| --- | --- | --- |
| 1a | 1, 2, 4, 8, 16, 32, 64 threads | 3 per configuration by default |
| 1b | Local counters, mutex counters, unsynchronized counters | Local once, mutex once, race three times |
| 1c | 1, 2, 4, 8, 16, 32, 64 workers, plus one reader | 3 per configuration by default |

For 1a and 1c, an optional final argument changes the number of repeats:

```bash
./lab1/build/benchmark 1a lab1/data/numbers.txt lab1/results/1a.csv 1
./lab1/build/benchmark 1c lab1/data/numbers.txt lab1/results/1c.csv 5
```

Task 1b always uses the logical processor count chosen by `working`. Its local
version runs first to establish correct sign counts for comparison with the
race. That ordering does not alter the three required implementations.

**Cache policy:** the launcher reads the input once, without parsing, before
**every** Task 1a/1c measurement. This unmeasured read gives a consistent warm-cache
procedure when the input fits in available cache. It is not a cold-storage
experiment. Task 1b already loads its array before timing. Cache policy is saved
in each CSV row. Do not compare these results to earlier runs with different
cache conditions without explaining the difference.

The terminal shows configuration, thread/worker count, run number, elapsed
milliseconds, peak MiB, total counted values, and a count-comparison status.
Full negative/zero/positive counts are in the CSV; reference counts are also
printed at the end. A race can match by chance and still has undefined behavior.

## 3. Task 2 is a separate, interactive operation

Prepare the eight files locally, then copy them to the USB:

```bash
./lab1/build/working split lab1/data/numbers.txt lab1/data/split
```

Copy `split_1.txt` through `split_8.txt` to a dedicated directory on the USB.
Then select Task 2 explicitly, using the real mount path:

```bash
./lab1/build/benchmark 2 /actual/usb/mount/lab1 lab1/results/2.csv
```

Before **each** of the 1-, 2-, and 4-thread runs, the runner stops and asks you to:

1. Eject/unmount the USB and physically unplug it.
2. Reconnect it and check that the same path is mounted.
3. Press Enter to start that one measurement.

There is no Task 2 warm-up, verification read, extra baseline run, or automatic
retry. One run per configuration is recorded. Do not read the data through
another command or application after reconnecting. On macOS, also disable
Spotlight indexing for the drive, as required by the assignment. The program
cannot verify the physical reconnect; the prompt is a reminder, not automatic
cache eviction or USB detection.

Keep the CSV on local storage, as in the example. Ctrl-D at a prompt cancels;
already completed rows remain saved. A new invocation overwrites its specified
CSV, so use another output name if preserving an earlier/partial run.

## 4. Plot CSV results with Python

Matplotlib is the only extra Python dependency. If your Python already has it,
use `python3` directly. Otherwise, install it into a project environment:

```bash
python3 -m venv lab1/.venv
lab1/.venv/bin/python -m pip install -r lab1/requirements.txt
```

Use `lab1/.venv/bin/python` in place of `python3` below if you created that environment.

Plot one task:

```bash
python3 lab1/plot.py lab1/results/1a.csv
```

Or plot your completed Task 1 CSVs together:

```bash
python3 lab1/plot.py lab1/results/1a.csv lab1/results/1b.csv lab1/results/1c.csv
```

Plot Task 2 after its measurements are complete:

```bash
python3 lab1/plot.py lab1/results/2.csv
```

The script saves a **PNG and a vector PDF** for each task in `lab1/results/plots`.
When both 1a and 1c are supplied, it also creates `task-1a-vs-1c.png` and `.pdf`.
Plotting only reads the CSV files: it never executes a benchmark or touches USB
data. It is fine to include the completed Task 2 CSV alongside others to plot
all existing results in one invocation.

Optional settings:

```bash
python3 lab1/plot.py lab1/results/1a.csv --show
python3 lab1/plot.py lab1/results/1a.csv --output-dir lab1/results/report-figures \
    --note 'My CPU model; physical/logical cores; warm-cache input'
```

`--show` opens interactive matplotlib windows when a graphical display/backend
is available. Without it, saving works on a headless system. Regenerating a plot
replaces the corresponding image/PDF.

### What the figures show

- **1a, 1c, and 2:** elapsed time, speed-up, and peak resident memory. Dots show
  individual measurements; lines show medians; shaded ranges show the minimum
  and maximum. Speed-up is `median time at 1 / median time at p`, with its point
  at one thread/worker exactly 1.0. One-run data naturally has no visible range.
- **1b:** time by version on a logarithmic axis, memory by version, and the total
  counted in every individual run against the expected 50 million. Race results
  are red and labeled as intentionally racy, regardless of whether they match.
- **1a versus 1c:** comparable elapsed-time curves and speed-up relative to each
  task's own baseline. The x-axis means threads for 1a and workers for 1c; Task
  1c additionally has its reader thread.

Ranges are observed min–max, **not confidence intervals**. The plotter rejects
failed correct-version counts or mixed datasets with different sign counts.
These checks do not turn a matching race into a correct algorithm.

## CSV fields and interpretation

Each row represents one process execution:

```text
task,version,threads,workers,run,time_ms,peak_kib,negative,zero,positive,total,counts_match,cache_policy
```

`version` is 1/2/3 for Task 1b and 0 otherwise. For Task 1c, `workers` is the
selected worker count and `threads = workers + 1`; for other tasks `workers=0`.
`threads` excludes the main coordination thread. `peak_kib` is the child
process's lifetime peak resident memory, not the launcher or plotter's memory.
The charts convert KiB to MiB. Task 1b's array contributes to that process peak.

The runner compares all three sign counts with the first correct run of that
selected task. A correct-version mismatch is saved with `counts_match=false`
and then stops the series. Race mismatches are recorded and expected. Compare
each series' reference with generation's counts; loading several task CSVs into
the plotter also checks their correct-version sign counts against each other.

Rows are flushed immediately so completed measurements survive a subsequent
failure or cancellation. Reusing an output filename starts a new CSV rather
than silently mixing sessions. Archive or rename the CSV if you want to retain
an earlier session. Keep paths with spaces inside shell quotes.

## Validation of the tooling

`ctest --test-dir lab1/build --output-on-failure` tests the runner against the
full 50-million-value workload, checks CSV rows and repetition counts, exercises
paths containing spaces/apostrophes, and confirms that Task 2 does not run when
its prompt receives EOF. The automated test acknowledges USB prompts against
temporary **local** files only; those are correctness tests, not USB measurements.

With Matplotlib installed, `python3 lab1/tests/plot_regression.py` separately
checks all chart types, PNG/PDF generation, speed-up arithmetic, and rejection of
failed correctness rows using small synthetic CSV fixtures.
