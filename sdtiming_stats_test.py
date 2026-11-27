"""Host test of the SD timing test's statistics code.

The target program's statistics (testing/target/sdtiming/statistics.c) are
built for the host with testing/sdtiming_stats_host.c and compared with a
reference in Python:

- Sorting: random, sorted, reversed and equal samples come back sorted.
- Percentiles: the nearest-rank percentile of statistics.h for every p from
  1 to 100, for counts from 1 to 41 and for 2000 samples. The median of an
  even count is the lower middle sample.
- Mean: exact, rounded to the nearest nanosecond (a half rounds up), also
  for samples near 2^64, where a plain sum would overflow.
- No samples: TimingSummarize fails.
- The result line: the exact text for known values, three decimals for the
  nanoseconds, -1 when the buffer is one byte too small, and the line
  testing/sdtiming_report.py reads back to the same numbers. With the
  "sdtiming: " prefix, a line of values below 100 ms (the maximum below
  10 s) stays below 128 characters, and a line of values at 100 s still fits
  the program's line buffer and the system log's message limit.
- The target program and statistics.c compile for i386, AMD64 and AArch64
  with the Vali compiler, when it is installed and a build directory has the
  generated protocol headers.

With --mutation-check, each mutation of statistics.c must make at least one
case fail.
"""
import argparse
from fractions import Fraction
import math
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TESTING = ROOT / 'testing'
PROGRAM_DIRECTORY = TESTING / 'target/sdtiming'
HELPER_SOURCE = TESTING / 'sdtiming_stats_host.c'

sys.path.insert(0, str(TESTING))
import sdtiming_report  # noqa: E402

# The lines of a real run stay below LINE_LIMIT characters, see sdtiming.c.
# The program formats a probe line into LINE_CAPACITY bytes (its
# __LINE_CAPACITY), and the system log keeps MESSAGE_MAX characters of a
# message (OSKERNELLOG_MESSAGE_MAX).
LINE_LIMIT = 128
LINE_CAPACITY = 160
MESSAGE_MAX = 255
PREFIX = 'sdtiming: '
# The longest probe name and the largest call count the program allows.
LONGEST_NAME = max(sdtiming_report.PROBE_NAMES, key=len)
MOST_CALLS = 100000

# Each mutation must make at least one case fail. (name, before, after)
MUTATIONS = (
    ('rank rounded down', '(((uint64_t)percent * count) + 99U) / 100U', '((uint64_t)percent * count) / 100U'),
    ('rank one too high', 'return samples[rank - 1];', 'return samples[rank < count ? rank : rank - 1];'),
    ('median is the 51st percentile', 'TimingPercentile(samples, count, 50)', 'TimingPercentile(samples, count, 51)'),
    ('p90 is the 89th percentile', 'TimingPercentile(samples, count, 90)', 'TimingPercentile(samples, count, 89)'),
    ('remainder not carried', '            whole++;\n            remainder -= count;',
     '            remainder -= count;'),
    ('mean rounded down', 'if (remainder * 2U >= count) {', 'if (0) {'),
    ('mean half rounded down', 'if (remainder * 2U >= count) {', 'if (remainder * 2U > count) {'),
    ('sorted from large to small', 'return leftValue > rightValue ? 1 : 0;', 'return leftValue > rightValue ? -1 : 1;'),
    ('maximum is the next to last sample', 'summary->Maximum = samples[count - 1];',
     'summary->Maximum = samples[count > 1 ? count - 2 : 0];'),
    ('nanoseconds with two digits', 'median %llu.%03llu', 'median %llu.%02llu'),
    ('truncated line accepted', '(size_t)length >= capacity', '(size_t)length > capacity'),
    ('empty sample list accepted', '    if (count == 0) {\n        return -1;\n    }\n\n    qsort',
     '    if (count == 0) {\n        count = 1;\n    }\n\n    qsort'),
)


def build_helper(source_directory, output):
    """Build the host helper from the statistics.c in source_directory."""
    binary = output / 'stats_host'
    compiler = os.environ.get('CC', 'clang')
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-g', '-I' + str(source_directory), str(HELPER_SOURCE),
                    str(source_directory / 'statistics.c'), '-o', str(binary)], check=True)
    return binary


