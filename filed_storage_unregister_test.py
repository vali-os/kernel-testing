"""Register and unregister device storage in filed on a POSIX host with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-filed-storage-unregister-') as directory:
    output = Path(directory)
    for service in ('process', 'file', 'device', 'storage', 'usb', 'contracts/filesystem',
                    'contracts/usbhost'):
        subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                        '--service', str(ROOT / 'protocols' / (service + '.gr')),
                        '--out', directory, '--lang-c', '--client', '--server'], check=True)
    command = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
               '-DVALI', '-DTESTING', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
               '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               '-Wno-unused-parameter', '-Wno-unused-but-set-parameter', '-Wno-sign-compare',
               '-Wno-unknown-warning-option', '-ffunction-sections', '-fdata-sections',
               '-Wl,--gc-sections', '-g',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I' + directory]
    command += ['-I' + str(ROOT / path) for path in
                ('testing/include', 'services/filed/include', 'librt/libos/include',
                 'librt/libusb/include', 'librt/libddk/include', 'librt/libds/include',
                 'librt/libgracht/include')]
    binary = str(output / 'test')
    command += [str(ROOT / path) for path in
                ('testing/filed_storage_unregister_test.c', 'librt/libds/list.c')]
    command += ['-o', binary]
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True)
    if args.mutation_check:
        source = ROOT / 'services/filed/storage/storage.c'
        text = source.read_text()
        for before, after, reason in (
            ('    ELEMENT_INIT(&storage->ListHeader, (uintptr_t)deviceId, storage);\n', '',
             'disk list key not set to the device ID'),
            ('ELEMENT_INIT(&storage->ListHeader, (uintptr_t)deviceId, storage);',
             'ELEMENT_INIT(&storage->ListHeader, (uintptr_t)deviceId, 0);',
             'disk list value not set to the storage'),
            ('    VFSStorageDelete(storage);\n}', '    free(storage);\n}',
             'storage freed without destroying its filesystems'),
        ):
            assert text.count(before) == 1, before
            mutant = output / 'storage.c'
            mutant.write_text(text.replace(before, after))
            subprocess.run(command + [f'-DFILED_STORAGE_SOURCE="{mutant}"'], check=True)
            if subprocess.run([binary], capture_output=True).returncode == 0:
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: ' + reason, flush=True)
