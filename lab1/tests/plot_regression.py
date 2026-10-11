"""Small synthetic CSV fixtures check chart math; these are not benchmark measurements."""
import csv
from pathlib import Path
import runpy
import sys
import tempfile

plotter = Path(__file__).resolve().parents[1] / 'plot.py'
fields = 'task,version,threads,workers,run,time_ms,peak_kib,negative,zero,positive,total,counts_match,cache_policy'.split(',')


def row(task, n, time, version=0, run=1):
    race = version == 1
    negative = 20_000_000 if race else 25_000_000
    return dict(zip(fields, [task, version, n + (task == '1c'), n if task == '1c' else 0,
                            run, time, 10240, negative, 0, 25_000_000, negative + 25_000_000,
                            'false' if race else 'true',
                            {'1a': 'warm', '1c': 'warm', '1b': 'array-loaded', '2': 'manual-reconnect'}[task]]))


with tempfile.TemporaryDirectory(prefix='lab1-plot-test-') as temp:
    root = Path(temp)
    files = []
    for task in ('1a', '1b', '1c', '2'):
        path = root / f'{task}.csv'
        if task == '1b':
            rows = [row(task, 12, 300 + i, version=1, run=i) for i in (1, 2, 3)]
            rows += [row(task, 12, 4000, version=2), row(task, 12, 10, version=3)]
        else:
            rows = [row(task, 1, 100), row(task, 1, 300, run=2),
                    row(task, 2, 50), row(task, 2, 100, run=2)]
        with path.open('w', newline='') as file:
            writer = csv.DictWriter(file, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)
        files.append(path)

    sys.argv = [str(plotter), *map(str, files), '--note', 'Synthetic regression fixtures — not measurements']
    state = runpy.run_path(str(plotter), run_name='__main__')
    figures = dict(state['figures'])
    assert len(figures) == 5
    for name in figures:
        for suffix in ('png', 'pdf'):
            assert (root / 'plots' / f'{name}.{suffix}').stat().st_size > 1000
    for task in ('1a', '1c', '2'):
        speedup = figures[f'task-{task}'].axes[1].lines[0].get_ydata()
        assert speedup[0] == 1.0
        assert abs(speedup[1] - 200 / 75) < 1e-12  # Ratio of medians, NOT median of ratios.
    for line in figures['task-1a-vs-1c'].axes[1].lines:
        assert line.get_ydata()[0] == 1.0
    assert len(figures['task-1b'].axes[2].patches) == 5  # All three race runs remain visible.

    # A correct-version count mismatch must be rejected rather than plotted as valid data.
    with files[0].open('a') as file:
        bad = row('1a', 4, 20)
        bad['counts_match'] = 'false'
        csv.DictWriter(file, fieldnames=fields).writerow(bad)
    try:
        state['load']()
    except ValueError:
        pass
    else:
        raise AssertionError('Plotter accepted failed correctness checks')
print('PASS: all chart types, PNG/PDF output, baseline and median math, count validation')
