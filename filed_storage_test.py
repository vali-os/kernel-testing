"""Run filed's actual partition detection on a POSIX host with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='filed-storage-') as directory:
    includes = ['testing/netadapter/include', 'testing/include', 'services/filed/include',
                'librt/libos/include', 'librt/libddk/include', 'librt/libds/include',
                'librt/libgracht/include', 'build/windows/protocols']
    command = [os.environ.get('CC', 'clang'), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
               '-DVALI', '-DTESTING', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
               '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               '-Wno-unused-parameter', '-Wno-sign-compare', '-g',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    command += ['-I' + str(ROOT / path) for path in includes]
    command += [str(ROOT / path) for path in (
        'testing/filed_storage_test.c', 'services/filed/storage/layouts/detect.c',
        'services/filed/storage/layouts/gpt.c', 'librt/libds/guid.c')]
    mbr = ROOT / 'services/filed/storage/layouts/mbr.c'
    output = ['-o', directory + '/test']
    subprocess.run(command + [str(mbr)] + output, check=True)
    subprocess.run([directory + '/test'], check=True)
    if args.mutation_check:
        # Reintroduce the null sector argument without modifying the working tree.
        source = mbr.read_text()
        anchor = 'VFSStorageDetectFileSystem(storage, bufferHandle, buffer, &sector)'
        assert source.count(anchor) == 1
        mutant = Path(directory) / 'mbr-mutant.c'
        mutant.write_text(source.replace(anchor, anchor.replace('&sector', '(0 ? &sector : NULL)')))
        subprocess.run(command + [str(mutant)] + output, check=True)
        if subprocess.run([directory + '/test']).returncode == 0:
            raise RuntimeError('Undetected regression: null whole-disk sector')
        print('Rejected null-sector mutation', flush=True)
