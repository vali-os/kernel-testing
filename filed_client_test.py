"""Test file open/import and dup/close with production libc and its local registry."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-filed-client-') as directory:
    output = Path(directory)
    for service in ('file', 'process', 'device', 'contracts/filesystem', 'contracts/usbhost'):
        subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                        '--service', str(ROOT / 'protocols' / (service + '.gr')),
                        '--out', directory, '--lang-c', '--client', '--server'], check=True)
    # Rename only the exported symbol; a macro would also rename Ops->close.
    close_source = (ROOT / 'librt/libc/io/close.c').read_text()
    assert close_source.count('\nclose(\n') == 1
    (output / 'test-close.c').write_text(close_source.replace('\nclose(\n', '\ntest_close(\n'))
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DVALI', '-DTESTING',
             '-DMOLLENOS', '-D__time64_t=int64_t', '-DNDEBUG', '-fms-extensions',
             '-Wno-macro-redefined', '-ffunction-sections', '-fdata-sections',
             '-Wl,--gc-sections', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-mllvm', '-asan-globals=0', '-g', '-I' + directory]
    flags += ['-I' + str(ROOT / path) for path in
              ('librt/libc/include', 'librt/libos/include', 'librt/libds/include',
               'librt/libddk/include', 'librt/libusb/include', 'librt/libgracht/include')]
    binary = str(output / 'test')
    command = flags + [str(ROOT / 'testing/filed_client_test.c'), '-o', binary]
    environment = dict(os.environ)
    environment['ASAN_OPTIONS'] = environment.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True, env=environment)
    if args.mutation_check:
        source = (ROOT / 'librt/libc/stdio/core/core.c').read_text()
        mutant = output / 'core-mutant.c'
        mutant.write_text(source.replace('oserr = OSHandlesAcquire(&handle->OSHandle);', 'oserr = OS_EOK;')
                          .replace('#include "private.h"', '#include "' + str(ROOT / 'librt/libc/stdio/core/private.h') + '"'))
        subprocess.run(command + [f'-DDESCRIPTOR_SOURCE="{mutant}"'], check=True)
        result = subprocess.run([binary], env=environment, capture_output=True)
        if result.returncode == 0:
            raise RuntimeError('Missing local duplicate reference was not detected')
        print('Rejected missing local duplicate reference')
