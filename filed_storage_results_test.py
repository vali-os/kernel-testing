"""Check filed's sys_storage results and driver/device disk identity on a POSIX host with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-filed-storage-results-') as directory:
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
                ('testing/filed_storage_results_test.c', 'librt/libds/list.c')]
    command += ['-o', binary]
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True)
    if args.mutation_check:
        source = ROOT / 'services/filed/storage/storage.c'
        ok_result = '    __SendRegisterResult(message, driverId, deviceId, OS_EOK);\n'
        stat_query = '    oserr = VFSStorageQueryDeviceStats(storage);\n'
        unregister_tail = ('    __SendUnregisterResult(message, driverId, deviceId, OS_EOK);\n'
                           '    __StorageDestroy(fsStorage, forced);\n')
        for edits, reason in (
            ((('        if (storage->Protocol.Storage.Device.DriverID == driverId &&\n',
               '        if (true &&\n'),),
             'disk lookup ignores the driver ID'),
            ((('            !storage->UnregisterPending) {', '            true) {'),),
             'disk marked for deferred teardown still found'),
            ((('    if (__FindRegisteredDisk(driverId, deviceId) != NULL) {', '    if (false) {'),),
             'duplicate disk accepted'),
            (((ok_result, ''),),
             'no result for an accepted disk'),
            (((ok_result, ''), (stat_query, stat_query + ok_result)),
             'register result sent after the stat query'),
            ((('        __SendRegisterResult(message, driverId, deviceId, OS_EEXISTS);\n', ''),),
             'no result for a duplicate disk'),
            ((('        __SendRegisterResult(message, driverId, deviceId, OS_EINVALPARAMS);\n', ''),),
             'no result for an invalid driver ID'),
            ((('        __SendRegisterResult(message, driverId, deviceId, OS_EOOM);\n', ''),),
             'no result when the storage cannot be created'),
            ((('        __SendUnregisterResult(message, driverId, deviceId, OS_ENOENT);\n', ''),),
             'no result for an unknown disk'),
            ((('        __SendUnregisterResult(message, driverId, deviceId, OS_EOK);\n', ''),),
             'no result for a deferred unregister'),
            (((unregister_tail, '    __StorageDestroy(fsStorage, forced);\n'
                                '    __SendUnregisterResult(message, driverId, deviceId, OS_EOK);\n'),),
             'unregister result sent after the unmount'),
        ):
            text = source.read_text()
            for before, after in edits:
                assert text.count(before) == 1, before
                text = text.replace(before, after)
            mutant = output / 'storage.c'
            mutant.write_text(text)
            subprocess.run(command + [f'-DFILED_STORAGE_SOURCE="{mutant}"'], check=True)
            if subprocess.run([binary], capture_output=True).returncode == 0:
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: ' + reason, flush=True)
