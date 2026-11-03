"""Test ordinary USB client storage and mapped IDE PIO transfers on a POSIX host."""
import argparse
import os
import shlex
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='client-shm-') as directory:
    # Keep host standard headers while using the actual target declarations.
    (Path(directory) / 'internal').mkdir()
    (Path(directory) / 'internal/_utils.h').write_text(
        (ROOT / 'librt/libc/include/internal/_utils.h').read_text())
    includes = ['testing/netadapter/include', 'testing/include', 'modules/storage/sata',
                'modules/storage/ide', 'librt/libos/include', 'librt/libgracht/include',
                'librt/libddk/include', 'librt/libds/include', 'librt/libusb/include',
                'build/windows/protocols']
    command = [os.environ.get('CC', 'clang'), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
               '-DVALI', '-DTESTING', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
               '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               '-Wno-unused-parameter', '-Wno-sign-compare', '-ffunction-sections', '-fdata-sections']
    command += ['-I' + directory] + ['-I' + str(ROOT / path) for path in includes]
    command += [str(ROOT / 'testing/client_shm_test.c'), str(ROOT / 'librt/libds/list.c'),
                '-Wl,--gc-sections', '-o', directory + '/test']
    command += shlex.split(os.environ.get('CLIENT_SHM_TEST_CFLAGS', ''))
    subprocess.run(command, check=True)
    subprocess.run([directory + '/test'], check=True)
    if args.mutation_check:
        source = (ROOT / 'modules/storage/ide/ide.c').read_text()
        anchor = 'if (status != OS_EOK) {\n        WARNING("IDE transfer buffer mapping failed: %u", status);'
        assert anchor in source
        mutant = Path(directory) / 'ide-mutant.c'
        mutant.write_text(source.replace(anchor, anchor.replace('!=', '==')))
        subprocess.run(command + [f'-DIDE_SOURCE="{mutant}"'], check=True)
        if subprocess.run([directory + '/test']).returncode == 0:
            raise RuntimeError('Undetected regression: inverted mapping cleanup')
        print('Rejected inverted mapping-cleanup mutation', flush=True)
