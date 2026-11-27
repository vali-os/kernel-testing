"""Host test of the SD timing test's log parser and derived split.

testing/sdtiming_report.py reads the program's lines out of a serial log.
The test checks, on logs written here:

- Probe lines with a level tag in front and a carriage return behind, as a
  board's serial log has them, are read with every value.
- Skipped lines, FAIL lines, open and delete retries, the file, start and
  result lines are found, and other programs' lines are ignored.
- A log of two boots keeps the last line of each probe and counts the runs.
- --probes lists expand to the probe names in run order, and an unknown name
  is refused.
- Each value of the derived split, from the minimums and from the medians,
  is the formula of the script's docstring, and is None when a probe it
  needs is missing.
- The table lists every probe and skipped probe, and the split.
- The script's command line prints the table and writes the JSON file, and
  refuses a log without probe lines.

With --mutation-check, each mutation of sdtiming_report.py must make at
least one case fail.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TESTING = ROOT / 'testing'
REPORT = TESTING / 'sdtiming_report.py'

# The values of the test log, in microseconds: (calls, min, median, p90,
# max, mean). Chosen so every split term differs.
VALUES = {
    'clock': (2000, 0.104, 0.112, 0.130, 3.001, 0.115),
    'filed-call': (2000, 80.5, 95.25, 140.0, 900.125, 101.0),
    'grant': (2000, 6.25, 7.5, 9.0, 50.0, 7.75),
    'driver-read-1': (2000, 300.0, 340.0, 400.0, 2000.0, 350.0),
    'driver-read-8': (2000, 410.75, 455.5, 610.25, 3120.875, 470.125),
    'driver-read-128': (2000, 2210.75, 2440.5, 2800.0, 9000.0, 2500.0),
    'file-read-4k': (2000, 1300.0, 1450.25, 1700.0, 8000.0, 1500.0),
    'file-read-64k': (2000, 4500.0, 4800.0, 5200.0, 12000.0, 4900.0),
    'memfs-read-4k': (2000, 150.125, 170.0, 220.0, 1500.0, 180.0),
    'file-overwrite-4k': (2000, 1800.0, 2100.0, 2600.0, 15000.0, 2200.0),
}


def probe_line(name, values):
    calls, *rest = values
    numbers = ' '.join(f'{label} {value:.3f}' for label, value in zip(('min', 'median', 'p90', 'max', 'mean'), rest))
    return f'sdtiming: probe {name} {calls} calls, us {numbers}'


def board_log(values=VALUES):
    """A serial log like the board's: level tags, carriage returns and lines
    of other programs between the program's lines."""
    lines = ['[notice-0-3] other: probe clock 1 calls, us min 9.000 median 9.000 p90 9.000 max 9.000 mean 9.000',
             '[notice-1-7] sdtiming: arguments ""',
             '[notice-1-7] sdtiming: file /data/sdtiming.bin, 8388608 bytes',
             '[notice-1-7] sdtiming: start driver 12 device 34, 1048576 sectors of 512 B, 2000 calls, 500 appends',
             '[warn-1-7] sdtiming: open of /data/sdtiming-grow.bin refused with 5, retry 1']
    for name, entry in values.items():
        lines.append(f'[notice-{len(lines) % 4}-7] ' + probe_line(name, entry))
        lines.append('[notice-2-9] SDHCI 1: something else entirely')
    lines += ['[error-1-7] sdtiming: FAIL probe file-append-4k call 17 at offset 69632: 9, 0 bytes moved',
              '[notice-1-7] sdtiming: probe memfs-extra skipped, creating /x failed: 5',
              '[warn-1-7] sdtiming: delete of /data/sdtiming-grow.bin refused with 13, retry 2',
              '[error-1-7] sdtiming: result FAIL, 10 probes, 1 skipped, 1 errors']
    return '\r\n'.join(lines) + '\r\n'


