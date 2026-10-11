"""Plot the benchmark runner's CSV files; no benchmarks are executed here."""
import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path
from statistics import median

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('csv', nargs='+', type=Path, help='One or more benchmark CSV files')
parser.add_argument('--output-dir', type=Path, help='Default: plots/ beside the first CSV')
parser.add_argument('--show', action='store_true', help='Also open interactive matplotlib windows')
parser.add_argument('--note', default='', help='Optional machine/procedure note printed on each figure')
args = parser.parse_args()

import matplotlib
if not args.show:
    matplotlib.use('Agg')  # Saving figures also works without a desktop/display.
import matplotlib.pyplot as plt

BLUE, TEAL, RED = '#2864b7', '#168477', '#ca5548'
plt.rcParams.update({
    'font.family': 'DejaVu Sans', 'font.size': 10, 'axes.titlesize': 12,
    'axes.titleweight': 'bold', 'axes.spines.top': False, 'axes.spines.right': False,
    'axes.edgecolor': '#cbd5e1', 'axes.labelcolor': '#334155',
    'xtick.color': '#475569', 'ytick.color': '#475569',
    'figure.facecolor': '#f6f8fb', 'axes.facecolor': 'white',
    'grid.color': '#e2e8f0', 'grid.linewidth': 0.8, 'savefig.dpi': 180,
    'pdf.fonttype': 42,
})


def load():
    tasks = defaultdict(list)
    reference = None
    for path in args.csv:
        with path.open(newline='') as file:
            rows = list(csv.DictReader(file))
        if not rows:
            raise ValueError(f'{path}: no completed measurements')
        for row in rows:
            for field in ('version', 'threads', 'workers', 'run', 'peak_kib', 'negative', 'zero', 'positive', 'total'):
                row[field] = int(row[field])
            row['time_ms'] = float(row['time_ms'])
            if not math.isfinite(row['time_ms']) or row['time_ms'] <= 0:
                raise ValueError(f'{path}: elapsed times must be positive')
            if row['task'] not in ('1a', '1b', '1c', '2'):
                raise ValueError(f'{path}: unknown task')
            if row['threads'] < 1 or row['peak_kib'] < 0:
                raise ValueError(f'{path}: invalid thread count or memory')
            signs = (row['negative'], row['zero'], row['positive'])
            if sum(signs) != row['total']:
                raise ValueError(f'{path}: inconsistent counter total')
            race = row['task'] == '1b' and row['version'] == 1
            if not race:
                if reference is None:
                    reference = signs
                if row['total'] != 50_000_000 or signs != reference or row['counts_match'] != 'true':
                    raise ValueError(f'{path}: correct-version counts disagree; do not combine different datasets')
            tasks[row['task']].append(row)
    return tasks


def canvas(title, subtitle, count=3):
    fig, axes = plt.subplots(1, count, figsize=(15 if count == 3 else 11, 5))
    fig.subplots_adjust(left=0.065, right=0.975, top=0.76, bottom=0.19, wspace=0.32)
    fig.text(0.035, 0.93, title, fontsize=19, weight='bold', color='#152b46')
    fig.text(0.035, 0.86, subtitle, fontsize=10, color='#52627a')
    fig.text(0.035, 0.035, args.note or 'Concurrency laboratory | Separate process per measurement',
             fontsize=9, color='#52627a')
    for ax in axes:
        ax.grid(axis='y')
        ax.set_axisbelow(True)
    return fig, axes


def grouped(rows):
    groups = defaultdict(list)
    for row in rows:
        groups[row['workers'] if row['task'] == '1c' else row['threads']].append(row)
    return dict(sorted(groups.items()))


def curve(ax, groups, value, color, label, center=None):
    xs = list(groups)
    samples = [[value(row) for row in groups[x]] for x in xs]
    centers = [center(groups[x]) for x in xs] if center else [median(values) for values in samples]
    ax.plot(xs, centers, 'o-', color=color, label=label, linewidth=2)
    ax.fill_between(xs, [min(values) for values in samples], [max(values) for values in samples],
                    color=color, alpha=0.12)
    for x, values in zip(xs, samples):
        ax.scatter([x] * len(values), values, s=15, color=color, alpha=0.5, zorder=3)
    ax.set_xscale('log', base=2)
    ax.set_xticks(xs, [str(x) for x in xs])
    ax.set_ylim(bottom=0)


def scaling(task, rows):
    groups = grouped(rows)
    if 1 not in groups:
        raise ValueError(f'Task {task}: a one-thread/worker baseline is required for speed-up')
    baseline = median(row['time_ms'] for row in groups[1])
    names = {'1a': 'Independent file partitions', '1c': 'Producer and consumers', '2': 'Files on USB storage'}
    policy = {'1a': 'Warm-cache read before each run', '1c': 'Warm-cache read before each run',
              '2': 'Manual USB reconnect before each run; no warm-up'}[task]
    fig, axes = canvas(f'Task {task}  |  {names[task]}',
                       f'{policy}. Time/memory: medians; speed-up: ratio of median times. Shading: min–max.')
    curve(axes[0], groups, lambda row: row['time_ms'] / 1000, BLUE, 'Elapsed time')
    curve(axes[1], groups, lambda row: baseline / row['time_ms'], TEAL, 'Speed-up',
          center=lambda values: baseline / median(row['time_ms'] for row in values))
    curve(axes[2], groups, lambda row: row['peak_kib'] / 1024, BLUE, 'Peak RSS')
    for ax, title, unit in zip(axes, ('Elapsed time', 'Speed-up vs. one', 'Peak resident memory'),
                                ('Seconds', 'Speed-up (×)', 'MiB')):
        ax.set_title(title, loc='left', pad=12)
        ax.set_ylabel(unit)
        ax.set_xlabel('Workers (+ one reader)' if task == '1c' else 'Threads')
    axes[1].axhline(1, linestyle=':', color='#94a3b8', linewidth=1)
    return fig


