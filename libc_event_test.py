"""Test write() and read() on a libc event descriptor on a POSIX host."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-libc-event-') as directory:
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DVALI', '-DTESTING',
             '-DMOLLENOS', '-D__time64_t=int64_t', '-fms-extensions',
             '-Wno-macro-redefined', '-ffunction-sections', '-fdata-sections',
             '-Wl,--gc-sections', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-g']
    flags += ['-I' + str(ROOT / path) for path in
              ('librt/libc/include', 'librt/libos/include', 'librt/libds/include',
               'librt/libddk/include')]
    binary = str(Path(directory) / 'test')
    # The libos event code is a separate source, because both event files
    # define a structure named Event.
    command = flags + [str(ROOT / 'testing/libc_event_test.c'),
                       str(ROOT / 'librt/libc/io/write.c'),
                       str(ROOT / 'librt/libos/event.c'),
                       str(ROOT / 'librt/libos/error_codes.c'), '-o', binary]
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True, timeout=30)
    if args.mutation_check:
        source = (ROOT / 'librt/libc/compat/event.c').read_text()
        merged = ('    if (oserr == OS_EINCOMPLETE) {\n'
                  '        *bytes_written = sizeof(unsigned int);\n'
                  '        return OS_EOK;\n'
                  '    }\n')
        written = ('    *bytes_written = sizeof(unsigned int);\n'
                   '    return OS_EOK;\n'
                   '}\n')
        mutations = {
            # The old code returned OS_EINCOMPLETE, so write() failed.
            'merged write fails': source.replace(merged, ''),
            'merged write posts to the I/O sets': source.replace(
                merged, '    if (oserr == OS_EINCOMPLETE) {\n'
                        '        oserr = OS_EOK;\n'
                        '    }\n'),
            # The old code reported sizeof(size_t) for a semaphore event.
            'byte count of size_t': source.replace(
                written, written.replace('unsigned int', 'size_t')),
        }
        assert source.count(merged) == 1
        assert source.count(written) == 1
        mutant = Path(directory) / 'event-mutant.c'
        for name, body in mutations.items():
            mutant.write_text(body)
            subprocess.run(command + ['-I' + str(ROOT / 'librt/libc/compat'),
                           f'-DLIBC_EVENT_SOURCE="{mutant}"'], check=True)
            if subprocess.run([binary], capture_output=True, timeout=30).returncode == 0:
                raise RuntimeError('Undetected mutation: ' + name)
            print('Rejected mutation: ' + name)
