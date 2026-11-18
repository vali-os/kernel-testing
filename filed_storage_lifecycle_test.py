"""Register, unregister and register again device storage in filed, with unregister
requests arriving during registration, on a POSIX host with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-filed-storage-lifecycle-') as directory:
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
                ('testing/filed_storage_lifecycle_test.c', 'librt/libds/list.c',
                 'librt/libds/guid.c')]
    command += ['-o', binary]
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True)
    if args.mutation_check:
        storage = 'services/filed/storage/storage.c'
        filesystem = 'services/filed/filesystem.c'
        for path, define, before, after, reason in (
            (storage, 'FILED_STORAGE_SOURCE',
             '    __StorageRemoveDirectory(storage);\n', '',
             'device directory left after unregister'),
            (filesystem, 'FILED_FILESYSTEM_SOURCE',
             '    oserr = VFSNodeUnlink(fsScope, cpath);', '    oserr = OS_EOK;',
             'filesystem mount directory left after unmount'),
            (filesystem, 'FILED_FILESYSTEM_SOURCE',
             '        __RemoveMountDirectory(fsScope, nodeHandle, path);\n', '',
             'mount directory left after a failed mount'),
            (storage, 'FILED_STORAGE_SOURCE',
             '    if (fsStorage->State == VFSSTORAGE_STATE_INITIALIZING) {', '    if (false) {',
             'unregister frees the storage while registration uses it'),
            (storage, 'FILED_STORAGE_SOURCE',
             '    if (storage->UnregisterPending) {', '    if (false) {',
             'unregister during registration is lost'),
            (filesystem, 'FILED_FILESYSTEM_SOURCE',
             '        VFSInterfaceDelete(fileSystem->Interface);\n', '',
             'interface not released on disconnect'),
            (filesystem, 'FILED_FILESYSTEM_SOURCE',
             '    VFSIdentifierFree(fileSystem->Storage, fileSystem->ID);\n', '',
             'filesystem identifier not returned'),
            (storage, 'FILED_STORAGE_SOURCE',
             '        item = list_front(&storage->Filesystems);\n',
             '        list_clear(&storage->Filesystems, (void (*)(element_t*, void*))(void (*)(void))FileSystemDestroy, NULL);\n'
             '        item = list_front(&storage->Filesystems);\n',
             'filesystems destroyed under the list spinlock'),
            (filesystem, 'FILED_FILESYSTEM_SOURCE',
             '            fileSystem->MountScope,\n            fileSystem->MountHandle\n        );',
             '            fileSystem->VFS,\n            fileSystem->MountHandle\n        );',
             'unmount in the filesystem\'s own VFS instead of the mount scope'),
        ):
            source = ROOT / path
            text = source.read_text()
            assert text.count(before) == 1, before
            mutant = output / source.name
            mutant.write_text(text.replace(before, after))
            subprocess.run(command + [f'-D{define}="{mutant}"'], check=True)
            if subprocess.run([binary], capture_output=True).returncode == 0:
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: ' + reason, flush=True)
