"""Test ioset_ctrl event validation and set updates with the target headers on a POSIX host."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-ioset-ctrl-') as directory:
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DVALI', '-DTESTING',
             '-DMOLLENOS', '-D__time64_t=int64_t', '-DNDEBUG', '-fms-extensions',
             '-Wno-macro-redefined', '-ffunction-sections', '-fdata-sections',
             '-Wl,--gc-sections', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-g']
    flags += ['-I' + str(ROOT / path) for path in
              ('librt/libc/include', 'librt/libos/include', 'librt/libds/include',
               'librt/libddk/include')]
    binary = str(Path(directory) / 'test')
    command = flags + [str(ROOT / 'testing/ioset_ctrl_test.c'), '-o', binary]
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True, timeout=30)
    if args.mutation_check:
        # The old check rejected a NULL event for every operation.
        source = (ROOT / 'librt/libc/compat/ioset.c').read_text()
        anchor = 'if (!event && op != IOSET_DEL) {'
        assert source.count(anchor) == 1
        mutant = Path(directory) / 'ioset-mutant.c'
        mutant.write_text(source.replace(anchor, 'if (!event) {'))
        subprocess.run(command + ['-I' + str(ROOT / 'librt/libc/compat'),
                       f'-DIOSET_SOURCE="{mutant}"'], check=True)
        if subprocess.run([binary], capture_output=True, timeout=30).returncode == 0:
            raise RuntimeError('Undetected mutation: NULL removal rejected')
        print('Rejected mutation: NULL removal rejected')

        # The old lookup never moved past the first entry, so IOSET_MOD on a
        # later entry looped forever. A timeout counts as caught.
        anchor = '            return itr;\n        }\n        itr = itr->link;\n    }\n    return NULL;'
        assert source.count(anchor) == 1
        mutant.write_text(source.replace(anchor, '            return itr;\n        }\n    }\n    return NULL;'))
        subprocess.run(command + ['-I' + str(ROOT / 'librt/libc/compat'),
                       f'-DIOSET_SOURCE="{mutant}"'], check=True)
        try:
            result = subprocess.run([binary], capture_output=True, timeout=10)
            if result.returncode == 0:
                raise RuntimeError('Undetected mutation: lookup stops at the first entry')
        except subprocess.TimeoutExpired:
            pass
        print('Rejected mutation: lookup stops at the first entry')

        # IOSET_MOD used to change only the events and keep the old data.
        anchor = ('        entry->event.events = event->events;\n'
                  '        entry->event.data   = event->data;\n'
                  '        return 0;\n    } else if (op == IOSET_DEL) {')
        assert source.count(anchor) == 1
        mutant.write_text(source.replace(anchor, anchor.replace(
            '        entry->event.data   = event->data;\n', '')))
        subprocess.run(command + ['-I' + str(ROOT / 'librt/libc/compat'),
                       f'-DIOSET_SOURCE="{mutant}"'], check=True)
        if subprocess.run([binary], capture_output=True, timeout=30).returncode == 0:
            raise RuntimeError('Undetected mutation: IOSET_MOD keeps the old data')
        print('Rejected mutation: IOSET_MOD keeps the old data')

        # Adding to our copy before the kernel agreed left an entry behind
        # when the kernel refused.
        anchor = '            free(entry);\n            return OsErrToErrNo(oserr);'
        assert source.count(anchor) == 1
        mutant.write_text(source.replace(anchor,
            '            entry->iod = iod;\n            entry->event = *event;\n'
            '            entry->link = NULL;\n            ioset_append(ios, entry);\n'
            '            return OsErrToErrNo(oserr);'))
        subprocess.run(command + ['-I' + str(ROOT / 'librt/libc/compat'),
                       f'-DIOSET_SOURCE="{mutant}"'], check=True)
        if subprocess.run([binary], capture_output=True, timeout=30).returncode == 0:
            raise RuntimeError('Undetected mutation: refused add kept in the set')
        print('Rejected mutation: refused add kept in the set')
