"""Test OSEventLock and OSEventUnlock against a scripted futex on a POSIX host."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-os-event-') as directory:
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DVALI', '-DTESTING',
             '-DMOLLENOS', '-D__time64_t=int64_t', '-fms-extensions',
             '-Wno-macro-redefined', '-ffunction-sections', '-fdata-sections',
             '-Wl,--gc-sections', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-g']
    flags += ['-I' + str(ROOT / path) for path in
              ('librt/libc/include', 'librt/libos/include', 'librt/libds/include',
               'librt/libddk/include')]
    binary = str(Path(directory) / 'test')
    command = flags + [str(ROOT / 'testing/os_event_test.c'), '-o', binary]
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True, timeout=30)
    if args.mutation_check:
        source = (ROOT / 'librt/libos/event.c').read_text()
        failed_wait = ('            if (oserr != OS_EOK) {\n'
                       '                return oserr;\n'
                       '            }\n')
        refused_wait = ('            if (oserr == OS_EINTERRUPTED && '
                        'atomic_load(event->SyncAddress) != value) {\n'
                        '                continue;\n'
                        '            }\n')
        maximum = '(unsigned int)currentValue < event->MaxValue'
        mutations = {
            # The old code took a unit after a failed wait.
            'failed wait lowers the counter': source.replace(
                failed_wait, failed_wait.replace(
                    '                return oserr;\n',
                    '                atomic_fetch_sub(event->SyncAddress, 1);\n'
                    '                return oserr;\n')),
            'refused wait treated as a failure': source.replace(refused_wait, ''),
            'counter raised above its maximum': source.replace(
                maximum, '(unsigned int)currentValue <= event->MaxValue'),
        }
        assert source.count(failed_wait) == 1
        assert source.count(refused_wait) == 1
        assert source.count(maximum) == 1
        mutant = Path(directory) / 'event-mutant.c'
        for name, body in mutations.items():
            mutant.write_text(body)
            subprocess.run(command + ['-I' + str(ROOT / 'librt/libos'),
                           f'-DOS_EVENT_SOURCE="{mutant}"'], check=True)
            if subprocess.run([binary], capture_output=True, timeout=30).returncode == 0:
                raise RuntimeError('Undetected mutation: ' + name)
            print('Rejected mutation: ' + name)