def run_helper(binary, samples, name='driver-read-8', capacity=160):
    """The helper's output for the samples, as a dictionary."""
    completed = subprocess.run([str(binary), name, str(capacity)], input='\n'.join(str(s) for s in samples) + '\n',
                               capture_output=True, text=True, check=True)
    output = {'percentiles': {}, 'sorted': []}
    for line in completed.stdout.splitlines():
        words = line.split(' ')
        if words[0] == 'summary':
            output['status'] = int(words[1])
            if len(words) > 2:
                output['summary'] = [int(word) for word in words[2:]]
        elif words[0] == 'percentile':
            output['percentiles'][int(words[1])] = int(words[2])
        elif words[0] == 'sorted':
            output['sorted'].append(int(words[1]))
        elif words[0] == 'line':
            output['line_length'] = int(words[1])
            output['line'] = line.split(' ', 2)[2] if output['line_length'] >= 0 else None
    return output


def nearest_rank(ordered, percent):
    """The reference percentile: the sample at rank ceil(p * n / 100)."""
    rank = max(1, math.ceil(Fraction(percent * len(ordered), 100)))
    return ordered[rank - 1]


def exact_mean(samples):
    """The reference mean: rounded to the nearest nanosecond, a half up."""
    mean = Fraction(sum(samples), len(samples))
    return math.floor(mean + Fraction(1, 2))


def microseconds(nanoseconds):
    return f'{nanoseconds // 1000}.{nanoseconds % 1000:03d}'


def run_cases(binary):
    """Run every case. Returns the names of the failed cases."""
    failures = []
    generator = random.Random(20261125)

    def case(name, condition):
        print(('ok   ' if condition else 'FAIL ') + name, flush=True)
        if not condition:
            failures.append(name)

    def check_samples(name, samples):
        output = run_helper(binary, samples)
        ordered = sorted(samples)
        expected = [len(samples), ordered[0], nearest_rank(ordered, 50), nearest_rank(ordered, 90), ordered[-1],
                    exact_mean(samples)]
        case(f'{name}: sorted', output['sorted'] == ordered)
        case(f'{name}: summary', output.get('summary') == expected)
        case(f'{name}: every percentile', all(output['percentiles'][p] == nearest_rank(ordered, p)
                                              for p in range(1, 101)))
        return output

    for count in range(1, 42):
        samples = [generator.randrange(500, 20000000) for _ in range(count)]
        check_samples(f'{count} random samples', samples)
    check_samples('2000 random samples', [generator.randrange(1000, 3000000) for _ in range(2000)])
    check_samples('sorted samples', list(range(1000, 1100)))
    check_samples('reversed samples', list(range(1100, 1000, -1)))
    check_samples('equal samples', [4242] * 17)

    output = run_helper(binary, [10, 20, 30, 40])
    case('median of an even count is the lower middle', output['summary'][2] == 20)
    output = run_helper(binary, [1, 2])
    case('mean of a half rounds up', output['summary'][5] == 2)
    output = run_helper(binary, [1, 1, 2])
    case('mean below a half rounds down', output['summary'][5] == 1)

    huge = [2 ** 64 - 1, 2 ** 64 - 3, 2 ** 64 - 7, 2 ** 63]
    check_samples('samples near 2^64', huge)
    check_samples('100000 large samples', [2 ** 64 - 1 - generator.randrange(0, 2 ** 40) for _ in range(100000)])

    output = run_helper(binary, [])
    case('no samples are refused', output.get('status') == -1 and 'summary' not in output)

    samples = [412345, 455012, 610234, 3120812, 470123]
    output = run_helper(binary, samples, name='driver-read-8')
    expected = ('probe driver-read-8 5 calls, us min 412.345 median 470.123 p90 3120.812 max 3120.812 '
                'mean 1013.705')
    case('line text', output.get('line') == expected and output['line_length'] == len(expected))
    output = run_helper(binary, [1, 20, 300], name='clock')
    case('line with values below a microsecond', output.get('line') ==
         'probe clock 3 calls, us min 0.001 median 0.020 p90 0.300 max 0.300 mean 0.107')
    output = run_helper(binary, samples, name='driver-read-8', capacity=len(expected))
    case('line one byte too long for the buffer is refused', output.get('line_length') == -1)
    output = run_helper(binary, samples, name='driver-read-8', capacity=len(expected) + 1)
    case('line that just fits is written', output.get('line') == expected)

    # The report script must read the line back to the same numbers.
    parsed = sdtiming_report.parse_log('[notice-0-12] sdtiming: ' + expected + '\n')
    probe = parsed['probes'].get('driver-read-8', {})
    case('report reads the line back', probe.get('calls') == 5 and probe.get('min') == 412.345
         and probe.get('median') == 470.123 and probe.get('mean') == 1013.705)

    # A long line of a real run: the longest name, the most calls, values
    # just below 100 ms and a maximum just below 10 s. It must stay below
    # 128 characters.
    long_run = [99999999] * (MOST_CALLS - 1) + [9999999999]
    output = run_helper(binary, long_run, name=LONGEST_NAME)
    longest = PREFIX + (output.get('line') or '')
    case(f'line of values below 100 ms has {len(longest)} characters, below {LINE_LIMIT}',
         output.get('line') is not None and len(longest) < LINE_LIMIT)
    case('report reads that line back', LONGEST_NAME in sdtiming_report.parse_log(longest)['probes'])

    # The longest line at all: every value at 100 s. It must fit the
    # program's line buffer and the system log's message limit.
    slow = [100 * 10 ** 9 - index for index in range(MOST_CALLS)]
    output = run_helper(binary, slow, name=LONGEST_NAME, capacity=LINE_CAPACITY)
    longest = PREFIX + (output.get('line') or '')
    case(f'line of values at 100 s has {len(longest)} characters and fits the program\'s buffer',
         output.get('line') is not None and len(longest) <= MESSAGE_MAX)
    case('line keeps every nanosecond', microseconds(99999900001) in (output.get('line') or ''))
    return failures


