"""Check the busy retry in the DDK storage transfer on a POSIX host with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-ddk-storage-retry-') as directory:
    output = Path(directory)
    for service in ('process', 'contracts/storage'):
        subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                        '--service', str(ROOT / 'protocols' / (service + '.gr')),
                        '--out', directory, '--lang-c', '--client'], check=True)
    command = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
               '-DVALI', '-DTESTING', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
               '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-unknown-warning-option',
               '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-I' + directory]
    command += ['-I' + str(ROOT / path) for path in
                ('testing/include', 'librt/libos/include', 'librt/libddk/include',
                 'librt/libgracht/include')]
    command += ['-idirafter', str(ROOT / 'librt/libc/include')]
    binary = str(output / 'test')
    command += [str(ROOT / 'testing/ddk_storage_retry_test.c'), '-o', binary]
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True)
    if args.mutation_check:
        source = ROOT / 'librt/libddk/storage.c'
        for before, after, reason in (
            ('#define STORAGE_BUSY_RETRY_COUNT    50', '#define STORAGE_BUSY_RETRY_COUNT    49',
             'one retry too few'),
            ('#define STORAGE_BUSY_RETRY_DELAY_MS 20', '#define STORAGE_BUSY_RETRY_DELAY_MS 10',
             'wait between retries too short'),
            ('    *driverBusy = status == OS_EBUSY;', '    *driverBusy = status != OS_EOK;',
             'other driver errors retried'),
            ('    *driverBusy = status == OS_EBUSY;', '    *driverBusy = false;',
             'busy result not retried'),
            ('    if (!driverBusy || retries == STORAGE_BUSY_RETRY_COUNT) {',
             '    if ((!driverBusy && status != OS_EBUSY) || retries == STORAGE_BUSY_RETRY_COUNT) {',
             'local busy failure retried'),
            ('    OSGrantTokenRevoke(transfer.Token.ControlHandleID);\n', '',
             'grant token of an attempt not revoked'),
            ('        status = __StorageBusyWait();\n        if (status != OS_EOK) {\n'
             '            return status;\n        }\n', '',
             'retry sent without waiting'),
        ):
            text = source.read_text()
            assert text.count(before) == 1, before
            mutant = output / 'storage.c'
            mutant.write_text(text.replace(before, after))
            subprocess.run(command + [f'-DDDK_STORAGE_SOURCE="{mutant}"'], check=True)
            if subprocess.run([binary], capture_output=True).returncode == 0:
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: ' + reason, flush=True)
