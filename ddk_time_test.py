"""Check the driver time helpers in librt/libddk/utils/time.c and the usched delay helpers in
librt/libos/uthreads/deadline.c on a POSIX host with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TIME_SOURCE = ROOT / 'librt/libddk/utils/time.c'
DEADLINE_SOURCE = ROOT / 'librt/libos/uthreads/deadline.c'

# Each mutation must make an assertion in ddk_time_test.c fail, or make the
# sanitizers stop the test.
MUTATIONS = (
    (TIME_SOURCE, 'DDK_TIME_SOURCE',
     '    if (microseconds > g_lastReadUs[source]) {',
     '    if (true) {',
     'an older read replaces the stored value'),
    (TIME_SOURCE, 'DDK_TIME_SOURCE',
     '            source, oserr);\n        return __LoadLastRead(source);',
     '            source, oserr);\n        return 0;',
     'a failed read returns zero instead of the last value'),
    (TIME_SOURCE, 'DDK_TIME_SOURCE',
     '    if (timestamp.Seconds < 0 || timestamp.Nanoseconds < 0) {',
     '    if (false) {',
     'a negative time is used'),
    (TIME_SOURCE, 'DDK_TIME_SOURCE',
     '    if ((unsigned int)source >= __TIME_SOURCE_COUNT) {',
     '    if (false) {',
     'an unknown source is read'),
    (TIME_SOURCE, 'DDK_TIME_SOURCE',
     '    return DDKGetTimeUs(source) / USEC_PER_MSEC;',
     '    return (DDKGetTimeUs(source) + USEC_PER_MSEC - 1) / USEC_PER_MSEC;',
     'milliseconds rounded up'),
    (TIME_SOURCE, 'DDK_TIME_SOURCE',
     '    if (microseconds == 0) {\n        return OS_EOK;\n    }\n',
     '',
     'a zero delay is passed to OSStall'),
    (TIME_SOURCE, 'DDK_TIME_SOURCE',
     '    duration.QuadPart = (uint64_t)microseconds * NSEC_PER_USEC;',
     '    duration.QuadPart = (uint32_t)(microseconds * 1000U);',
     'stall wraps around in 32 bits'),
    (DEADLINE_SOURCE, 'USCHED_DEADLINE_SOURCE',
     '    until->tv_nsec = (long)(nanoseconds % NSEC_PER_SEC);',
     '    until->tv_nsec = (long)nanoseconds;',
     'nanoseconds not carried into seconds'),
    (DEADLINE_SOURCE, 'USCHED_DEADLINE_SOURCE',
     '    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {\n        return -1;\n    }\n',
     '    (void)timespec_get(&now, TIME_UTC);\n',
     'failed clock read ignored'),
    (DEADLINE_SOURCE, 'USCHED_DEADLINE_SOURCE',
     '    if (usched_deadline_after_us(microseconds, &until) != 0) {\n        return -1;\n    }\n',
     '    (void)usched_deadline_after_us(microseconds, &until);\n',
     'job sleeps without a deadline'),
)


def compile_command(directory, binary):
    command = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
               '-DVALI', '-DTESTING', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
               '-fms-extensions', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-sign-compare',
               '-Wno-unknown-warning-option', '-g', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-I' + directory]
    command += ['-I' + str(ROOT / path) for path in
                ('testing/include', 'librt/libos/include', 'librt/libddk/include')]
    command += ['-idirafter', str(ROOT / 'librt/libc/include')]
    command += [str(ROOT / 'testing/ddk_time_test.c'), '-Wl,--wrap=timespec_get', '-o', binary]
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation-check', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vali-ddk-time-') as directory:
        output = Path(directory)
        binary = str(output / 'test')
        command = compile_command(directory, binary)
        subprocess.run(command, check=True)
        subprocess.run([binary], check=True)
        if not args.mutation_check:
            return
        for source, macro, before, after, reason in MUTATIONS:
            text = source.read_text()
            assert text.count(before) == 1, before
            mutant = output / source.name
            mutant.write_text(text.replace(before, after))
            subprocess.run(command + [f'-D{macro}="{mutant}"'], check=True)
            if subprocess.run([binary], capture_output=True).returncode == 0:
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: ' + reason, flush=True)


if __name__ == '__main__':
    main()
