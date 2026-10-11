"""Full-size lab correctness check. Temporary local files are NOT USB measurements."""
from pathlib import Path
import csv
import re
import subprocess
import sys
import tempfile

exe = str(Path(sys.argv[1]).resolve())


def run(*args):
    result = subprocess.run([exe, *map(str, args)], capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r'Negative: (\d+) Zero: (\d+) Positive: (\d+) Total: (\d+)', result.stdout)
    return tuple(map(int, match.groups())) if match else None


with tempfile.TemporaryDirectory(prefix="lab1's check-") as temp:
    data = Path(temp) / 'numbers.txt'
    parts = Path(temp) / 'split'
    reference = run('generate', data)
    assert reference[3] == 50_000_000 and sum(reference[:3]) == reference[3]
    run('split', data, parts)

    # Independently check split lengths and preserved byte order with bounded memory.
    with data.open('rb') as original:
        for index in range(1, 9):
            lines = 0
            with (parts / f'split_{index}.txt').open('rb') as part:
                while block := part.read(1024 * 1024):
                    assert original.read(len(block)) == block
                    lines += block.count(b'\n')
            assert lines == 6_250_000
        assert original.read(1) == b''

    if len(sys.argv) > 2:
        # Validate the real launcher/CSV pipeline, including paths with spaces and apostrophes.
        runner = str(Path(sys.argv[2]).resolve())
        for task in ('1a', '1b', '1c', '2'):
            output = Path(temp) / f'{task}.csv'
            command = [runner, task, str(parts if task == '2' else data), str(output)]
            repeats = 2 if task == '1a' else 1
            if task in ('1a', '1c'):
                command.append(str(repeats))
            # Test only: acknowledge prompts with local files; no USB result is claimed.
            result = subprocess.run(command, input='\n\n\n', capture_output=True, text=True, timeout=180)
            assert result.returncode == 0, result.stdout + result.stderr
            with output.open(newline='') as file:
                rows = list(csv.DictReader(file))
            assert len(rows) == {'1a': 14, '1b': 5, '1c': 7, '2': 3}[task]
            for row in rows:
                observed = tuple(int(row[field]) for field in ('negative', 'zero', 'positive', 'total'))
                race = task == '1b' and row['version'] == '1'
                assert race or observed == reference
                assert (row['counts_match'] == 'true') == (observed == reference)
                assert float(row['time_ms']) > 0 and int(row['peak_kib']) > 0
                assert row['cache_policy'] == {'1a': 'warm', '1c': 'warm', '1b': 'array-loaded', '2': 'manual-reconnect'}[task]
                if task == '1c':
                    assert int(row['threads']) == int(row['workers']) + 1
            if task == '1b':
                assert [row['version'] for row in rows] == ['3', '2', '1', '1', '1']
            if task == '2':
                assert result.stdout.count('Press Enter when ready') == 3
            print(f'PASS: task {task} launcher, CSV, counts and run configuration', flush=True)

        cancelled = Path(temp) / 'cancelled.csv'
        result = subprocess.run([runner, '2', str(parts), str(cancelled)], input='', capture_output=True, text=True)
        assert result.returncode != 0 and 'cancelled' in result.stderr
        assert len(cancelled.read_text().splitlines()) == 1  # Header only: no run before confirmation.
        assert subprocess.run([runner, 'all', str(data), str(cancelled)], capture_output=True).returncode != 0
        print('PASS: USB requires confirmation; no all-tasks command', flush=True)
    else:
        for task in ('1a', '1c'):
            for threads in (1, 2, 4, 8, 16, 32, 64):
                assert run(task, threads, data) == reference
        for version in (2, 3):
            assert run('1b', version, data) == reference
        for _ in range(3):
            run('1b', 1, data)
        for threads in (1, 2, 4):
            assert run('2', threads, parts) == reference
    print(f'PASS: all correct versions match {reference}', flush=True)
