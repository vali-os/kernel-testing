"""Read the SD timing test's lines from a serial log and print its table.

The SD timing test (testing/target/sdtiming) times the parts of a file read
one by one and logs one line per probe. This script finds those lines in any
serial log, from QEMU or from the board, prints a table of the probes and a
split of the 4 KiB file read derived from them. testing/sdtiming_boot.py uses
the same functions for its runs.

    python3 testing/sdtiming_report.py rpi4-boot.txt
    python3 testing/sdtiming_report.py rpi4-boot.txt --json rpi4-timing.json

All values are microseconds. Each probe line gives the minimum, median, 90th
percentile, maximum and mean of its calls. The split is computed twice, once
from the minimums and once from the medians:

  ipc_round_trip = filed-call
      One IPC round trip from the program to filed and back. It holds filed's
      handle check (one system call in filed) and one clock read.

  driver_and_card_4k = driver-read-8 - filed-call - grant
      What the SD driver and the card take for a 4 KiB read, without the IPC
      round trip and the grant token of the request. It assumes that a round
      trip to the driver costs the same as one to filed. It holds the driver's
      import of the token, its mapping of the buffer, its CPU work, the card
      command, the card's own access time, the data transfer on the bus and
      the interrupt and wake-up of the driver. These parts cannot be told
      apart from outside the driver.

  per_extra_sector = (driver-read-128 - driver-read-8) / 120
      What each sector beyond the eighth adds to one driver read: mostly the
      bus transfer of 512 bytes, plus the longer DMA setup. It assumes the
      cost grows in a straight line from 8 to 128 sectors. driver-read-1 is
      left out, because a single sector may use another card command.

  filed_mfs_and_hops_4k = file-read-4k - driver-read-8
      What a 4 KiB file read adds on top of the driver read that MFS sends:
      the program's grant token, the round trips to filed and to MFS, and
      the work of filed and MFS. It assumes MFS sends exactly one 8-sector
      read for the request. Anything else MFS reads, or a read MFS answers
      from a cache, makes this number wrong, and that cannot be seen from the
      log.

  mfs_hop_and_work_4k = file-read-4k - memfs-read-4k - driver-read-8
      The part of the line above that belongs to MFS: the round trip from
      filed to MFS and MFS's own work. memfs-read-4k goes through the
      program's grant token, the round trip to filed, filed's transfer
      handler and the import and mapping of the buffer, with an empty file.
      In a file read MFS imports and maps the buffer instead of filed, so the
      two are taken as equal. memfs-read-4k copies no data. A 4 KiB file read
      through MFS copies none either, because the card reads straight into
      the program's buffer.

The difference of two minimums (or two medians) is not the minimum (or
median) of a difference, because the samples of two probes are not paired.
The split is an estimate. It is most reliable from the minimums, where no
waiting is mixed in. A value below 0 means the estimate does not hold for
that log, for example because one of the probes waited more often.
"""
import argparse
import json
from pathlib import Path
import re

# The program's lines (see the top of testing/target/sdtiming/sdtiming.c).
# Serial logs carry a level tag before each message, so the lines are found
# anywhere in a log line.
PROBE = re.compile(r'sdtiming: probe (\S+) (\d+) calls, us min (\d+\.\d+) median (\d+\.\d+) p90 (\d+\.\d+) '
                   r'max (\d+\.\d+) mean (\d+\.\d+)')
SKIPPED = re.compile(r'sdtiming: probe (\S+) skipped, (.*?)\s*$')
FILE = re.compile(r'sdtiming: file (\S+), (\d+) bytes')
START = re.compile(r'sdtiming: start driver (\d+) device (\d+), (\d+) sectors of (\d+) B, (\d+) calls, '
                   r'(\d+) appends')
RESULT = re.compile(r'sdtiming: result (PASS|FAIL), (\d+) probes, (\d+) skipped, (\d+) errors')
FAIL = re.compile(r'sdtiming: FAIL .*?\s*$')
RETRY = re.compile(r'sdtiming: (open|delete) of (\S+) refused with (\d+), retry (\d+)')

# Every probe, in the order the program runs them.
PROBE_NAMES = ('clock', 'filed-call', 'grant', 'driver-read-1', 'driver-read-8', 'driver-read-128',
               'file-read-4k', 'file-read-64k', 'memfs-read-4k', 'file-overwrite-4k', 'file-append-4k')
# The names --probes accepts besides the probe names, and what they select.
PROBE_GROUPS = {
    'driver-read': ('driver-read-1', 'driver-read-8', 'driver-read-128'),
    'file-read': ('file-read-4k', 'file-read-64k'),
}

STATISTICS = ('min', 'median', 'p90', 'max', 'mean')

