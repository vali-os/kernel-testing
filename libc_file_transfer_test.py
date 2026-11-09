"""Test libc file I/O with managed SHM buffers using target headers."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-libc-file-') as directory:
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DVALI', '-DTESTING',
             '-DMOLLENOS', '-D__time64_t=int64_t', '-DNDEBUG', '-fms-extensions',
             '-Wno-macro-redefined', '-ffunction-sections', '-fdata-sections',
             '-Wl,--gc-sections', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-g']
    flags += ['-I' + str(ROOT / path) for path in
              ('librt/libc/include', 'librt/libos/include', 'librt/libds/include',
               'librt/libddk/include')]
    binary = str(Path(directory) / 'test')
    command = flags + [str(ROOT / 'testing/libc_file_transfer_test.c'), '-o', binary]
    environment = dict(os.environ)
    environment['ASAN_OPTIONS'] = environment.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True, env=environment)
    if args.mutation_check:
        source = (ROOT / 'librt/libc/io/open.c').read_text()
        anchor = 'if (transferred > requested) {'
        assert anchor in source
        mutant = Path(directory) / 'open-mutant.c'
        mutant.write_text(source.replace(anchor, 'if (transferred < requested) {'))
        subprocess.run(command + [f'-DFILE_IO_SOURCE="{mutant}"'], check=True)
        result = subprocess.run([binary], env=environment, capture_output=True)
        if result.returncode == 0:
            raise RuntimeError('Undetected oversized-result mutation')
        print('Rejected oversized-result mutation')