def find_protocols():
    """A build directory's generated protocol headers, which the program
    includes. They do not depend on the architecture."""
    for candidate in [ROOT / 'build/protocols'] + sorted((ROOT / 'build').glob('*/protocols')):
        if (candidate / 'sys_file_service_client.h').exists():
            return candidate
    return None


def cross_compile(output):
    """Compile the target program for each architecture, without linking."""
    compiler = os.environ.get('VALI_CLANG', '/usr/local/valicc/bin/clang')
    if not Path(compiler).exists():
        print('Vali compiler not found, cross-compilation skipped:', compiler, flush=True)
        return
    protocols = find_protocols()
    if protocols is None:
        print('No build directory with generated protocol headers, cross-compilation skipped', flush=True)
        return
    includes = ['-I' + str(ROOT / path) for path in (
        'librt/libc/include', 'librt/libos/include', 'librt/libddk/include', 'librt/libds/include',
        'librt/libgracht/include')] + ['-I' + str(protocols)]
    for arch in ('i386', 'amd64', 'aarch64'):
        flags = [compiler, f'--target={arch}-uml-vali', '-fms-extensions', '-ffreestanding', '-nostdlibinc', '-O2',
                 '-Wall', '-Werror', '-Wno-address-of-packed-member', '-Wno-unused-function',
                 '-DTIMING_ARGUMENTS="--calls 10 --probes clock,driver-read"']
        if arch == 'aarch64':
            flags += ['-march=armv8-a', '-mno-outline-atomics']
        for source in ('statistics.c', 'sdtiming.c'):
            subprocess.run(flags + includes + ['-c', str(PROGRAM_DIRECTORY / source),
                                               '-o', str(output / f'{arch}-{source}.o')], check=True)
        print('Target program compilation:', arch, 'passed', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--mutation-check', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vali-sdtiming-stats-') as directory:
        output = Path(directory)
        failures = run_cases(build_helper(PROGRAM_DIRECTORY, output))
        if failures:
            raise SystemExit(f'sdtiming statistics test failed: {failures}')
        cross_compile(output)
        if not args.mutation_check:
            return

        for name, before, after in MUTATIONS:
            mutant = output / 'mutant'
            if mutant.exists():
                shutil.rmtree(mutant)
            mutant.mkdir()
            shutil.copy(PROGRAM_DIRECTORY / 'statistics.h', mutant / 'statistics.h')
            original = (PROGRAM_DIRECTORY / 'statistics.c').read_text()
            assert original.count(before) == 1, name
            (mutant / 'statistics.c').write_text(original.replace(before, after))
            print(f'--- mutation: {name}', flush=True)
            try:
                failures = run_cases(build_helper(mutant, mutant))
            except subprocess.CalledProcessError as error:  # A mutant that crashes the helper fails too.
                failures = [f'helper failed: {error}']
            if not failures:
                raise SystemExit(f'Mutation did not fail a case: {name}')
            print('sdtiming mutation rejected:', name, flush=True)


if __name__ == '__main__':
    main()