# The derived split. Each entry is (name, terms, divisor): the value is the
# sum of sign * probe value over the terms, divided by divisor. The
# docstring above explains each one.
SPLIT = (
    ('ipc_round_trip', ((1, 'filed-call'),), 1),
    ('driver_and_card_4k', ((1, 'driver-read-8'), (-1, 'filed-call'), (-1, 'grant')), 1),
    ('per_extra_sector', ((1, 'driver-read-128'), (-1, 'driver-read-8')), 120),
    ('filed_mfs_and_hops_4k', ((1, 'file-read-4k'), (-1, 'driver-read-8')), 1),
    ('mfs_hop_and_work_4k', ((1, 'file-read-4k'), (-1, 'memfs-read-4k'), (-1, 'driver-read-8')), 1),
)


def expand_probes(text):
    """The probe names that a --probes list of the program selects, in run order."""
    if not text:
        return list(PROBE_NAMES)
    chosen = set()
    for item in text.split(','):
        if item in PROBE_GROUPS:
            chosen.update(PROBE_GROUPS[item])
        elif item in PROBE_NAMES:
            chosen.add(item)
        else:
            raise ValueError(f'unknown probe {item}')
    return [name for name in PROBE_NAMES if name in chosen]


def parse_log(text):
    """The program's lines in a serial log. Probes are keyed by name. A
    probe that shows up more than once (a log of several boots) keeps its
    last line, and the count of runs is in 'runs'."""
    probes = {}
    for match in PROBE.finditer(text):
        probes[match[1]] = {
            'calls': int(match[2]),
            **{name: float(match[3 + index]) for index, name in enumerate(STATISTICS)},
            'line': match.group(0),
        }
    skipped = {}
    failures = []
    retries = []
    for line in text.splitlines():
        match = SKIPPED.search(line)
        if match:
            skipped[match[1]] = match[2]
        match = FAIL.search(line)
        if match:
            failures.append(match.group(0))
        match = RETRY.search(line)
        if match:
            retries.append(match.group(0))
    starts = list(START.finditer(text))
    files = list(FILE.finditer(text))
    results = list(RESULT.finditer(text))
    start = starts[-1] if starts else None
    file = files[-1] if files else None
    result = results[-1] if results else None
    return {
        'runs': len(results),
        'start': {'driver': int(start[1]), 'device': int(start[2]), 'sectors': int(start[3]),
                  'sector_size': int(start[4]), 'calls': int(start[5]), 'appends': int(start[6]),
                  'line': start.group(0)} if start else None,
        'file': {'path': file[1], 'bytes': int(file[2])} if file else None,
        'probes': probes,
        'skipped': skipped,
        'failures': failures,
        'retries': retries,
        'result': {'verdict': result[1], 'probes': int(result[2]), 'skipped': int(result[3]),
                   'errors': int(result[4]), 'line': result.group(0)} if result else None,
    }


def derived_split(probes, statistic):
    """The split of SPLIT for one statistic ('min' or 'median'). A value is
    None when a probe it needs is missing."""
    split = {}
    for name, terms, divisor in SPLIT:
        if all(probe in probes for _, probe in terms):
            split[name] = sum(sign * probes[probe][statistic] for sign, probe in terms) / divisor
        else:
            split[name] = None
    return split


def format_table(parsed):
    """The probes as a text table, then the split, for a person to read."""
    lines = [f'{"probe":<18} {"calls":>6} ' + ' '.join(f'{name:>11}' for name in STATISTICS) + '   (us)']
    for name in PROBE_NAMES:
        if name in parsed['probes']:
            probe = parsed['probes'][name]
            lines.append(f'{name:<18} {probe["calls"]:>6} ' + ' '.join(f'{probe[s]:>11.3f}' for s in STATISTICS))
        elif name in parsed['skipped']:
            lines.append(f'{name:<18} skipped, {parsed["skipped"][name]}')
    lines.append('')
    lines.append(f'{"derived split":<24} {"from min":>11} {"from median":>11}   (us)')
    by_minimum = derived_split(parsed['probes'], 'min')
    by_median = derived_split(parsed['probes'], 'median')
    for name, _, _ in SPLIT:
        values = [by_minimum[name], by_median[name]]
        lines.append(f'{name:<24} ' + ' '.join('{:>11}'.format('-' if value is None else f'{value:.3f}')
                                                 for value in values))
    return '\n'.join(lines)


def report(parsed):
    """The parsed lines with both splits, as written to JSON."""
    return {**parsed, 'split': {'min': derived_split(parsed['probes'], 'min'),
                                'median': derived_split(parsed['probes'], 'median')}}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('log', type=Path, help='Serial log with the program\'s lines')
    parser.add_argument('--json', type=Path, help='Also write the parsed lines and the split to this file')
    args = parser.parse_args()
    parsed = parse_log(args.log.read_text(errors='replace'))
    if not parsed['probes'] and not parsed['skipped']:
        raise SystemExit(f'{args.log} holds no sdtiming probe lines')
    if parsed['runs'] > 1:
        print(f'{args.log} holds {parsed["runs"]} runs, the table shows the last line of each probe')
    print(format_table(parsed))
    for line in parsed['failures']:
        print(line)
    if parsed['result']:
        print(parsed['result']['line'])
    if args.json:
        args.json.write_text(json.dumps(report(parsed), indent=2) + '\n')


if __name__ == '__main__':
    main()