def load_report(path):
    spec = importlib.util.spec_from_file_location(f'report_{abs(hash(str(path)))}', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_cases(report, script, output):
    """Run every case. Returns the names of the failed cases."""
    failures = []

    def case(name, condition):
        print(('ok   ' if condition else 'FAIL ') + name, flush=True)
        if not condition:
            failures.append(name)

    parsed = report.parse_log(board_log())
    probes = parsed['probes']
    case('every probe line read', sorted(probes) == sorted(VALUES))
    case('every value read', all(
        [probes[name]['calls']] + [probes[name][s] for s in ('min', 'median', 'p90', 'max', 'mean')]
        == list(values) for name, values in VALUES.items()))
    case('other programs ignored', probes['clock']['min'] == 0.104)
    case('skipped line read', parsed['skipped'] == {'memfs-extra': 'creating /x failed: 5'})
    case('FAIL line read', parsed['failures'] == [
        'sdtiming: FAIL probe file-append-4k call 17 at offset 69632: 9, 0 bytes moved'])
    case('retries read', len(parsed['retries']) == 2)
    case('file line read', parsed['file'] == {'path': '/data/sdtiming.bin', 'bytes': 8388608})
    case('start line read', parsed['start'] is not None and parsed['start']['driver'] == 12
         and parsed['start']['device'] == 34 and parsed['start']['sectors'] == 1048576
         and parsed['start']['sector_size'] == 512 and parsed['start']['calls'] == 2000
         and parsed['start']['appends'] == 500)
    case('result line read', parsed['result'] is not None and parsed['result']['verdict'] == 'FAIL'
         and parsed['result']['probes'] == 10 and parsed['result']['skipped'] == 1
         and parsed['result']['errors'] == 1 and parsed['runs'] == 1)

    second = dict(VALUES, **{'grant': (2000, 9.0, 9.5, 10.0, 11.0, 9.25)})
    twice = report.parse_log(board_log() + board_log(second))
    case('two boots: last line kept', twice['probes']['grant']['min'] == 9.0 and twice['runs'] == 2)

    case('all probes by default', report.expand_probes(None) == list(report.PROBE_NAMES))
    case('groups expand in run order', report.expand_probes('file-read,clock,driver-read') == [
        'clock', 'driver-read-1', 'driver-read-8', 'driver-read-128', 'file-read-4k', 'file-read-64k'])
    try:
        report.expand_probes('clock,nonsense')
        refused = False
    except ValueError:
        refused = True
    case('unknown probe refused', refused)

    for statistic in ('min', 'median'):
        def value(name):
            return VALUES[name][('min', 'median').index(statistic) + 1]
        split = report.derived_split(probes, statistic)
        expected = {
            'ipc_round_trip': value('filed-call'),
            'driver_and_card_4k': value('driver-read-8') - value('filed-call') - value('grant'),
            'per_extra_sector': (value('driver-read-128') - value('driver-read-8')) / 120,
            'filed_mfs_and_hops_4k': value('file-read-4k') - value('driver-read-8'),
            'mfs_hop_and_work_4k': value('file-read-4k') - value('memfs-read-4k') - value('driver-read-8'),
        }
        case(f'split from {statistic}', set(split) == set(expected)
             and all(abs(split[name] - expected[name]) < 1e-9 for name in expected))

    partial = {name: probe for name, probe in probes.items() if name != 'memfs-read-4k'}
    split = report.derived_split(partial, 'min')
    case('split without memfs-read-4k', split['mfs_hop_and_work_4k'] is None
         and split['filed_mfs_and_hops_4k'] is not None)

    table = report.format_table(parsed)
    case('table lists every probe', all(name in table for name in VALUES) and 'memfs-extra' not in table)
    skipped = report.parse_log(board_log().replace(
        'sdtiming: probe memfs-extra skipped', 'sdtiming: probe file-append-4k skipped'))
    case('table lists a skipped probe', 'file-append-4k     skipped, creating /x failed: 5'
         in report.format_table(skipped))
    case('table lists the split', 'driver_and_card_4k' in table and f'{455.5 - 95.25 - 7.5:.3f}' in table
         and f'{410.75 - 80.5 - 6.25:.3f}' in table)

    log = output / 'board.txt'
    log.write_text(board_log())
    written = output / 'board.json'
    completed = subprocess.run([sys.executable, str(script), str(log), '--json', str(written)],
                               capture_output=True, text=True)
    case('command line prints the table', completed.returncode == 0 and 'driver-read-128' in completed.stdout
         and 'result FAIL' in completed.stdout)
    data = json.loads(written.read_text()) if written.exists() else {}
    case('command line writes the JSON', data.get('split', {}).get('min', {}).get('ipc_round_trip') == 80.5)
    empty = output / 'empty.txt'
    empty.write_text('[notice-0-1] nothing here\n')
    completed = subprocess.run([sys.executable, str(script), str(empty)], capture_output=True, text=True)
    case('log without probe lines refused', completed.returncode != 0)
    return failures


# Each mutation must make at least one case fail. (name, before, after)
MUTATIONS = (
    ('grant not subtracted', "((1, 'driver-read-8'), (-1, 'filed-call'), (-1, 'grant'))",
     "((1, 'driver-read-8'), (-1, 'filed-call'))"),
    ('extra sectors divided by 128', "(-1, 'driver-read-8')), 120)", "(-1, 'driver-read-8')), 128)"),
    ('memfs not subtracted', "((1, 'file-read-4k'), (-1, 'memfs-read-4k'), (-1, 'driver-read-8'))",
     "((1, 'file-read-4k'), (-1, 'driver-read-8'))"),
    ('first probe line kept', "    for match in PROBE.finditer(text):\n        probes[match[1]] = {",
     "    for match in PROBE.finditer(text):\n        if match[1] in probes:\n            continue\n"
     "        probes[match[1]] = {"),
    ('missing probe taken as 0', "if all(probe in probes for _, probe in terms):", "if True:"),
    ('groups ignored', "        if item in PROBE_GROUPS:", "        if False:"),
)


def apply_mutation(text, name, before, after):
    assert text.count(before) == 1, name
    return text.replace(before, after)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--mutation-check', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vali-sdtiming-report-') as directory:
        output = Path(directory)
        failures = run_cases(load_report(REPORT), REPORT, output)
        if failures:
            raise SystemExit(f'sdtiming report test failed: {failures}')
        if not args.mutation_check:
            return
        for name, before, after in MUTATIONS:
            mutant = output / 'mutant'
            if mutant.exists():
                shutil.rmtree(mutant)
            mutant.mkdir()
            script = mutant / 'sdtiming_report.py'
            script.write_text(apply_mutation(REPORT.read_text(), name, before, after))
            print(f'--- mutation: {name}', flush=True)
            try:
                failures = run_cases(load_report(script), script, mutant)
            except Exception as error:  # A mutant that raises fails too.
                failures = [f'raised {error!r}']
            if not failures:
                raise SystemExit(f'Mutation did not fail a case: {name}')
            print('sdtiming report mutation rejected:', name, flush=True)


if __name__ == '__main__':
    main()