def counters(rows):
    groups = {version: [r for r in rows if r['version'] == version] for version in (1, 2, 3)}
    groups = {version: values for version, values in groups.items() if values}
    labels = {1: 'Unsynchronized\n(intentionally racy)', 2: 'Mutex', 3: 'Local counters'}
    colors = {1: RED, 2: BLUE, 3: TEAL}
    fig, axes = canvas('Task 1b  |  Cost and correctness of shared counters',
                       f'{rows[0]["threads"]} threads. Loading excluded from time; included in peak memory. Race results have undefined behavior.')
    for x, (version, values) in enumerate(groups.items()):
        times = [r['time_ms'] for r in values]
        mid = median(times)
        axes[0].errorbar(x, mid, yerr=[[mid - min(times)], [max(times) - mid]],
                         fmt='o', color=colors[version], capsize=6, markersize=8)
        axes[0].scatter([x] * len(times), times, color=colors[version], s=20, alpha=0.5)
        axes[0].annotate(f'{mid:,.2f} ms', (x, max(times)), xytext=(0, 9),
                         textcoords='offset points', ha='center', fontsize=9)
        memory = [r['peak_kib'] / 1024 for r in values]
        axes[1].bar(x, median(memory), width=0.55, color=colors[version], alpha=0.85)
        axes[1].scatter([x] * len(memory), memory, color='#334155', s=14)
    for ax in axes[:2]:
        ax.set_xticks(range(len(groups)), [labels[v] for v in groups])
        ax.set_xlim(-0.65, len(groups) - 0.35)
    axes[0].set_yscale('log')
    axes[0].margins(y=0.35)
    axes[0].set_title('Elapsed time (log scale)', loc='left', pad=12)
    axes[0].set_ylabel('Milliseconds; median and min–max')
    axes[1].set_title('Peak resident memory', loc='left', pad=12)
    axes[1].set_ylabel('MiB; median and individual runs')
    axes[1].margins(y=0.2)
    ordered = sorted(rows, key=lambda r: (r['version'], r['run']))
    axes[2].bar(range(len(ordered)), [r['total'] / 1_000_000 for r in ordered],
                color=[colors[r['version']] for r in ordered], width=0.65)
    axes[2].axhline(50, linestyle='--', color='#334155', label='Expected: 50 million')
    axes[2].set_xticks(range(len(ordered)), [f'v{r["version"]}\nr{r["run"]}' for r in ordered])
    axes[2].set_ylim(0, max(60, max(r['total'] for r in ordered) / 1_000_000 * 1.15))
    axes[2].set_title('Counted values in every run', loc='left', pad=12)
    axes[2].set_ylabel('Millions of values')
    axes[2].legend(frameon=False, fontsize=8, loc='upper right')
    return fig


def comparison(tasks):
    fig, axes = canvas('Task 1a vs. Task 1c  |  How work distribution changes scaling',
                       'Median time and speed-up; dots: individual runs; shading: min–max. Task 1c has one additional reader.', 2)
    for task, color in (('1a', BLUE), ('1c', TEAL)):
        groups = grouped(tasks[task])
        baseline = median(r['time_ms'] for r in groups[1])
        curve(axes[0], groups, lambda r: r['time_ms'] / 1000, color, f'Task {task}')
        curve(axes[1], groups, lambda r: baseline / r['time_ms'], color, f'Task {task}',
              center=lambda values: baseline / median(r['time_ms'] for r in values))
    for ax, title, unit in zip(axes, ('Elapsed time', 'Speed-up vs. own baseline'), ('Seconds', 'Speed-up (×)')):
        ax.set_title(title, loc='left', pad=12)
        ax.set_ylabel(unit)
        ax.set_xlabel('Threads in 1a / workers in 1c')
        ax.legend(frameon=False)
    return fig


try:
    tasks = load()
    destination = args.output_dir or args.csv[0].parent / 'plots'
    destination.mkdir(parents=True, exist_ok=True)
    figures = [(f'task-{task}', counters(rows) if task == '1b' else scaling(task, rows))
               for task, rows in sorted(tasks.items())]
    if '1a' in tasks and '1c' in tasks:
        figures.append(('task-1a-vs-1c', comparison(tasks)))
    for name, fig in figures:
        for extension in ('png', 'pdf'):
            path = destination / f'{name}.{extension}'
            fig.savefig(path)
            print(f'Saved {path}')
    if args.show:
        plt.show()
    else:
        plt.close('all')
except (OSError, ValueError, KeyError) as error:
    parser.exit(1, f'Error: {error}\n')
