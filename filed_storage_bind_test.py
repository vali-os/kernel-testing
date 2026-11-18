"""Bind mounts of registered disks and unregister during the stat query in filed,
with the real VFS code, on a POSIX host with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FILED = 'services/filed/'
# Production sources compiled as their own translation units. storage.c,
# filesystem.c and storage/utils.c are included by the test itself.
SOURCES = [FILED + path for path in (
    'scope.c', 'storage/storage_device.c', 'storage/storage_memory.c',
    'vfs/vfs.c', 'vfs/utils.c', 'vfs/bind.c', 'vfs/mount.c', 'vfs/memfs.c',
    'vfs/handlers/mkdir.c', 'vfs/handlers/open.c', 'vfs/handlers/close.c',
    'vfs/handlers/unlink.c', 'vfs/handlers/handle_stat.c')]
# The libds sources are not under test. They are built once, without -Werror,
# as gcc warns about constant tables in mstr_digits.c.
LIBRARY_SOURCES = ['librt/libds/' + path for path in (
    'list.c', 'guid.c', 'hashtable.c', 'support/ds.c',
    'mstring/common/mstr_builder.c', 'mstring/core/merr.c', 'mstring/core/mstr_at.c',
    'mstring/core/mstr_clone.c', 'mstring/core/mstr_cmp.c', 'mstring/core/mstr_delete.c',
    'mstring/core/mstr_find.c', 'mstring/core/mstr_hash.c', 'mstring/core/mstr_join.c',
    'mstring/core/mstr_new.c', 'mstring/core/mstr_replace.c', 'mstring/core/mstr_split.c',
    'mstring/core/mstr_substr.c', 'mstring/unicode/mstr_conv.c',
    'mstring/unicode/mstr_digits.c', 'mstring/unicode/mstr_props.c',
    'mstring/unicode/mstr_unicode.c', 'mstring/path/mstr_path_basename.c',
    'mstring/path/mstr_path_change_extension.c', 'mstring/path/mstr_path_dirname.c',
    'mstring/path/mstr_path_join.c', 'mstring/path/mstr_path_new_u8.c',
    'mstring/path/mstr_path_tokens.c')]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-filed-storage-bind-') as directory:
    output = Path(directory)
    for service in ('process', 'file', 'device', 'storage', 'usb', 'contracts/filesystem',
                    'contracts/usbhost', 'contracts/storage'):
        subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                        '--service', str(ROOT / 'protocols' / (service + '.gr')),
                        '--out', directory, '--lang-c', '--client', '--server'], check=True)
    # storage_device.c needs only GetGrachtClient from the libc internal header.
    (output / 'internal').mkdir()
    (output / 'internal/_utils.h').write_text('''#pragma once
#include <gracht/client.h>
gracht_client_t* GetGrachtClient(void);
''')
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
             '-DVALI', '-DTESTING', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
             '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             '-Wno-unused-parameter', '-Wno-unused-but-set-parameter', '-Wno-sign-compare',
             '-Wno-unknown-warning-option', '-Wno-missing-field-initializers',
             '-Wno-unused-variable', '-Wno-implicit-fallthrough', '-Wno-type-limits',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-g',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I' + directory]
    # memfs packs directory entries back to back (__ReadDirectoryEntry), so the
    # second entry is not 8-byte aligned. That is outside this test.
    flags += ['-fno-sanitize=alignment']
    flags += ['-I' + str(ROOT / path) for path in
              ('testing/include', 'services/filed/include', 'librt/libos/include',
               'librt/libusb/include', 'librt/libddk/include', 'librt/libds/include',
               'librt/libgracht/include')]
    binary = str(output / 'test')

    library = []
    for index, path in enumerate(LIBRARY_SOURCES):
        library.append(str(output / f'library{index}.o'))
        subprocess.run([flag for flag in flags if flag != '-Werror'] +
                       ['-w', '-c', str(ROOT / path), '-o', library[-1]], check=True)

    def build(replacements=None, defines=()):
        sources = [str(ROOT / 'testing/filed_storage_bind_test.c')]
        sources += [(replacements or {}).get(path, str(ROOT / path)) for path in SOURCES]
        subprocess.run(flags + list(defines) + sources + library + ['-o', binary], check=True)

    # Two string leaks in the VFS are known and outside this test: the root
    # node name from __StatRootNode, and the name VFSNodeCreateChild clones
    # before VFSNodeChildNew clones it again. Any other leak fails the test.
    suppressions = output / 'lsan.supp'
    suppressions.write_text('leak:__StatRootNode\nleak:VFSNodeCreateChild\n')
    environment = dict(os.environ)
    environment['LSAN_OPTIONS'] = 'suppressions=' + str(suppressions)

    build()
    subprocess.run([binary], check=True, env=environment)
    if args.mutation_check:
        storage = FILED + 'storage/storage.c'
        filesystem = FILED + 'filesystem.c'
        defines = {storage: 'FILED_STORAGE_SOURCE', filesystem: 'FILED_FILESYSTEM_SOURCE'}
        list_insert = '    list_append(&g_disks, &storage->ListHeader);\n'
        after_query = ('    usched_mtx_lock(&g_diskLock);\n'
                       '    list_append(&g_disks, &storage->ListHeader);\n'
                       '    usched_mtx_unlock(&g_diskLock);\n'
                       '    // An unregister that arrived during the stat query means')
        for path, edits, reason in (
            (FILED + 'vfs/bind.c',
             (('    hashtable_remove(\n            &source->Mounts,', '    hashtable_get(\n            &source->Mounts,'),),
             'unbind leaves the source node listed as bound'),
            (FILED + 'vfs/handlers/unlink.c',
             (('    usched_rwlock_r_lock(&node->Lock);\n    oserr = VFSNodeEnsureLoaded(node);\n'
               '    usched_rwlock_r_unlock(&node->Lock);\n',
               '    usched_rwlock_w_lock(&node->Lock);\n    oserr = VFSNodeEnsureLoaded(node);\n'
               '    usched_rwlock_w_unlock(&node->Lock);\n'),),
             'unlink loads the node while holding its write lock'),
            (FILED + 'vfs/utils.c',
             (('        if (stats.Name != NULL &&\n', '        if (false &&\n'),),
             'loading a directory replaces nodes created before the load'),
            (FILED + 'vfs/vfs.c',
             (('return child1->Target == child2->Target ? 0 : 1;',
               'return (uint64_t)child1 != (uint64_t)child2;'),),
             'mount records compared by record address'),
            (filesystem, (('        __RemoveBind(fileSystem);\n', ''),),
             'unmount leaves the bind in place'),
            (filesystem, (('    if (createdDirectory) {\n        __RemoveMountDirectory',
                           '    if (false) {\n        __RemoveMountDirectory'),),
             'bind target created for the bind is left behind'),
            (filesystem, (('    if (createdDirectory) {\n        __RemoveMountDirectory',
                           '    if (true) {\n        __RemoveMountDirectory'),),
             'bind target that existed before is removed'),
            (filesystem, (('    if (oserr == OS_EEXISTS) {\n        createdDirectory = false;',
                           '    if (false) {\n        createdDirectory = false;'),),
             'existing bind target is not opened'),
            (filesystem, (('    if (fileSystem->BindPath != NULL) {\n        ERROR',
                           '    if (false) {\n        ERROR'),
                          ('    fileSystem->BindPath             = bindPath;\n',
                           '    fileSystem->BindPath             = bindPath;\n    fileSystem->BindPath = NULL; mstr_delete(bindPath);\n')),
             'bind not stored in the filesystem'),
            (filesystem, (('                mstr_delete(bindPath);\n                mstr_delete(label);\n',
                           '                mstr_delete(bindPath);\n'),),
             'default mount label leaked after a bind'),
            (storage, ((list_insert, ''),
                       ('    // An unregister that arrived during the stat query means', after_query)),
             'storage listed only after the stat query'),
            (storage, (('    if (unregisterPending) {\n        goto exit;',
                        '    if (false && unregisterPending) {\n        goto exit;'),),
             'disk mounted after an unregister during the stat query'),
            (storage, (('        if (list_remove(&g_disks, &storage->ListHeader) != 0) {\n'
                        '            ERROR("sys_storage_register_invocation storage for driver %u device %u was not in the disk list",\n'
                        '                  driverId, deviceId);\n'
                        '        }\n        usched_mtx_unlock(&g_diskLock);\n        VFSStorageDelete(storage);',
                        '        usched_mtx_unlock(&g_diskLock);\n        VFSStorageDelete(storage);'),),
             'failed stat query leaves the storage listed'),
            (FILED + 'storage/storage_device.c',
             (('    return __DeviceQueryStats(\n', '    return OS_EOK; (void)__DeviceQueryStats(\n'),),
             'stat query result ignored'),
        ):
            text = (ROOT / path).read_text()
            for before, after in edits:
                assert text.count(before) == 1, before
                text = text.replace(before, after)
            # A copy outside the source tree must find the private VFS headers.
            text = text.replace('#include "private.h"',
                                '#include "' + str(ROOT / FILED / 'vfs/private.h') + '"')
            text = text.replace('#include "../private.h"',
                                '#include "' + str(ROOT / FILED / 'vfs/private.h') + '"')
            mutant = output / ('mutant_' + Path(path).name)
            mutant.write_text(text)
            if path in defines:
                build(defines=[f'-D{defines[path]}="{mutant}"'])
            else:
                build(replacements={path: str(mutant)})
            if subprocess.run([binary], capture_output=True, env=environment).returncode == 0:
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: ' + reason, flush=True)
